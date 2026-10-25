#define CT_LOG_CATEGORY "ui"
#include <ct/ui/panels/export_dialog.h>

#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/media/encoder_caps.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>
#include <ct/ui/recorder_errors.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace ct::ui {

using exportng::Preset;
using Microsoft::WRL::ComPtr;

namespace {

struct SizeChoice {
    const char* label;
    int w, h;
};
constexpr SizeChoice kSizes[] = {{"Canvas", 0, 0}, {"1440p", 2560, 1440}, {"1080p", 1920, 1080}, {"720p", 1280, 720}, {"Custom", -1, -1}};
constexpr int kCustomSize = 4;
constexpr int kFpsValues[] = {0, 60, 30, 24};
constexpr const char* kFpsLabels[] = {"Canvas", "60", "30", "24"};
constexpr const char* kContainers[] = {"MP4 video", "Animated GIF", "PNG still", "M4A audio"};
constexpr const char* kRc[] = {"Quality", "Variable bitrate", "Constant bitrate"};
constexpr uint32_t kAacRates[] = {96, 128, 160, 192};   // what the Media Foundation AAC encoder accepts
constexpr const char* kAacLabels[] = {"96 kbps", "128 kbps", "160 kbps", "192 kbps"};

std::wstring Wide(const std::string& s) { return Utf8ToWide(s); }

} // namespace

ExportDialog::ExportDialog() : m_alive(std::make_shared<int>(0)) {}

ExportDialog::~ExportDialog() {
    m_alive.reset();
    if (m_job) {
        m_job->Cancel();
        m_job->Join(INFINITE);
    }
}

bool ExportDialog::Busy() const { return m_job && !m_job->GetProgress().finished.load(std::memory_order_acquire); }
double ExportDialog::Fraction() const { return m_job ? m_job->GetProgress().fraction.load() : 0.0; }
bool ExportDialog::WantsRedraw() const { return Busy() || (m_open && !m_caps_known); }

void ExportDialog::CancelExport() {
    if (m_job) m_job->Cancel();
}

std::string ExportDialog::SanitizeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const bool bad = c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
                         static_cast<unsigned char>(c) < 32;
        out.push_back(bad ? '_' : c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "Untitled";
    return out;
}

std::string ExportDialog::DefaultFileName(const std::string& project_name, Preset::Container c) {
    return SanitizeFileName(project_name.empty() ? std::string("Untitled") : project_name) + exportng::ContainerExtension(c);
}

std::string ExportDialog::MakePresetId(const std::string& name) {
    std::string id;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) id.push_back(static_cast<char>(std::tolower(u)));
        else if (!id.empty() && id.back() != '-') id.push_back('-');
        if (id.size() >= 48) break;
    }
    while (!id.empty() && id.back() == '-') id.pop_back();
    if (id.empty()) id = "preset";
    return "user-" + id;
}

void ExportDialog::SelectPreset(const Preset& p) {
    m_preset = p;
    m_last_preset_id = p.id;
    m_size_choice = kCustomSize;
    for (int i = 0; i < kCustomSize; ++i)
        if (kSizes[i].w == p.video.width && kSizes[i].h == p.video.height) m_size_choice = i;
    m_fps_choice = 0;
    for (int i = 0; i < 4; ++i)
        if (kFpsValues[i] == p.video.fps_num && (p.video.fps_den == 1 || kFpsValues[i] == 0)) m_fps_choice = i;
    FixExtension();
}

void ExportDialog::FixExtension() {
    if (m_dest.empty()) return;
    const std::string want = exportng::ContainerExtension(m_preset.container);
    std::string ext = ToLowerAscii(PathExt(m_dest));
    if (ext == want || (m_preset.container == Preset::Container::Png && (ext == ".jpg" || ext == ".jpeg"))) return;
    if (!ext.empty()) m_dest.resize(m_dest.size() - ext.size());
    m_dest += want;
}

void ExportDialog::Open(const project::Project& project, TimeUs playhead_us, const Host& host, const std::string& preset_id) {
    if (Busy()) {
        m_minimized = false;
        return;
    }
    m_job.reset();
    m_host = host;
    m_project = project;   // snapshot
    m_playhead = playhead_us;
    m_open = true;
    m_minimized = false;
    m_error.clear();
    m_banner.Clear();
    m_range = 0;
    m_builtin = Preset::Builtin();
    const std::string want = !preset_id.empty() ? preset_id : m_last_preset_id;
    const Preset* p = Preset::FindBuiltin(want);
    SelectPreset(p ? *p : m_builtin.front());
    m_selected = 0;
    for (size_t i = 0; i < m_builtin.size(); ++i)
        if (m_builtin[i].id == m_preset.id) m_selected = static_cast<int>(i);
    std::string dir = platform::Paths::DefaultRecordingsDir();
    if (dir.empty()) dir = fs::Absolute(".");
    m_dest = PathJoin(dir, DefaultFileName(m_project.name, m_preset.container));
    // Slow parts on the IO pool: user presets (disk) and the HEVC encoder check (MFTEnumEx).
    m_caps_known = false;
    std::weak_ptr<int> alive = m_alive;
    Async::Run([this, alive, want] {
        std::vector<Preset> user = Preset::LoadUser();
        const bool hevc = media::EncoderCaps::HasHardware(media::VideoCodec::Hevc);
        Async::PostToUi([this, alive, want, user = std::move(user), hevc]() mutable {
            if (alive.expired()) return;
            m_user = std::move(user);
            m_hevc_hw = hevc;
            m_caps_known = true;
            for (size_t i = 0; i < m_user.size(); ++i)
                if (m_user[i].id == want) {
                    m_selected = static_cast<int>(m_builtin.size() + i);
                    SelectPreset(m_user[i]);
                }
            if (m_host.request_redraw) m_host.request_redraw();
        });
    });
}

TimeUs ExportDialog::RangeDuration() const {
    if (m_preset.container == Preset::Container::Png) return 0;
    if (m_range == 1 && m_project.ui.loop) return std::max<TimeUs>(m_project.ui.loop->second - m_project.ui.loop->first, 0);
    return m_project.Duration();
}

bool ExportDialog::BrowseDestination() {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf())))) return false;
    const char* ext = exportng::ContainerExtension(m_preset.container);
    COMDLG_FILTERSPEC spec[2] = {};
    std::wstring name, pattern;
    switch (m_preset.container) {
    case Preset::Container::Mp4: name = L"MP4 video"; pattern = L"*.mp4"; break;
    case Preset::Container::Gif: name = L"Animated GIF"; pattern = L"*.gif"; break;
    case Preset::Container::Png: name = L"PNG / JPEG image"; pattern = L"*.png;*.jpg;*.jpeg"; break;
    case Preset::Container::M4a: name = L"M4A audio"; pattern = L"*.m4a"; break;
    }
    spec[0] = {name.c_str(), pattern.c_str()};
    spec[1] = {L"All files", L"*.*"};
    dlg->SetFileTypes(2, spec);
    dlg->SetDefaultExtension(Wide(ext + 1).c_str());
    dlg->SetTitle(Wide(str::kExportSaveDialogTitle).c_str());
    dlg->SetOptions(FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const std::string folder(PathParent(m_dest));
    if (!folder.empty() && fs::IsDirectory(folder)) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(Wide(folder).c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf()))))
            dlg->SetFolder(item.Get());
    }
    dlg->SetFileName(Wide(std::string(PathName(m_dest))).c_str());
    if (FAILED(dlg->Show(static_cast<HWND>(m_host.owner_hwnd)))) return false;   // cancelled
    ComPtr<IShellItem> result;
    if (FAILED(dlg->GetResult(result.GetAddressOf()))) return false;
    PWSTR path = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
    m_dest = WideToUtf8(path);
    CoTaskMemFree(path);
    return true;
}

void ExportDialog::StartExport() {
    m_error.clear();
    if (m_dest.empty()) {
        m_error = str::kExportNoDestination;
        return;
    }
    if (m_preset.container != Preset::Container::Png && RangeDuration() <= 0) {
        m_error = str::kExportEmpty;
        return;
    }
    if (!m_host.device || !m_host.render_context || !m_host.audio_devices) return;
    fs::CreateDirectories(PathParent(m_dest));
    exportng::ExportRequest req;
    req.project_snapshot = m_project;
    req.preset = m_preset;
    req.output_path = m_dest;
    req.faults = m_faults;
    if (m_preset.container == Preset::Container::Png) {
        req.range_start = m_playhead;
    } else if (m_range == 1 && m_project.ui.loop) {
        req.range_start = m_project.ui.loop->first;
        req.range_end = m_project.ui.loop->second;
    }
    std::weak_ptr<int> alive = m_alive;
    req.on_finished = [this, alive] {
        if (!alive.expired()) OnFinished();
    };
    m_job = std::make_unique<exportng::ExportJob>();
    const HRESULT hr = m_job->Start(*m_host.device, *m_host.render_context, *m_host.audio_devices, std::move(req));
    if (FAILED(hr)) {
        m_error = Format(str::kExportFailed, HrToString(hr).c_str());
        m_job.reset();
        return;
    }
    m_job_path = m_dest;
    m_banner.Clear();
    m_open = false;
    m_minimized = false;
}

void ExportDialog::RunBannerAction(int action) {
    switch (action) {
    case kActionChooseFolder:
        // The save dialog picks the new file (and so the folder); the banner goes once a destination was chosen.
        if (BrowseDestination()) m_banner.Clear();
        break;
    case kActionRetry:
        m_banner.Clear();
        StartExport();
        break;
    default: break;
    }
}

void ExportDialog::OnFinished() {
    if (!m_job || !m_job->GetProgress().finished.load()) return;
    m_job->Join(5000);
    const exportng::ExportJob::Progress& p = m_job->GetProgress();
    if (m_host.toasts) {
        if (p.cancelled.load()) {
            m_host.toasts->Push(str::kExportCancelled, ToastKind::Info);
        } else if (FAILED(p.hr)) {
            // The dialog comes back (same settings) with the banner below; the toast only says that it failed.
            m_host.toasts->Push(Format(str::kExportFailed, p.error.c_str()), ToastKind::Error, {}, 6.f);
        } else {
            const std::string path = m_job_path;
            std::vector<ToastButton> buttons;
            buttons.push_back({str::kExportOpenFile, [path] { fs::OpenWithShell(path); }});
            buttons.push_back({str::kExportShowInFolder, [path] { fs::ShowInExplorer(path); }});
            m_host.toasts->Push(Format(str::kExportDone, std::string(PathName(path)).c_str()), ToastKind::Success, std::move(buttons), 10.f);
        }
    }
    if (!p.cancelled.load() && FAILED(p.hr)) {
        ShowErrorView(m_banner, DescribeExportFailure(p.hr, p.error, m_job_path), p.hr);
        m_open = true;   // reopen with the same settings: choose another folder or try again
    }
    m_job.reset();
    if (m_host.request_redraw) m_host.request_redraw();
}

void ExportDialog::Draw() {
    if (m_job && m_job->GetProgress().finished.load()) OnFinished();   // also without a UI poster
    if (Busy()) {
        if (!m_minimized) DrawProgress();
        return;
    }
    if (m_open) DrawSettings();
}

void ExportDialog::DrawSettings() {
    ImGui::SetNextWindowSize(FitViewport(ImVec2(Dp(760), Dp(560))), ImGuiCond_Appearing);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(str::kExportTitle, &m_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    // Left: presets.
    ImGui::BeginChild("##presets", ImVec2(Dp(220), -Dp(44)), ImGuiChildFlags_Borders);
    ImGui::TextDisabled("%s", str::kExportBuiltin);
    const int nb = static_cast<int>(m_builtin.size());
    for (int i = 0; i < nb + static_cast<int>(m_user.size()); ++i) {
        if (i == nb) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", str::kExportUser);
        }
        const Preset& p = i < nb ? m_builtin[static_cast<size_t>(i)] : m_user[static_cast<size_t>(i - nb)];
        ImGui::PushID(i);
        if (ImGui::Selectable(p.name.c_str(), m_selected == i)) {
            m_selected = i;
            SelectPreset(p);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: settings, under the error banner of a failed export (P11 11.5).
    ImGui::BeginChild("##settings", ImVec2(0, -Dp(44)));
    m_banner.Draw("##export_error");
    if (const int action = m_banner.TakeAction()) RunBannerAction(action);
    const project::Canvas& cv = m_project.canvas;
    int container = static_cast<int>(m_preset.container);
    if (SectionHeader(str::kExportFormat)) {
        if (PropEnum(str::kExportContainer, container, kContainers, 4)) {
            m_preset.container = static_cast<Preset::Container>(container);
            FixExtension();
        }
        if (m_preset.container == Preset::Container::Mp4) {
            // Codec: HEVC only with a hardware encoder.
            PropRowBegin(str::kExportCodec);
            const char* cur = m_preset.video.codec == exportng::VideoCodec::Hevc ? "HEVC (H.265)" : "H.264";
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##codec", cur)) {
                if (ImGui::Selectable("H.264", m_preset.video.codec == exportng::VideoCodec::H264)) m_preset.video.codec = exportng::VideoCodec::H264;
                ImGui::BeginDisabled(!m_hevc_hw);
                if (ImGui::Selectable("HEVC (H.265)", m_preset.video.codec == exportng::VideoCodec::Hevc)) m_preset.video.codec = exportng::VideoCodec::Hevc;
                ImGui::EndDisabled();
                if (!m_hevc_hw && m_caps_known) ItemTooltip(str::kExportHevcUnavailable);
                ImGui::EndCombo();
            }
            if (m_preset.video.codec == exportng::VideoCodec::Hevc && m_caps_known && !m_hevc_hw)
                ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "%s", str::kExportHevcUnavailable);
        }
    }
    const bool has_video = m_preset.container != Preset::Container::M4a;
    if (has_video && SectionHeader(str::kExportVideo)) {
        const char* size_labels[5];
        for (int i = 0; i < 5; ++i) size_labels[i] = kSizes[i].label;
        if (PropEnum(str::kExportSize, m_size_choice, size_labels, 5) && m_size_choice != kCustomSize) {
            m_preset.video.width = kSizes[m_size_choice].w;
            m_preset.video.height = kSizes[m_size_choice].h;
        }
        if (m_size_choice == kCustomSize) {
            int w = m_preset.video.width > 0 ? m_preset.video.width : cv.width;
            if (PropInt(str::kExportWidth, w, 16, 8192, "px", cv.width)) m_preset.video.width = std::max(16, w & ~1);
            m_preset.video.height = std::max(16, static_cast<int>(std::lround(static_cast<double>(m_preset.video.width) * cv.height / std::max(1, cv.width))) & ~1);
        }
        if (m_preset.container == Preset::Container::Mp4) {
            if (PropEnum(str::kExportFps, m_fps_choice, kFpsLabels, 4)) {
                m_preset.video.fps_num = kFpsValues[m_fps_choice];
                m_preset.video.fps_den = 1;
            }
            int rc = static_cast<int>(m_preset.video.rc);
            if (PropEnum(str::kExportRateControl, rc, kRc, 3)) m_preset.video.rc = static_cast<exportng::RateControl>(rc);
            if (m_preset.video.rc == exportng::RateControl::Quality) {
                PropInt(str::kExportQuality, m_preset.video.quality, 0, 100, nullptr, 75);
            } else {
                int kbps = static_cast<int>(m_preset.video.bitrate_kbps);
                if (PropInt(str::kExportBitrate, kbps, 500, 200000, "kbps", 10000)) m_preset.video.bitrate_kbps = static_cast<uint32_t>(kbps);
                if (m_preset.video.rc == exportng::RateControl::Vbr) {
                    int peak = static_cast<int>(std::max(m_preset.video.max_bitrate_kbps, m_preset.video.bitrate_kbps));
                    if (PropInt(str::kExportMaxBitrate, peak, kbps, 300000, "kbps", kbps * 3 / 2)) m_preset.video.max_bitrate_kbps = static_cast<uint32_t>(peak);
                }
            }
        }
    }
    if (m_preset.container == Preset::Container::Gif && SectionHeader(str::kExportGif)) {
        PropInt(str::kExportGifFps, m_preset.gif.fps, 1, 50, "fps", 20);
        PropInt(str::kExportGifColors, m_preset.gif.max_colors, 2, 256, nullptr, 256);
        PropToggle(str::kExportGifDither, m_preset.gif.dither);
    }
    if ((m_preset.container == Preset::Container::Mp4 || m_preset.container == Preset::Container::M4a) && SectionHeader(str::kExportAudio)) {
        int a = 3;
        for (int i = 0; i < 4; ++i)
            if (kAacRates[i] == m_preset.audio.bitrate_kbps) a = i;
        if (PropEnum(str::kExportAudioBitrate, a, kAacLabels, 4)) m_preset.audio.bitrate_kbps = kAacRates[a];
    }
    if (SectionHeader(str::kExportRange)) {
        if (m_preset.container == Preset::Container::Png) {
            char tc[32];
            PropValue(str::kExportStillAt, FormatTimecode(tc, sizeof(tc), m_playhead, cv.fps_num, cv.fps_den));
        } else {
            const char* items[2] = {str::kExportRangeWhole, str::kExportRangeLoop};
            ImGui::BeginDisabled(!m_project.ui.loop.has_value());
            SegmentedControl("##range", &m_range, items, 2);
            ImGui::EndDisabled();
            if (!m_project.ui.loop) m_range = 0;
            char d[32];
            PropValue(str::kExportDuration, FormatDuration(d, sizeof(d), RangeDuration()));
        }
    }
    if (SectionHeader(str::kExportOutput)) {
        int fn = 0, fd = 1;
        exportng::ExportJob::OutputFps(m_preset, cv, fn, fd);
        const SizeI out = exportng::ExportJob::OutputSize(m_preset, cv);
        if (m_preset.container == Preset::Container::M4a) PropValue(str::kExportOutput, "AAC, 48 kHz");
        else PropValue(str::kExportOutput, Format("%d x %d, %.4g fps", out.w, out.h, static_cast<double>(fn) / fd).c_str());
        const uint64_t bytes = m_preset.EstimateBytes(RangeDuration(), SizeI{cv.width, cv.height}, cv.fps_num / std::max(1, cv.fps_den));
        PropValue(str::kExportEstimate, ("~" + FormatBytes(bytes)).c_str());
        const float w = PropRowBegin(str::kExportDestination);
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "%s", m_dest.c_str());
        ImGui::SetNextItemWidth(w - Dp(90));
        if (ImGui::InputText("##dest", buf, sizeof(buf))) m_dest = buf;
        ImGui::SameLine();
        if (Button(str::kExportBrowse, ButtonKind::Normal, ImVec2(Dp(84), 0))) BrowseDestination();
    }
    if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", m_error.c_str());
    ImGui::EndChild();

    // Bottom bar.
    if (Button(str::kExportSaveAsPreset)) {
        std::snprintf(m_save_name, sizeof(m_save_name), "%s", m_preset.name.c_str());
        ImGui::OpenPopup("##savepreset");
    }
    if (ImGui::BeginPopup("##savepreset")) {
        ImGui::TextUnformatted(str::kExportPresetName);
        ImGui::SetNextItemWidth(Dp(240));
        const bool enter = ImGui::InputText("##name", m_save_name, sizeof(m_save_name), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((Button(str::kExportSave, ButtonKind::Primary) || enter) && m_save_name[0]) {
            Preset p = m_preset;
            p.name = m_save_name;
            p.id = MakePresetId(p.name);
            std::weak_ptr<int> alive = m_alive;
            Async::Run([this, alive, p] {
                Err err;
                const bool ok = p.SaveUser(&err);
                Async::PostToUi([this, alive, p, ok, err] {
                    if (alive.expired()) return;
                    if (ok) {
                        auto it = std::find_if(m_user.begin(), m_user.end(), [&](const Preset& u) { return u.id == p.id; });
                        if (it != m_user.end()) *it = p;
                        else m_user.push_back(p);
                        if (m_host.toasts) m_host.toasts->Push(Format(str::kExportPresetSaved, p.name.c_str()), ToastKind::Success);
                    } else if (m_host.toasts) {
                        m_host.toasts->Push(err.message, ToastKind::Error);
                    }
                    if (m_host.request_redraw) m_host.request_redraw();
                });
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (Button(str::kExportCancel)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - Dp(200));
    if (Button(str::kExportCancel, ButtonKind::Normal, ImVec2(Dp(88), 0))) m_open = false;
    ImGui::SameLine();
    const bool blocked = m_preset.container == Preset::Container::Mp4 && m_preset.video.codec == exportng::VideoCodec::Hevc && m_caps_known && !m_hevc_hw;
    ImGui::BeginDisabled(blocked);
    bool start = Button(str::kExportStart, ButtonKind::Primary, ImVec2(Dp(88), 0));
    ImGui::EndDisabled();
    // Keyboard (P11 11.6): Enter starts the export, Esc closes, unless a text field or a popup has the keys.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_open = false;
        else if (!blocked && !ImGui::GetIO().NavVisible &&
                 (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            start = true;
    }
    if (start && !blocked) StartExport();
    ImGui::End();
}

void ExportDialog::DrawProgress() {
    const exportng::ExportJob::Progress& p = m_job->GetProgress();
    ImGui::SetNextWindowSize(FitViewport(ImVec2(Dp(440), 0)), ImGuiCond_Always);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(str::kExportExporting, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
                                                         ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(std::string(PathName(m_job_path)).c_str());
        const float f = static_cast<float>(p.fraction.load());
        ImGui::ProgressBar(f, ImVec2(-1, Dp(8)), "");
        ImGui::Text(str::kExportProgressFmt, static_cast<long long>(p.frames_done.load()), static_cast<long long>(p.frames_total.load()));
        ImGui::SameLine();
        ImGui::TextDisabled("  ");
        ImGui::SameLine();
        ImGui::Text(str::kExportSpeedFmt, p.speed_realtime.load());
        char eta[32];
        ImGui::SameLine();
        ImGui::TextDisabled("  ");
        ImGui::SameLine();
        ImGui::Text(str::kExportEtaFmt, FormatDuration(eta, sizeof(eta), p.eta_us.load()));
        ImGui::Spacing();
        if (Button(str::kExportMinimize)) m_minimized = true;
        ImGui::SameLine();
        if (Button(str::kExportCancel, ButtonKind::Danger)) m_job->Cancel();
        // Esc minimizes the progress window (the export keeps running; Cancel stays an explicit click).
        if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) m_minimized = true;
    }
    ImGui::End();
}

} // namespace ct::ui
