// Editor shell: menu bar, docking layout, preview placeholder + transport, shortcut dispatch, overlays.
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/editor_shell.h>

#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/export/preset.h>
#include <ct/platform/image_io.h>
#include <ct/project/edit_ops.h>
#include <ct/project/serialize.h>
#include <ct/ui/editor_commands.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>

#include <imgui_internal.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace ct::ui {

using namespace ct::project;
using namespace metrics;

namespace {

// Deterministic speech-like peaks so the demo's audio lanes show waveforms before P06 provides real ones.
class DemoMediaProvider final : public TimelineMediaProvider {
public:
    explicit DemoMediaProvider(const Project* p) : m_project(p) {}
    MediaStatus Thumbnail(const Id&, TimeUs, ImTextureID&, ImVec2&, ImVec2&) override { return MediaStatus::Unavailable; }
    const float* PeaksMinMax(const Id& media, int stream, size_t* n, int* per_second, MediaStatus* status) override {
        const Media* m = m_project->FindMedia(media);
        if (!m || m->duration_us <= 0) {
            *n = 0;
            *status = MediaStatus::Unavailable;
            return nullptr;
        }
        const std::string key = media + "#" + std::to_string(stream);
        auto it = m_cache.find(key);
        if (it == m_cache.end()) {
            std::vector<float> v;
            const size_t count = static_cast<size_t>(m->duration_us / 10'000);
            v.resize(count * 2);
            uint32_t seed = 2166136261u;
            for (char ch : key) seed = (seed ^ static_cast<uint8_t>(ch)) * 16777619u;
            float env = 0;
            for (size_t i = 0; i < count; ++i) {
                seed = seed * 1664525u + 1013904223u;
                const float r = static_cast<float>(seed >> 8) / 16777216.0f;
                const double t = static_cast<double>(i) / 100.0;
                // Phrases of ~2-4 s with pauses; syllable modulation inside.
                const double phrase = std::sin(t * 0.9 + stream * 1.3) + 0.6 * std::sin(t * 2.3 + 0.7);
                const float target = phrase > -0.2 ? static_cast<float>(0.25 + 0.55 * std::fabs(std::sin(t * 7.0 + stream))) : 0.03f;
                env += (target - env) * 0.25f;
                const float a = std::min(1.0f, env * (0.55f + 0.45f * r));
                v[2 * i] = -a * (0.85f + 0.15f * r);
                v[2 * i + 1] = a;
            }
            it = m_cache.emplace(key, std::move(v)).first;
        }
        *n = it->second.size() / 2;
        *per_second = 100;
        *status = MediaStatus::Ready;
        return it->second.data();
    }

private:
    const Project* m_project;
    std::map<std::string, std::vector<float>> m_cache;
};

ImU32 ToU32(const ColorF& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }

} // namespace

EditorShell::EditorShell() {
    m_preview.run_action = [this](Action a) { Run(a); };
    for (const exportng::Preset& p : exportng::Preset::Builtin()) m_export_presets.emplace_back(p.id, p.name);
}
EditorShell::~EditorShell() = default;

void EditorShell::SetupImGui() {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDockingWithShift = false;
#if defined(CT_DEBUG)
    io.ConfigDebugHighlightIdConflicts = true;
#endif
    Theme::Apply(ImGui::GetStyle());
    Fonts::Load(*io.Fonts);
}

bool EditorShell::LoadProject(const char* path, std::string* error) {
    Project p;
    Err err;
    if (!project::Load(path, p, &err)) {
        if (error) *error = err.message;
        CT_LOG_WARN("editor: cannot load %s: %s", path, err.message.c_str());
        return false;
    }
    m_state.SetProject(std::move(p));
    m_state.pps = 40;
    m_state.scroll_us = 0;
    return true;
}

void EditorShell::EnableDemo() {
    m_demo_media = std::make_unique<DemoMediaProvider>(&m_state.project);
    SetMediaProvider(m_demo_media.get());
    m_media.on_import = [this] { m_toasts.Push("File dialogs arrive with the editor window (P09 9.2)", ToastKind::Info, {}, 4.0f); };
    // Show a populated state: the first screen clip selected, playhead a few seconds in.
    for (const Clip& c : m_state.project.clips)
        if (c.source == Clip::Source::Screen) {
            m_state.selection.Set(c.id);
            break;
        }
    m_state.SetPlayhead(4 * kUsPerSecond);
}

void EditorShell::SetMediaProvider(TimelineMediaProvider* p) {
    m_timeline.SetMediaProvider(p);
    m_media.SetMediaProvider(p);
}

bool EditorShell::WantsContinuousRedraw() const {
    return m_state.playback->Playing() || m_timeline.WantsRedraw() || m_preview.WantsRedraw() || !m_toasts.Empty() ||
           m_state.DragActive() || m_stats || m_props.WantsRedraw();
}

// --- actions ------------------------------------------------------------------------------------------------------

void EditorShell::DispatchShortcuts() {
    // A modal (unsaved changes, recovery, remove media, settings) owns the keyboard: Space/Delete/Ctrl+S must not act
    // on the document behind it. The popup stack is last frame's, which is what was on screen when the key went down.
    if (ImGui::GetTopMostPopupModal()) return;
    Action actions[8];
    const int n = Shortcuts::Poll(actions, 8);
    for (int i = 0; i < n; ++i) Run(actions[i]);
}

void EditorShell::Run(Action a) {
    EditorState& s = m_state;
    PlaybackControl& pb = *s.playback;
    const TimeUs frame = s.FrameUs();
    switch (a) {
    case Action::TogglePlay: pb.Playing() ? pb.Pause() : pb.Play(1.0); break;
    case Action::Forward: pb.Play(pb.Rate() >= 1 ? std::min(pb.Rate() * 2, 4.0) : 1.0); break;
    case Action::Reverse: pb.Play(pb.Rate() <= -1 ? std::max(pb.Rate() * 2, -4.0) : -1.0); break;
    case Action::ForwardSlow: pb.Play(0.5); break;
    case Action::ReverseSlow: pb.Play(-0.5); break;
    case Action::Stop: pb.Pause(); break;
    case Action::PrevFrame: pb.Pause(); pb.Seek(std::max<TimeUs>(0, s.Playhead() - frame)); break;
    case Action::NextFrame: pb.Pause(); pb.Seek(s.Playhead() + frame); break;
    case Action::Back1s: pb.Pause(); pb.Seek(std::max<TimeUs>(0, s.Playhead() - kUsPerSecond)); break;
    case Action::Forward1s: pb.Pause(); pb.Seek(s.Playhead() + kUsPerSecond); break;
    case Action::GoStart: pb.Seek(0); break;
    case Action::GoEnd: pb.Seek(s.project.Duration()); break;
    case Action::LoopIn: cmd::SetLoopIn(s); break;
    case Action::LoopOut: cmd::SetLoopOut(s); break;
    case Action::ClearLoopIn: cmd::ClearLoopIn(s); break;
    case Action::ClearLoopOut: cmd::ClearLoopOut(s); break;
    case Action::Split: cmd::SplitAtPlayhead(s); break;
    case Action::Delete: cmd::DeleteSelection(s, s.ripple); break;
    case Action::RippleDelete: cmd::DeleteSelection(s, true); break;
    case Action::Undo: s.Undo(); break;
    case Action::Redo: s.Redo(); break;
    case Action::Cut: cmd::CutSelection(s); break;
    case Action::Copy: cmd::CopySelection(s); break;
    case Action::Paste: cmd::PasteAtPlayhead(s); break;
    case Action::Duplicate: cmd::DuplicateSelection(s); break;
    case Action::SelectAll: cmd::SelectAll(s); break;
    case Action::AddMarker: cmd::AddMarkerAtPlayhead(s); break;
    case Action::AddCameraKey: cmd::AddCameraKeyAtPlayhead(s); break;
    case Action::RemoveCameraKey: cmd::RemoveNearestCameraKey(s); break;
    case Action::ZoomIn: m_timeline.ZoomBy(s, 1.25, -1); break;
    case Action::ZoomOut: m_timeline.ZoomBy(s, 1 / 1.25, -1); break;
    case Action::FitTimeline: m_timeline.Fit(s); break;
    case Action::NudgeLeft: cmd::NudgeSelection(s, -1); break;
    case Action::NudgeRight: cmd::NudgeSelection(s, 1); break;
    case Action::NudgeLeft10: cmd::NudgeSelection(s, -10); break;
    case Action::NudgeRight10: cmd::NudgeSelection(s, 10); break;
    case Action::ToggleRipple: s.ripple = !s.ripple; break;
    case Action::ToggleSnap: s.snap = !s.snap; break;
    case Action::FocusMedia: ImGui::SetWindowFocus(str::kPanelMedia); break;
    case Action::FocusPreview: ImGui::SetWindowFocus(str::kPanelPreview); break;
    case Action::FocusProperties: ImGui::SetWindowFocus(str::kPanelProperties); break;
    case Action::FocusTimeline: ImGui::SetWindowFocus(str::kPanelTimeline); break;
    case Action::ToggleStats: m_stats = !m_stats; break;
    case Action::CheatSheet: m_cheat_sheet = !m_cheat_sheet; break;
    default:
        if (on_action && on_action(a)) break;
        if (a == Action::Import && m_media.on_import) {
            m_media.on_import();
            break;
        }
        m_toasts.Push("Not available in this build yet", ToastKind::Info, {}, 3.0f);
        break;
    }
}

// --- menu bar -------------------------------------------------------------------------------------------------------

void EditorShell::DrawMenuBar() {
    EditorState& s = m_state;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Dp(10, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Dp(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(8, 8));
    if (ImGui::BeginMainMenuBar()) {
        // Brand mark.
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            dl->AddCircleFilled(ImVec2(p.x + Dp(9), p.y + h * 0.5f), Dp(6), col::record);
            dl->AddCircle(ImVec2(p.x + Dp(9), p.y + h * 0.5f), Dp(9), WithAlpha(col::record, 0.35f), 0, Dp(1.5f));
            ImGui::Dummy(ImVec2(Dp(22), h));
            ImGui::SameLine(0, Dp(6));
        }
        auto item = [&](const char* label, Action a, bool enabled = true, bool selected = false) {
            if (ImGui::MenuItem(label, Shortcuts::KeysFor(a), selected, enabled)) Run(a);
        };
        if (ImGui::BeginMenu(str::kMenuFile)) {
            item(str::kNew, Action::New);
            item(str::kOpen, Action::Open);
            if (ImGui::BeginMenu(str::kOpenRecent, !recent_files.empty() && on_open_recent)) {
                std::string pick;
                for (size_t i = 0; i < recent_files.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const std::string& f = recent_files[i];
                    const size_t slash = f.find_last_of("\\/");
                    const char* name = slash == std::string::npos ? f.c_str() : f.c_str() + slash + 1;
                    if (ImGui::MenuItem(name)) pick = f;
                    ItemTooltip(f.c_str());
                    ImGui::PopID();
                }
                ImGui::EndMenu();
                if (!pick.empty()) on_open_recent(pick);
            }
            ImGui::Separator();
            item(str::kImportMedia, Action::Import);
            if (ImGui::MenuItem(str::kImportRecording, nullptr, false, static_cast<bool>(on_import_recording))) on_import_recording();
            ImGui::Separator();
            item(str::kSave, Action::Save);
            item(str::kSaveAs, Action::SaveAs);
            item(str::kExportEllipsis, Action::Export);
            if (ImGui::MenuItem(str::kClose, nullptr, false, static_cast<bool>(on_close_project))) on_close_project();
            ImGui::Separator();
            if (ImGui::MenuItem(str::kExit) && m_hwnd) PostMessageW(static_cast<HWND>(m_hwnd), WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(str::kMenuEdit)) {
            char undo[96], redo[96];
            std::snprintf(undo, sizeof(undo), "%s %s", str::kUndo, s.undo.UndoLabel());
            std::snprintf(redo, sizeof(redo), "%s %s", str::kRedo, s.undo.RedoLabel());
            item(undo, Action::Undo, s.undo.CanUndo());
            item(redo, Action::Redo, s.undo.CanRedo());
            ImGui::Separator();
            const bool sel = !s.selection.clips.empty();
            item(str::kCut, Action::Cut, sel);
            item(str::kCopy, Action::Copy, sel);
            item(str::kPaste, Action::Paste, !s.clipboard.empty());
            item(str::kDuplicate, Action::Duplicate, sel);
            item(str::kDelete, Action::Delete, !s.selection.Empty());
            item(str::kSelectAll, Action::SelectAll);
            ImGui::Separator();
            if (ImGui::MenuItem(str::kPreferences, nullptr, false, static_cast<bool>(on_preferences))) on_preferences();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(str::kMenuView)) {
            const char* names[] = {str::kPanelMedia, str::kPanelPreview, str::kPanelProperties, str::kPanelTimeline};
            for (int i = 0; i < 4; ++i) ImGui::MenuItem(names[i], nullptr, &m_show_panels[i]);
            if (ImGui::MenuItem(str::kResetLayout)) m_reset_layout = true;
            ImGui::Separator();
            item(str::kFitTimeline, Action::FitTimeline);
            item(str::kZoomIn, Action::ZoomIn);
            item(str::kZoomOut, Action::ZoomOut);
            ImGui::Separator();
            item(str::kStatsOverlay, Action::ToggleStats, true, m_stats);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(str::kMenuTimeline)) {
            item(str::kSplit, Action::Split);
            item(str::kRippleDelete, Action::RippleDelete, !s.selection.clips.empty());
            item(str::kAddMarker, Action::AddMarker);
            if (ImGui::BeginMenu(str::kAddTrackMenu)) {
                if (ImGui::MenuItem(str::kVideoTrack)) cmd::AddTrack(s, Track::Kind::Video);
                if (ImGui::MenuItem(str::kAudioTrack)) cmd::AddTrack(s, Track::Kind::Audio);
                if (ImGui::MenuItem(str::kCalloutTrack)) cmd::AddTrack(s, Track::Kind::Annotations);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            item(str::kSnapping, Action::ToggleSnap, true, s.snap);
            item(str::kRippleMode, Action::ToggleRipple, true, s.ripple);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(str::kMenuCamera)) {
            const bool aut = s.project.camera.mode == Camera::Mode::Auto;
            if (ImGui::MenuItem(str::kCameraAuto, nullptr, aut)) cmd::SetCameraMode(s, Camera::Mode::Auto);
            if (ImGui::MenuItem(str::kCameraManual, nullptr, !aut)) cmd::SetCameraMode(s, Camera::Mode::Manual);
            if (ImGui::MenuItem(str::kRegenerate, nullptr, false, static_cast<bool>(m_props.on_regenerate))) m_props.on_regenerate(s);
            ImGui::Separator();
            item(str::kAddKey, Action::AddCameraKey);
            if (ImGui::MenuItem(str::kClearKeys, nullptr, false, !s.project.camera.keys.empty())) {
                s.Edit(str::kUndoCamera, [](Project& p) {
                    p.camera.keys.clear();
                    return true;
                });
                s.PruneSelection();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(str::kMenuExport)) {
            item(str::kExportEllipsis, Action::Export);
            ImGui::Separator();
            DrawExportPresetItems();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(str::kMenuHelp)) {
            item(str::kShortcuts, Action::CheatSheet, true, m_cheat_sheet);
            if (ImGui::MenuItem(str::kOpenLogs, nullptr, false, static_cast<bool>(on_open_logs))) on_open_logs();
            if (ImGui::MenuItem(str::kAbout, nullptr, false, static_cast<bool>(on_about))) on_about();
            ImGui::EndMenu();
        }

        // Centre: project name with dirty dot.
        {
            const char* name = s.project.name.empty() ? str::kUntitled : s.project.name.c_str();
            const float w = ImGui::CalcTextSize(name).x + (s.IsDirty() ? Dp(14) : 0);
            const float cx = (ImGui::GetWindowWidth() - w) * 0.5f;
            if (cx > ImGui::GetCursorPosX() + Dp(20)) {
                ImGui::SameLine(cx);
                ImGui::PushStyleColor(ImGuiCol_Text, ToVec4(col::text_dim));
                ImGui::TextUnformatted(name);
                ImGui::PopStyleColor();
                if (s.IsDirty()) {
                    const ImVec2 r = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(r.x + Dp(8), (ImGui::GetItemRectMin().y + r.y) * 0.5f), Dp(3), col::text_dim);
                }
            }
        }
        // Right: Record | Export (opens the dialog) + chevron (preset list).
        const float bh = Dp(26);
        const float rec_w = Dp(92), exp_w = Dp(72), chev_w = Dp(26);
        ImGui::SameLine(ImGui::GetWindowWidth() - rec_w - exp_w - chev_w - Dp(22));
        ImGui::SetCursorPosY((ImGui::GetFrameHeight() - bh) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Dp(10, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusControl));
        char rec[48];
        std::snprintf(rec, sizeof(rec), "%s  %s", ICON_RECORD, str::kRecord);
        if (Button(rec, ButtonKind::Record, ImVec2(rec_w, bh))) Run(Action::OpenRecorder);
        ItemTooltip(str::kRecord, Shortcuts::KeysFor(Action::OpenRecorder));
        ImGui::SameLine(0, Dp(8));
        ImGui::SetCursorPosY((ImGui::GetFrameHeight() - bh) * 0.5f);
        if (Button(str::kExport, ButtonKind::Primary, ImVec2(exp_w, bh))) Run(Action::Export);
        ItemTooltip(str::kExport, Shortcuts::KeysFor(Action::Export));
        ImGui::SameLine(0, Dp(2));
        ImGui::SetCursorPosY((ImGui::GetFrameHeight() - bh) * 0.5f);
        char chev[32];
        std::snprintf(chev, sizeof(chev), "%s##exportpresets", ICON_CHEVRON_DOWN);
        if (Button(chev, ButtonKind::Primary, ImVec2(chev_w, bh))) ImGui::OpenPopup("##exportpresetmenu");
        ItemTooltip(str::kExportPresetsMenu);
        ImGui::PopStyleVar(2);
        if (ImGui::BeginPopup("##exportpresetmenu")) {
            if (ImGui::MenuItem(str::kExportEllipsis, Shortcuts::KeysFor(Action::Export))) Run(Action::Export);
            ImGui::Separator();
            DrawExportPresetItems();
            ImGui::EndPopup();
        }
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar(4);
}

void EditorShell::DrawExportPresetItems() {
    std::string pick;
    for (const auto& [id, name] : m_export_presets) {
        ImGui::PushID(id.c_str());
        if (ImGui::MenuItem(name.c_str(), nullptr, false, static_cast<bool>(on_export_preset))) pick = id;
        ImGui::PopID();
    }
    if (!pick.empty()) on_export_preset(pick);
}

// --- layout ---------------------------------------------------------------------------------------------------------

void EditorShell::BuildLayout(unsigned int id) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, vp->WorkSize);
    const float W = std::max(vp->WorkSize.x, 1.0f), H = std::max(vp->WorkSize.y, 1.0f);
    ImGuiID top = 0, bottom = 0, left = 0, rest = 0, right = 0, centre = 0;
    ImGui::DockBuilderSplitNode(id, ImGuiDir_Down, std::min(0.6f, Dp(kTimelineHeight) / H), &bottom, &top);
    ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, std::min(0.35f, Dp(kMediaWidth) / W), &left, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, std::min(0.4f, Dp(kPropertiesWidth) / std::max(1.0f, W - Dp(kMediaWidth))), &right, &centre);
    ImGui::DockBuilderDockWindow(str::kPanelMedia, left);
    ImGui::DockBuilderDockWindow(str::kPanelProperties, right);
    ImGui::DockBuilderDockWindow(str::kPanelPreview, centre);
    ImGui::DockBuilderDockWindow(str::kPanelTimeline, bottom);
    ImGui::DockBuilderFinish(id);
}

void EditorShell::DrawStats() {
    if (!m_stats) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - Dp(16), vp->WorkPos.y + Dp(16)), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.9f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##stats", nullptr, flags)) {
        FontScope f(FontRole::Mono);
        ImGui::Text("frame   %6.2f ms", m_frame_ms);
        if (status.gpu_ms >= 0) ImGui::Text("gpu     %6.2f ms", status.gpu_ms);
        else ImGui::Text("gpu          -");
        ImGui::Text("clips   %d drawn / %zu", m_timeline.ClipsDrawn(), m_state.project.clips.size());
        ImGui::Text("undo    %zu (%zu KB)", m_state.undo.Entries(), m_state.undo.Bytes() / 1024);
        if (draw_stats_extra) draw_stats_extra();
    }
    ImGui::End();
}

void EditorShell::UpdateTitle() {
    if (!m_hwnd) return;
    std::string t = m_state.project.name.empty() ? str::kUntitled : m_state.project.name;
    if (m_state.IsDirty()) t += "*";
    t += " - ";
    t += str::kAppName;
    if (t == m_title) return;
    m_title = t;
    SetWindowTextW(static_cast<HWND>(m_hwnd), Utf8ToWide(t).c_str());
}

// --- frame ------------------------------------------------------------------------------------------------------------

void EditorShell::Frame() {
    const double now = ImGui::GetTime();
    m_frame_ms = (now - m_last_frame_s) * 1000.0;
    m_last_frame_s = now;
    m_state.playback->Tick(now);
    DispatchShortcuts();
    UpdateTitle();

    DrawMenuBar();
    switch (DrawStatusBar(m_state, status)) {
    case StatusBarClick::Warnings:
        if (on_open_logs) on_open_logs();
        break;
    case StatusBarClick::Export:
        if (on_export_restore) on_export_restore();
        break;
    case StatusBarClick::None: break;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID dock_id = ImGui::GetID("##editordock");
    // With a persisted layout the ini restored the dock nodes: build only when there is none (or on Reset layout).
    if (m_reset_layout || !ImGui::DockBuilderGetNode(dock_id) || (!persist_layout && !m_layout_built)) {
        BuildLayout(dock_id);
        m_reset_layout = false;
        for (bool& b : m_show_panels) b = true;
    }
    m_layout_built = true;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg0));
    ImGui::DockSpaceOverViewport(dock_id, vp, ImGuiDockNodeFlags_None);
    ImGui::PopStyleColor();

    const ImGuiWindowFlags panel = ImGuiWindowFlags_NoCollapse;
    // Panels are toggled from the View menu: no close buttons or window menus on the dock tabs.
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoWindowMenuButton;
    if (m_show_panels[0]) {
        ImGui::SetNextWindowClass(&wc);
        if (ImGui::Begin(str::kPanelMedia, nullptr, panel)) {
            m_banners.Draw(BannerPanel::Media);
            m_media.Draw(m_state);
        }
        ImGui::End();
    }
    if (m_show_panels[1]) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg0));
        ImGui::SetNextWindowClass(&wc);
        const bool open = ImGui::Begin(str::kPanelPreview, nullptr, panel | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (open) {
            m_banners.Draw(BannerPanel::Preview);
            m_preview.Draw(m_state);
        }
        ImGui::End();
    }
    if (m_show_panels[2]) {
        ImGui::SetNextWindowClass(&wc);
        if (ImGui::Begin(str::kPanelProperties, nullptr, panel)) {
            m_banners.Draw(BannerPanel::Properties);
            m_props.Draw(m_state);
        }
        ImGui::End();
    }
    if (m_show_panels[3]) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(8, 6));
        ImGui::SetNextWindowClass(&wc);
        const bool open = ImGui::Begin(str::kPanelTimeline, nullptr, panel | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (open) {
            m_banners.Draw(BannerPanel::Timeline);
            m_timeline.DrawToolbar(m_state);
            ImGui::Dummy(Dp(0, 2));
            m_timeline.Draw(m_state);
        }
        ImGui::End();
    }
    DrawShortcutSheet(&m_cheat_sheet);
    DrawStats();
    m_toasts.Draw(kStatusBarHeight);
}

// --- screenshot ---------------------------------------------------------------------------------------------------------

bool SaveWindowScreenshot(void* hwnd_ptr, const char* path) {
    HWND hwnd = static_cast<HWND>(hwnd_ptr);
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        // PW_CLIENTONLY | PW_RENDERFULLCONTENT (flip-model swapchains need the full-content flag).
        ok = PrintWindow(hwnd, mdc, PW_CLIENTONLY | 0x00000002) != FALSE;
        if (!ok) ok = BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY) != FALSE;
        SelectObject(mdc, old);
        if (ok) {
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                rgba[i * 4 + 0] = src[i * 4 + 2];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 0];
                rgba[i * 4 + 3] = 255;
            }
            ok = platform::ImageIo::WritePngRgba(path, rgba.data(), w, h, w * 4);
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    if (ok) CT_LOG_INFO("screenshot: %s (%dx%d)", path, w, h);
    else CT_LOG_WARN("screenshot failed: %s", path);
    return ok;
}

} // namespace ct::ui
