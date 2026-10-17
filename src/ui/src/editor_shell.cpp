// Editor shell: menu bar, docking layout, preview placeholder + transport, shortcut dispatch, overlays.
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/editor_shell.h>

#include <ct/core/log.h>
#include <ct/core/strings.h>
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

EditorShell::EditorShell() {}
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
    return m_state.playback->Playing() || m_timeline.WantsRedraw() || !m_toasts.Empty() || m_state.DragActive() || m_stats;
}

// --- actions ------------------------------------------------------------------------------------------------------

void EditorShell::DispatchShortcuts() {
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
            ImGui::BeginDisabled();
            ImGui::MenuItem(str::kOpenRecent);
            ImGui::EndDisabled();
            ImGui::Separator();
            item(str::kImportMedia, Action::Import);
            if (ImGui::MenuItem(str::kImportRecording)) Run(Action::Import);
            ImGui::Separator();
            item(str::kSave, Action::Save);
            item(str::kSaveAs, Action::SaveAs);
            item(str::kExportEllipsis, Action::Export);
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
            ImGui::BeginDisabled();
            ImGui::MenuItem(str::kPreferences);
            ImGui::EndDisabled();
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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(str::kMenuHelp)) {
            item(str::kShortcuts, Action::CheatSheet, true, m_cheat_sheet);
            ImGui::BeginDisabled();
            ImGui::MenuItem(str::kOpenLogs);
            ImGui::MenuItem(str::kAbout);
            ImGui::EndDisabled();
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
        // Right: Record | Export.
        const float bh = Dp(26);
        const float rec_w = Dp(92), exp_w = Dp(96);
        ImGui::SameLine(ImGui::GetWindowWidth() - rec_w - exp_w - Dp(20));
        ImGui::SetCursorPosY((ImGui::GetFrameHeight() - bh) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Dp(10, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Dp(kRadiusControl));
        char rec[48];
        std::snprintf(rec, sizeof(rec), "%s  %s", ICON_RECORD, str::kRecord);
        if (Button(rec, ButtonKind::Record, ImVec2(rec_w, bh))) Run(Action::OpenRecorder);
        ItemTooltip(str::kRecord, Shortcuts::KeysFor(Action::OpenRecorder));
        ImGui::SameLine(0, Dp(8));
        ImGui::SetCursorPosY((ImGui::GetFrameHeight() - bh) * 0.5f);
        char exp[48];
        std::snprintf(exp, sizeof(exp), "%s  %s", str::kExport, ICON_CHEVRON_DOWN);
        if (Button(exp, ButtonKind::Primary, ImVec2(exp_w, bh))) Run(Action::Export);
        ItemTooltip(str::kExport, Shortcuts::KeysFor(Action::Export));
        ImGui::PopStyleVar(2);
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar(4);
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

// --- preview ----------------------------------------------------------------------------------------------------------

void EditorShell::DrawPreviewPlaceholder(ImVec2 mn, ImVec2 mx) {
    const Canvas& cv = m_state.project.canvas;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = Dp(kRadiusCanvas);
    // Canvas background (gradient along the angle).
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(mn, mx, ToU32(cv.background.color_a), r);
    if (cv.background.kind == Background::Kind::Gradient) {
        const float ang = cv.background.angle_deg * 3.14159265f / 180.0f;
        const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
        const float half = 0.5f * (std::fabs((mx.x - mn.x) * std::sin(ang)) + std::fabs((mx.y - mn.y) * std::cos(ang)));
        const ImVec2 d(std::sin(ang) * half, -std::cos(ang) * half);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, c - d, c + d, ToU32(cv.background.color_a), ToU32(cv.background.color_b));
    }
    // Screen layer inset by the padding, with the canvas corner radius and shadow.
    const float scale = (mx.x - mn.x) / std::max(1, cv.width);
    const float pad = cv.padding_px * scale;
    const ImVec2 sa(mn.x + pad, mn.y + pad), sb(mx.x - pad, mx.y - pad);
    if (sb.x > sa.x + 8 && sb.y > sa.y + 8) {
        const float sr = cv.corner_radius_px * scale;
        if (cv.shadow.enabled) {
            const float blur = std::max(2.0f, cv.shadow.blur_px * scale);
            const float off = cv.shadow.offset_y_px * scale;
            for (int i = 6; i >= 1; --i) {
                const float g = blur * i / 6.0f;
                dl->AddRectFilled(ImVec2(sa.x - g * 0.5f, sa.y - g * 0.5f + off), ImVec2(sb.x + g * 0.5f, sb.y + g * 0.5f + off),
                                  Rgb(0x000000, static_cast<uint32_t>(cv.shadow.opacity * 255 / 10)), sr + g);
            }
        }
        dl->AddRectFilled(sa, sb, Rgb(0x0B0D11), sr);
        // A faux window chrome so the placeholder reads as a screen recording.
        const float bar = std::max(6.0f, (sb.y - sa.y) * 0.06f);
        dl->AddRectFilled(sa, ImVec2(sb.x, sa.y + bar), Rgb(0x1A1E26), sr, ImDrawFlags_RoundTop);
        for (int i = 0; i < 3; ++i)
            dl->AddCircleFilled(ImVec2(sa.x + bar * (0.8f + 0.7f * i), sa.y + bar * 0.5f), bar * 0.18f,
                                i == 0 ? Rgb(0xFF5F57) : i == 1 ? Rgb(0xFEBC2E) : Rgb(0x28C840));
        const ImVec2 body(sa.x, sa.y + bar);
        const float bw = sb.x - sa.x;
        dl->AddRectFilled(ImVec2(body.x + bw * 0.04f, body.y + bar * 0.9f), ImVec2(body.x + bw * 0.24f, sb.y - bar * 0.9f), Rgb(0x151922), Dp(4));
        for (int i = 0; i < 4; ++i) {
            const float y = body.y + bar * (1.2f + 1.1f * i);
            if (y + bar * 0.4f > sb.y - bar) break;
            dl->AddRectFilled(ImVec2(body.x + bw * 0.28f, y), ImVec2(body.x + bw * (0.85f - 0.12f * (i % 3)), y + bar * 0.35f), Rgb(0x1C212B), Dp(3));
        }
        // Caption.
        const char* msg = str::kPreviewPending;
        FontScope f(FontRole::Label);
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        const ImVec2 tp((sa.x + sb.x - ts.x) * 0.5f, sb.y - bar * 1.6f - ts.y);
        if (ts.x < bw - Dp(16)) {
            dl->AddRectFilled(tp - Dp(10, 5), tp + ts + Dp(10, 5), WithAlpha(col::bg0, 0.8f), Dp(999));
            dl->AddText(tp, col::text_dim, msg);
        }
    }
    if (m_safe_areas) {
        const ImVec2 sz = mx - mn;
        dl->AddRect(mn + sz * 0.05f, mx - sz * 0.05f, WithAlpha(col::text, 0.35f));
        dl->AddRect(mn + sz * 0.10f, mx - sz * 0.10f, WithAlpha(col::text, 0.25f));
    }
}

void EditorShell::DrawTransport() {
    EditorState& s = m_state;
    PlaybackControl& pb = *s.playback;
    const float h = Dp(kControlDefault);
    const float total_w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Dp(12));
    if (IconButton("start", ICON_PREVIOUS, "Go to start", false, nullptr, ImVec2(h, h))) Run(Action::GoStart);
    ImGui::SameLine(0, Dp(2));
    if (IconButton("prev", ICON_CHEVRON_LEFT, "Previous frame", false, nullptr, ImVec2(h, h))) Run(Action::PrevFrame);
    ImGui::SameLine(0, Dp(2));
    {
        // Primary play button: filled accent circle.
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("play", ImVec2(h + Dp(4), h));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) Run(Action::TogglePlay);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + (h + Dp(4)) * 0.5f, p.y + h * 0.5f);
        dl->AddCircleFilled(c, h * 0.5f, hov ? col::accent_hover : col::accent, 32);
        FontScope f(FontRole::Icon);
        const char* icon = pb.Playing() ? ICON_PAUSE : ICON_PLAY;
        const ImVec2 ts = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(std::floor(c.x - ts.x * 0.5f + (pb.Playing() ? 0 : Dp(1))), std::floor(c.y - ts.y * 0.5f)), Rgb(0xFFFFFF), icon);
        ItemTooltip(pb.Playing() ? "Pause" : "Play", "Space");
    }
    ImGui::SameLine(0, Dp(2));
    if (IconButton("next", ICON_CHEVRON_RIGHT, "Next frame", false, nullptr, ImVec2(h, h))) Run(Action::NextFrame);
    ImGui::SameLine(0, Dp(2));
    if (IconButton("end", ICON_NEXT, "Go to end", false, nullptr, ImVec2(h, h))) Run(Action::GoEnd);
    ImGui::SameLine(0, Dp(16));
    {
        // Timecode "00:01:12.03 / 00:03:05.00" (current bright, total faint), vertically centred on the buttons.
        char a[32], b[32];
        const auto& cv = s.project.canvas;
        FormatTimecode(a, sizeof(a), s.Playhead(), cv.fps_num, cv.fps_den);
        std::snprintf(b, sizeof(b), "  /  ");
        const size_t bl = std::strlen(b);
        FormatTimecode(b + bl, sizeof(b) - bl, s.project.Duration(), cv.fps_num, cv.fps_den);
        ImFont* mono = Fonts::Get(FontRole::Mono);
        const float fs = Dp(13);
        const ImVec2 sa = mono ? mono->CalcTextSizeA(fs, FLT_MAX, 0, a) : ImVec2(0, 0);
        const ImVec2 sb = mono ? mono->CalcTextSizeA(fs, FLT_MAX, 0, b) : ImVec2(0, 0);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float ty = std::floor(p.y + (h - sa.y) * 0.5f);
        dl->AddText(mono, fs, ImVec2(p.x, ty), col::text, a);
        dl->AddText(mono, fs, ImVec2(p.x + sa.x, ty), col::text_faint, b);
        ImGui::Dummy(ImVec2(sa.x + sb.x, h));
    }
    if (pb.Rate() != 0 && pb.Rate() != 1) {
        ImGui::SameLine(0, Dp(10));
        char r[16];
        std::snprintf(r, sizeof(r), "%.4g\xC3\x97", pb.Rate());
        ImGui::TextColored(ToVec4(col::warn), "%s", r);
    }
    // Right side: fit, loop, safe areas, volume.
    const float right_w = Dp(84) + h * 3 + Dp(24);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Dp(8), total_w - right_w));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY());
    static const char* const kFit[] = {"Fit", "50 %", "100 %", "200 %"};
    char fit[32];
    std::snprintf(fit, sizeof(fit), "%s  %s", kFit[m_fit_mode], ICON_CHEVRON_DOWN);
    if (Button(fit, ButtonKind::Normal, ImVec2(Dp(84), h))) ImGui::OpenPopup("##fitmenu");
    if (BeginPopover("##fitmenu")) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(kFit[i], m_fit_mode == i)) m_fit_mode = i;
        EndPopover();
    }
    ImGui::SameLine(0, Dp(4));
    const bool loop = s.project.ui.loop.has_value();
    if (IconButton("loop", ICON_REPEAT, str::kLoop, loop, nullptr, ImVec2(h, h))) {
        if (loop) s.project.ui.loop.reset();
        else s.project.ui.loop = std::make_pair(TimeUs(0), s.project.Duration());
        s.Touch();
    }
    ImGui::SameLine(0, Dp(2));
    if (IconButton("safe", ICON_FULLSCREEN, str::kSafeAreas, m_safe_areas, nullptr, ImVec2(h, h))) m_safe_areas = !m_safe_areas;
    ImGui::SameLine(0, Dp(2));
    IconButton("vol", ICON_VOLUME, "Volume", false, nullptr, ImVec2(h, h));
}

void EditorShell::DrawPreview() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float transport_h = Dp(48);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area(avail.x, std::max(1.0f, avail.y - transport_h));
    const Canvas& cv = m_state.project.canvas;
    const float margin = Dp(kPreviewMargin);
    const float aw = std::max(1.0f, area.x - margin * 2), ah = std::max(1.0f, area.y - margin * 2);
    const float aspect = static_cast<float>(std::max(1, cv.width)) / static_cast<float>(std::max(1, cv.height));
    float w = aw, h = aw / aspect;
    if (h > ah) {
        h = ah;
        w = ah * aspect;
    }
    if (m_fit_mode > 0) {
        const float z = m_fit_mode == 1 ? 0.5f : m_fit_mode == 2 ? 1.0f : 2.0f;
        w = cv.width * z;
        h = cv.height * z;
    }
    const ImVec2 mn(std::floor(origin.x + (area.x - w) * 0.5f), std::floor(origin.y + (area.y - h) * 0.5f));
    const ImVec2 mx(mn.x + w, mn.y + h);
    ImGui::GetWindowDrawList()->PushClipRect(origin, origin + area, true);
    if (draw_preview) draw_preview(mn, mx);
    else DrawPreviewPlaceholder(mn, mx);
    ImGui::GetWindowDrawList()->PopClipRect();
    ImGui::Dummy(area);
    // Transport bar on a slightly darker strip.
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(tp, ImVec2(tp.x + avail.x, tp.y), col::border);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(8));
    DrawTransport();
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
        ImGui::Text("gpu     %s", status.gpu_ms >= 0 ? "" : "-");
        ImGui::Text("clips   %d drawn / %zu", m_timeline.ClipsDrawn(), m_state.project.clips.size());
        ImGui::Text("undo    %zu (%zu KB)", m_state.undo.Entries(), m_state.undo.Bytes() / 1024);
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
    if (DrawStatusBar(m_state, status)) {
        if (on_action) on_action(Action::None);
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID dock_id = ImGui::GetID("##editordock");
    if (!m_layout_built || m_reset_layout || !ImGui::DockBuilderGetNode(dock_id)) {
        BuildLayout(dock_id);
        m_layout_built = true;
        m_reset_layout = false;
        for (bool& b : m_show_panels) b = true;
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg0));
    ImGui::DockSpaceOverViewport(dock_id, vp, ImGuiDockNodeFlags_None);
    ImGui::PopStyleColor();

    const ImGuiWindowFlags panel = ImGuiWindowFlags_NoCollapse;
    // Panels are toggled from the View menu: no close buttons or window menus on the dock tabs.
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoWindowMenuButton;
    if (m_show_panels[0]) {
        ImGui::SetNextWindowClass(&wc);
        if (ImGui::Begin(str::kPanelMedia, nullptr, panel)) m_media.Draw(m_state);
        ImGui::End();
    }
    if (m_show_panels[1]) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg0));
        ImGui::SetNextWindowClass(&wc);
        const bool open = ImGui::Begin(str::kPanelPreview, nullptr, panel | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (open) DrawPreview();
        ImGui::End();
    }
    if (m_show_panels[2]) {
        ImGui::SetNextWindowClass(&wc);
        if (ImGui::Begin(str::kPanelProperties, nullptr, panel)) m_props.Draw(m_state);
        ImGui::End();
    }
    if (m_show_panels[3]) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(8, 6));
        ImGui::SetNextWindowClass(&wc);
        const bool open = ImGui::Begin(str::kPanelTimeline, nullptr, panel | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (open) {
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
