#define CT_LOG_CATEGORY "bar"
#include <ct/ui/recorder_bar.h>

#include <ct/audio/devices.h>
#include <ct/audio/wasapi_capture.h>
#include <ct/audio/wasapi_loopback.h>
#include <ct/capture/display.h>
#include <ct/capture/webcam_source.h>
#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/encoder_caps.h>
#include <ct/platform/hotkeys.h>
#include <ct/platform/mf_runtime.h>
#include <ct/platform/nv12_blit.h>
#include <ct/platform/paths.h>
#include <ct/platform/tray_icon.h>
#include <ct/recorder/countdown_overlay.h>
#include <ct/recorder/recording_frame.h>
#include <ct/recorder/recording_session.h>
#include <ct/recorder/region_overlay.h>
#include <ct/ui/dialogs.h>
#include <ct/ui/error_ux.h>
#include <ct/ui/recorder_errors.h>
#include <ct/ui/fonts.h>
#include <ct/ui/settings_window.h>
#include <ct/ui/icons.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace ct::ui {

using recorder::State;
using namespace metrics;

namespace {

constexpr float kIdleW = 560, kIdleH = 64;   // dip (docs/05 section 2)
constexpr float kRecW = 260, kRecH = 44;
constexpr float kSnapDip = 16;               // snap to work-area edges within 16 dip
constexpr float kMarginDip = 24;             // default distance from the bottom of the work area
constexpr UINT kTrayMsg = WM_APP + 41;
enum HotkeyId { kHkToggle = 1, kHkPause = 2, kHkMarker = 3, kHkCancel = 4, kHkEsc = 5 };
constexpr UINT_PTR kArmTimer = 0xC7A1;   // re-arm the session this long after the bar last changed (debounce)
constexpr UINT kArmDelayMs = 700;
enum MenuId { kMenuRecord = 1, kMenuPause, kMenuStop, kMenuEditor, kMenuSettings, kMenuShow, kMenuQuit };
constexpr const char* kSyntheticCameraId = "synthetic";

const int kFpsChoices[] = {15, 24, 30, 60, 0};   // 0 = native (monitor refresh)
const int kQualityValues[] = {60, 70, 85};

float NowS() { return static_cast<float>(ImGui::GetTime()); }

// Null sinks for the idle meters / camera preview.
struct NullAudioSink final : audio::AudioSink {
    bool OnAudio(uint32_t, const audio::AudioPacket&) override { return true; }
};
struct PreviewSink final : capture::FrameSink {
    capture::WebcamSource* src = nullptr;
    bool OnFrame(ID3D11Texture2D*, uint32_t index, TimeUs) override {
        src->Nv12Pool().Release(index);   // only the preview copy is used
        return true;
    }
};

// Idle level monitor: a capture stream with a null sink, run while a device popover is open.
struct AudioMonitor {
    std::shared_ptr<audio::AudioDevices> devices;
    std::unique_ptr<audio::WasapiCapture> mic;
    std::unique_ptr<audio::WasapiLoopback> sys;
    NullAudioSink sink;
    std::atomic<bool> running{false};
    float Peak() const {
        const audio::Meter* m = mic ? &mic->Levels() : sys ? &sys->Levels() : nullptr;
        if (!m || !running.load()) return audio::Meter::kFloorDb;
        float p = audio::Meter::kFloorDb;
        for (int c = 0; c < std::max(1, m->Channels()) && c < audio::Meter::kMaxChannels; ++c) p = std::max(p, m->PeakDb(c));
        return p;
    }
    ~AudioMonitor() {
        if (mic) mic->Stop();
        if (sys) sys->Stop();
    }
};

struct CameraPreview {
    capture::WebcamSource src;
    PreviewSink sink;
    capture::QpcMediaTimeSource clock{0};
    std::atomic<bool> running{false};
    std::atomic<bool> failed{false};
    CameraPreview() : clock(Clock::QpcNow()) { sink.src = &src; }
    ~CameraPreview() { src.Shutdown(); }
};

struct ToastItem {
    int id = 0;
    std::string text;
    ToastKind kind = ToastKind::Info;
    std::string bundle;   // Saved toast: Open / Show / Delete act on it
    float shown_at = 0;
    float seconds = 6;
};

std::wstring W(const std::string& s) { return Utf8ToWide(s); }

std::string BundleName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void FormatClock(char* buf, size_t n, TimeUs t) {
    const int64_t s = std::max<int64_t>(0, t / 1'000'000);
    std::snprintf(buf, n, "%02lld:%02lld:%02lld", static_cast<long long>(s / 3600), static_cast<long long>((s / 60) % 60),
                  static_cast<long long>(s % 60));
}

// --- small drawing helpers -------------------------------------------------------------------------------------------

void DrawCentered(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, const char* text) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(std::floor((min.x + max.x - ts.x) * 0.5f), std::floor((min.y + max.y - ts.y) * 0.5f)), col, text);
}

// A hoverable/clickable area at the cursor; returns pressed. Draws the hover/active fill.
bool Chip(const char* id, ImVec2 size, bool active, bool* hovered_out = nullptr, ImU32 fill_override = 0) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    const ImGuiID gid = window->GetID(id);
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, gid)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    ImU32 bg = fill_override;
    if (!bg) bg = held ? col::bg4 : active ? col::bg3 : hovered ? col::bg3 : 0;
    if (bg) window->DrawList->AddRectFilled(bb.Min, bb.Max, bg, Dp(kRadiusControl));
    ImGui::RenderNavCursor(bb, gid);   // keyboard focus ring (Tab / arrows, P11 11.6)
    if (hovered_out) *hovered_out = hovered;
    return pressed;
}

void VSeparator(ImDrawList* dl, float x, float y0, float y1) {
    dl->AddLine(ImVec2(std::floor(x) + 0.5f, y0), ImVec2(std::floor(x) + 0.5f, y1), col::border, 1.0f);
}

void Icon(ImDrawList* dl, ImVec2 center, ImU32 color, const char* glyph, FontRole role = FontRole::Icon) {
    FontScope f(role);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(std::floor(center.x - ts.x * 0.5f), std::floor(center.y - ts.y * 0.5f)), color, glyph);
}

// Region glyph: dashed rectangle with corner ticks.
void RegionGlyph(ImDrawList* dl, ImVec2 c, ImU32 color) {
    const float w = Dp(14), h = Dp(10), t = Dp(1.2f);
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    const float dash = Dp(2.5f);
    for (float x = a.x; x < b.x; x += dash * 2) {
        dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + dash, b.x), a.y), color, t);
        dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + dash, b.x), b.y), color, t);
    }
    for (float y = a.y; y < b.y; y += dash * 2) {
        dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + dash, b.y)), color, t);
        dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + dash, b.y)), color, t);
    }
}

// Window glyph: rectangle with a title strip.
void WindowGlyph(ImDrawList* dl, ImVec2 c, ImU32 color) {
    const float w = Dp(14), h = Dp(11);
    const ImVec2 a(std::floor(c.x - w * 0.5f) + 0.5f, std::floor(c.y - h * 0.5f) + 0.5f), b(a.x + w, a.y + h);
    dl->AddRect(a, b, color, Dp(1.5f), Dp(1.2f));
    dl->AddRectFilled(a, ImVec2(b.x, a.y + Dp(3)), color, Dp(1.5f), ImDrawFlags_RoundTop);
}

// Grip: 2 x 3 dots.
void GripGlyph(ImDrawList* dl, ImVec2 c, ImU32 color) {
    const float r = Dp(1.3f), dx = Dp(3), dy = Dp(4);
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j) dl->AddCircleFilled(ImVec2(c.x + (i * 2 - 1) * dx * 0.5f, c.y + (j - 1) * dy), r, color, 8);
}

// Compact 3-segment meter (green < -18 dBFS, amber < -6, red above), horizontal.
void MiniMeter(ImDrawList* dl, ImVec2 a, ImVec2 size, float db, bool enabled) {
    const float bounds[4] = {-60, -18, -6, 0};
    const ImU32 colors[3] = {col::success, col::warn, col::danger};
    const float gap = Dp(1.5f);
    const float seg = (size.x - gap * 2) / 3;
    for (int i = 0; i < 3; ++i) {
        const float x0 = a.x + i * (seg + gap);
        dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x0 + seg, a.y + size.y), enabled ? col::bg4 : col::bg3, Dp(1));
        if (!enabled) continue;
        float fill = (db - bounds[i]) / (bounds[i + 1] - bounds[i]);
        fill = std::clamp(fill, 0.0f, 1.0f);
        if (fill > 0) dl->AddRectFilled(ImVec2(x0, a.y), ImVec2(x0 + seg * fill, a.y + size.y), colors[i], Dp(1));
    }
}

void Strike(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col::bg1, Dp(3.5f));
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), color, Dp(1.4f));
}

} // namespace

// ======================================================================================================================

struct RecorderBar::Impl final : recorder::SessionListener {
    platform::D3DDevice* dev = nullptr;
    platform::Window* win = nullptr;
    Settings settings;
    std::string settings_path;
    Options opt;
    Hooks hooks;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    recorder::RecordingSession session;
    recorder::RegionOverlay region_overlay;
    recorder::CountdownOverlay countdown;
    recorder::RecordingFrame frame;
    platform::HotkeyManager hotkeys;
    platform::TrayIcon tray;
    HICON icon_normal = nullptr, icon_recording = nullptr;

    // Sources and devices (filled asynchronously).
    std::vector<capture::DisplayInfo> displays;
    int monitor = 0;
    int source = 0;               // 0 monitor, 1 region, 2 window
    RectI region;                 // region / window mode selection (virtual px)
    int region_display = -1;
    bool start_after_region = false;
    std::shared_ptr<audio::AudioDevices> audio_devices = std::make_shared<audio::AudioDevices>();
    std::vector<audio::DeviceInfo> mics, outputs;
    std::vector<capture::WebcamInfo> cameras;
    bool devices_listed = false, caps_listed = false, hevc_hw = false;
    std::wstring system_device;   // runtime only (settings have no field for it)

    // Session state mirrored on the UI thread.
    State state = State::Idle;
    recorder::LiveStats stats;
    bool preparing = false;
    int countdown_left = 0;
    int64_t trigger_qpc = 0;
    bool quit_after_finish = false;
    std::string last_bundle;

    // Error UX (P11 11.5).
    ErrorBanner error_banner;
    std::string error_bundle;     // the kept bundle of the failed recording (Open in editor / Show in folder)
    bool error_recording = false; // between OnStateChanged(Error) and OnFailure
    int last_failure_cause = -1;
    std::string nav_label;        // diagnostics: label of the nav-focused item (last frame)
    std::string frame_nav_label;
    bool focus_first = false;     // keyboard focus to the first control on the next frame

    // Window geometry and animation.
    bool visible = true;
    POINT anchor{};               // bottom-centre of the bar, px
    float size_from_w = kIdleW, size_from_h = kIdleH, size_to_w = kIdleW, size_to_h = kIdleH;
    float size_anim_t0 = -1;
    float anim_until = 0;
    RECT grip_px{};               // client px, for WM_NCHITTEST
    float rec_started_at = 0;

    // Meters (dB with a 20 dB/s fall-off).
    float shown_mic = -120, shown_sys = -120;
    float last_meter_t = 0;
    std::shared_ptr<AudioMonitor> monitor_audio;
    Popup monitor_for = Popup::None;

    // Camera preview.
    std::shared_ptr<CameraPreview> preview;
    platform::Nv12Blit blit;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> preview_tex;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> preview_rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preview_srv;
    uint64_t preview_serial = 0;
    bool preview_has_frame = false;
    float preview_wanted_until = 0;

    // Settings window (its own decorated platform window through the multi-viewport backend).
    std::unique_ptr<SettingsWindow> settings_win;

    // Popups and toasts.
    Popup open_request = Popup::None;
    std::vector<ToastItem> toasts;
    int next_toast = 1;

    // ------------------------------------------------------------------------------------------------------------
    float Scale() const { return win ? win->DpiScale() : 1.0f; }
    int Px(float dip) const { return static_cast<int>(std::lround(dip * Scale())); }

    void Animate(float seconds = 0.25f) { anim_until = std::max(anim_until, NowS() + seconds); }

    void PushToast(std::string text, ToastKind kind, std::string bundle = {}, float seconds = 6) {
        ToastItem t;
        t.id = next_toast++;
        t.text = std::move(text);
        t.kind = kind;
        t.bundle = std::move(bundle);
        t.shown_at = NowS();
        t.seconds = seconds;
        toasts.push_back(std::move(t));
        if (toasts.size() > 4) toasts.erase(toasts.begin());
        Animate();
    }

    // --- SessionListener (UI thread through Async::PostToUi) ----------------------------------------------------
    void OnStateChanged(State s) override {
        const State old = state;
        state = s;
        if (s == State::Error) error_recording = true;
        Animate(0.3f);
        if (s == State::Countdown || s == State::Starting || s == State::Recording) SetBarSize(kRecW, kRecH);
        if (s == State::Recording && old != State::Paused) {
            rec_started_at = NowS();
            if (error_banner.Visible() && error_bundle.empty() && last_failure_cause >= 0) error_banner.Clear();
            const capture::DisplayInfo* d = CurrentDisplay();
            if (d) {
                RectI r = source == 0 ? d->rect : capture::ClampRegionToDisplay(*d, region);
                std::vector<RectI> rects;
                for (const auto& di : displays) rects.push_back(di.rect);
                if (source != 0) frame.Show(r, rects, d->dpi);   // a full monitor needs no frame around it
            }
            if (icon_recording) tray.SetIcon(icon_recording);
            if (settings.recording.hide_bar_while_recording) ShowBar(false);
        }
        if (s == State::Finalizing || s == State::Idle) frame.Hide();
        if (s == State::Idle) {
            preparing = false;
            ScheduleArm();
            SetBarSize(kIdleW, kIdleH);
            if (icon_normal) tray.SetIcon(icon_normal);
            tray.SetTip(W(str::kTrayTip).c_str());
            if (countdown.IsRunning()) countdown.Cancel();
            hotkeys.Unregister(win->Hwnd(), kHkEsc);
            if (settings.recording.hide_bar_while_recording && !visible && !opt.start_hidden) ShowBar(true);
            if (quit_after_finish && hooks.quit) hooks.quit();
        }
    }
    void OnCountdown(int seconds_left) override { countdown_left = seconds_left; }
    void OnTick(const recorder::LiveStats& st) override {
        stats = st;
        char clock[32], tip[96];
        FormatClock(clock, sizeof(clock), st.elapsed_us);
        std::snprintf(tip, sizeof(tip), str::kTrayTipRecording, clock);
        tray.SetTip(W(tip).c_str());
    }
    void OnFinished(const std::string& bundle_path) override {
        last_bundle = bundle_path;
        if (quit_after_finish) return;
        if (error_recording) return;   // the error panel that follows carries Open in editor / Show in folder
        ShowSavedToastImpl(bundle_path);
    }
    // Only reached through OnFailure's default; kept for the interface.
    void OnError(const std::string& message, HRESULT hr) override {
        recorder::SessionFailure f;
        f.message = message;
        f.hr = hr;
        OnFailure(f);
    }
    void OnFailure(const recorder::SessionFailure& f) override {
        error_recording = false;
        last_failure_cause = static_cast<int>(f.cause);
        const ErrorView v = DescribeFailure(f);
        ShowErrorView(error_banner, v, f.hr);
        error_bundle = f.saved ? f.bundle_path : std::string();
        if (!visible) tray.Notify(W(str::kAppName).c_str(), W(v.message).c_str());
        CT_LOG_WARN("error panel: %s (%s, cause %s)", v.message.c_str(), HrToString(f.hr).c_str(), recorder::FailureCauseName(f.cause));
        // TDR: the session is idle now; the app recreates the device before the next frame (P11 11.5).
        if (v.recreate_device && hooks.recreate_device) hooks.recreate_device();
        Animate();
    }
    void OnWarning(const std::string& message) override { PushToast(message, ToastKind::Warning); }
    void OnWarningInfo(const recorder::SessionWarning& w) override {
        const ErrorView v = DescribeWarning(w);
        if (!v.banner) {
            PushToast(w.message, ToastKind::Warning);
            return;
        }
        // Microphone busy / unplugged: the recording goes on; a banner (not a toast) so it is not missed.
        ShowErrorView(error_banner, v, w.hr);
        error_bundle.clear();
        if (!visible) tray.Notify(W(str::kAppName).c_str(), W(v.message).c_str());
        Animate();
    }

    void RunErrorAction(int action) {
        switch (action) {
        case kActionChooseFolder: {
            // A modal shell dialog must not run inside the ImGui frame (mouse capture, P09 pitfalls): post it.
            std::weak_ptr<bool> weak = alive;
            Impl* self = this;
            Async::PostToUi([weak, self]() {
                if (!weak.expired() && *weak.lock()) self->ChooseOutputFolder();
            });
            break;
        }
        case kActionOpenEditor:
            if (hooks.open_editor && !error_bundle.empty()) hooks.open_editor(error_bundle);
            error_banner.Clear();
            break;
        case kActionShowFolder:
            if (!error_bundle.empty()) fs::ShowInExplorer(error_bundle);
            break;
        default: break;
        }
        Animate();
    }

    void ChooseOutputFolder() {
        const std::string start = settings.recording.output_dir.empty() ? platform::Paths::DefaultRecordingsDir()
                                                                         : settings.recording.output_dir;
        const std::string folder = dialogs::PickFolder(win->Hwnd(), str::kErrPickFolderTitle, start);
        if (folder.empty()) return;   // cancelled: the banner stays
        SetOutputFolder(folder);
    }

    void SetOutputFolder(const std::string& folder) {
        settings.recording.output_dir = folder;
        SaveSettingsAsync();
        if (settings_win && settings_win->IsOpen()) settings_win->Open(settings, settings_win->Section());
        error_banner.Clear();
        PushToast(Format(str::kErrFolderChanged, folder.c_str()), ToastKind::Info, {}, 6);
        CT_LOG_INFO("output folder changed after an error: %s", folder.c_str());
    }
    void OnPrepared(HRESULT hr) override {
        preparing = false;
        if (FAILED(hr) && countdown.IsRunning()) countdown.Cancel();
    }

    void ShowSavedToastImpl(const std::string& bundle_path) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), str::kToastSaved, BundleName(bundle_path).c_str());
        if (!visible) tray.Notify(W(str::kNotifySavedTitle).c_str(), W(BundleName(bundle_path)).c_str());
        PushToast(buf, ToastKind::Success, bundle_path, 8);
    }

    // --- geometry -------------------------------------------------------------------------------------------------
    const capture::DisplayInfo* CurrentDisplay() const {
        const int idx = source == 0 ? monitor : (region_display >= 0 ? region_display : monitor);
        for (const auto& d : displays)
            if (d.index == idx) return &d;
        return displays.empty() ? nullptr : &displays[0];
    }

    void DefaultAnchor() {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        anchor.x = (mi.rcWork.left + mi.rcWork.right) / 2;
        anchor.y = mi.rcWork.bottom - Px(kMarginDip);
    }

    void InitAnchor() {
        const auto& b = settings.window.bar;
        if (b.x == 0 && b.y == 0) {
            DefaultAnchor();
            return;
        }
        // Settings keep the idle bar's top-left in px; the anchor is its bottom-centre.
        anchor.x = b.x + Px(kIdleW) / 2;
        anchor.y = b.y + Px(kIdleH);
        if (!MonitorFromPoint(POINT{anchor.x, anchor.y - 1}, MONITOR_DEFAULTTONULL)) DefaultAnchor();
    }

    void ApplyWindowRect(float w_dip, float h_dip) {
        const int w = Px(w_dip), h = Px(h_dip);
        SetWindowPos(win->Hwnd(), HWND_TOPMOST, anchor.x - w / 2, anchor.y - h, w, h, SWP_NOACTIVATE);
    }

    void SetBarSize(float w_dip, float h_dip) {
        if (w_dip == size_to_w && h_dip == size_to_h) return;
        size_from_w = CurrentW();
        size_from_h = CurrentH();
        size_to_w = w_dip;
        size_to_h = h_dip;
        size_anim_t0 = NowS();
        Animate(kMotionMs / 1000.0f + 0.05f);
    }
    float AnimT() const {
        if (size_anim_t0 < 0) return 1;
        return EaseOut(std::clamp((NowS() - size_anim_t0) * 1000.0f / kMotionMs, 0.0f, 1.0f));
    }
    float CurrentW() const { return size_from_w + (size_to_w - size_from_w) * AnimT(); }
    float CurrentH() const { return size_from_h + (size_to_h - size_from_h) * AnimT(); }

    void UpdateWindowSize() {
        if (size_anim_t0 < 0) return;
        const float t = AnimT();
        ApplyWindowRect(CurrentW(), CurrentH());
        if (t >= 1.0f) size_anim_t0 = -1;
    }

    void SaveSettingsAsync() {
        Settings copy = settings;
        std::string path = settings_path;
        if (path.empty()) return;
        Async::Run([copy = std::move(copy), path = std::move(path)]() {
            Err err;
            if (!copy.Save(path, &err)) CT_LOG_WARN("saving settings failed: %s", err.message.c_str());
        });
    }

    void RememberPosition() {
        RECT rc;
        GetWindowRect(win->Hwnd(), &rc);
        anchor.x = (rc.left + rc.right) / 2;
        anchor.y = rc.bottom;
        settings.window.bar.x = anchor.x - Px(kIdleW) / 2;
        settings.window.bar.y = anchor.y - Px(kIdleH);
        SaveSettingsAsync();
    }

    void Snap(RECT& rc) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& w = mi.rcWork;
        const int snap = Px(kSnapDip);
        const int width = rc.right - rc.left, height = rc.bottom - rc.top;
        if (std::abs(rc.left - w.left) < snap) rc.left = w.left;
        if (std::abs(w.right - rc.right) < snap) rc.left = w.right - width;
        if (std::abs(rc.top - w.top) < snap) rc.top = w.top;
        if (std::abs(w.bottom - rc.bottom) < snap) rc.top = w.bottom - height;
        const int cx = (w.left + w.right) / 2;
        if (std::abs((rc.left + width / 2) - cx) < snap) rc.left = cx - width / 2;   // centre line
        rc.right = rc.left + width;
        rc.bottom = rc.top + height;
    }

    // The bar was shown from the keyboard (tray icon Enter, tray menu): activate it so Tab / Enter reach it.
    void FocusForKeyboard() {
        SetForegroundWindow(win->Hwnd());
        focus_first = true;
        Animate();
    }

    void ShowBar(bool show) {
        visible = show;
        ShowWindow(win->Hwnd(), show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show) SetWindowPos(win->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Animate();
    }

    // --- async device discovery -------------------------------------------------------------------------------
    // Two jobs: displays + audio endpoints are fast (the popovers need them first); encoder caps (activates every
    // encoder MFT once) and camera modes (opens each camera) take up to a few hundred ms each.
    void DiscoverAsync() {
        std::weak_ptr<bool> weak = alive;
        platform::D3DDevice* d = dev;
        auto devices = audio_devices;
        Impl* self = this;
        Async::Run([weak, d, devices, self]() {
            auto disp = capture::EnumerateDisplays(d->Factory());
            std::vector<audio::DeviceInfo> m, o;
            if (SUCCEEDED(devices->Init())) {
                m = devices->List(true);
                o = devices->List(false);
            }
            Async::PostToUi([weak, self, disp = std::move(disp), m = std::move(m), o = std::move(o)]() mutable {
                if (weak.expired()) return;
                self->displays = std::move(disp);
                self->mics = std::move(m);
                self->outputs = std::move(o);
                self->devices_listed = true;
                if (self->monitor >= static_cast<int>(self->displays.size())) self->monitor = 0;
                CT_LOG_INFO("bar: %zu displays, %zu mics, %zu outputs", self->displays.size(), self->mics.size(),
                            self->outputs.size());
                self->Animate();
            });
        });
    }
    // Encoder caps (activates every encoder MFT once, MFStartup/MFShutdown around it) and camera modes (opens each
    // camera): up to a few hundred ms of CPU on the IO pool. Started after the first frame (P11 11.2) so it does not
    // compete with the window's first present; a recording started earlier waits on the caps cache lock.
    void DiscoverCapsAsync() {
        if (caps_started) return;
        caps_started = true;
        std::weak_ptr<bool> weak = alive;
        const bool synthetic = opt.synthetic_camera;
        Impl* self = this;
        Async::Run([weak, synthetic, self]() {
            const bool hevc = media::EncoderCaps::HasHardware(media::VideoCodec::Hevc);
            media::EncoderCaps::HasHardware(media::VideoCodec::H264);   // warm the cache for the first recording
            std::vector<capture::WebcamInfo> cams;
            capture::WebcamSource::Enumerate(cams, true);
            if (synthetic) {
                capture::WebcamInfo s;
                s.name = "Synthetic camera";
                s.symbolic_link = Utf8ToWide(kSyntheticCameraId);
                s.sizes = {SizeI{1280, 720}, SizeI{640, 360}};
                cams.push_back(std::move(s));
            }
            Async::PostToUi([weak, self, hevc, cams = std::move(cams)]() mutable {
                if (weak.expired()) return;
                self->cameras = std::move(cams);
                self->hevc_hw = hevc;
                self->caps_listed = true;
                if (!self->hevc_hw && self->settings.recording.codec == "hevc") self->settings.recording.codec = "h264";
                CT_LOG_INFO("bar: %zu cameras, HEVC hardware encoder %s", self->cameras.size(), self->hevc_hw ? "yes" : "no");
                self->Animate();
            });
        });
    }

    // Hovering REC (P11 11.2 "prewarm"): MF stays started for the process and the DXGI device manager exists before
    // the click, so the session prepare skips MFStartup and MFCreateDXGIDeviceManager. Not the audio devices: the
    // microphone LED stays off until recording. Once per process; cheap no-op afterwards.
    void PrewarmEncoder() {
        if (prewarmed || state != State::Idle) return;
        prewarmed = true;
        DiscoverCapsAsync();
        platform::D3DDevice* d = dev;
        Async::Run([d]() {
            const int64_t t0 = Clock::QpcNow();
            CT_LOG_IF_FAILED(platform::MfRuntime::EnsureForProcess());
            CT_LOG_IF_FAILED(d->InitMfManager());
            media::EncoderCaps::HasHardware(media::VideoCodec::H264);
            CT_LOG_INFO("bar: encoder prewarm %.1f ms", static_cast<double>(Clock::QpcNow() - t0) * 1000.0 / Clock::QpcFrequency());
        });
    }

    void StartMonitor(Popup which) {
        if (monitor_for == which || state != State::Idle) return;
        StopMonitor();
        if (which == Popup::Mic && (!settings.recording.mic || mics.empty())) return;
        if (which == Popup::System && (!settings.recording.system_audio || outputs.empty())) return;
        auto mon = std::make_shared<AudioMonitor>();
        mon->devices = audio_devices;
        monitor_for = which;
        monitor_audio = mon;
        const std::wstring mic_id = W(settings.recording.mic_device);
        const std::wstring sys_id = system_device;
        Async::Run([mon, which, mic_id, sys_id]() {
            audio::CaptureConfig cc;
            if (which == Popup::Mic) {
                cc.device_id = mic_id;
                auto c = std::make_unique<audio::WasapiCapture>();
                if (SUCCEEDED(c->Init(cc, *mon->devices)) && c->Start(Clock::QpcNow(), mon->sink)) {
                    mon->mic = std::move(c);
                    mon->running.store(true);
                }
            } else {
                cc.device_id = sys_id;
                auto c = std::make_unique<audio::WasapiLoopback>();
                if (SUCCEEDED(c->Init(cc, *mon->devices)) && c->Start(Clock::QpcNow(), mon->sink)) {
                    mon->sys = std::move(c);
                    mon->running.store(true);
                }
            }
        });
    }
    void StopMonitor() {
        if (!monitor_audio) return;
        auto mon = std::move(monitor_audio);
        monitor_for = Popup::None;
        Async::Run([mon]() mutable { mon.reset(); });   // Stop() joins the capture thread: not on the UI thread
    }

    void StartPreview() {
        if (preview || state != State::Idle || settings.recording.webcam_device.empty()) return;
        auto p = std::make_shared<CameraPreview>();
        preview = p;
        preview_has_frame = false;
        platform::D3DDevice* d = dev;
        const std::string id = settings.recording.webcam_device;
        const SizeI size = ParseSize(settings.recording.webcam_resolution);
        Async::Run([p, d, id, size]() {
            HRESULT hr;
            if (id == kSyntheticCameraId) {
                hr = p->src.InitWithReader(*d, capture::CreateSyntheticWebcamReader(*d, size, 30, true), size, 30, 1);
            } else {
                capture::WebcamSource::Config c;
                c.symbolic_link = Utf8ToWide(id);
                c.preferred_size = size;
                hr = p->src.Init(*d, c);
            }
            if (SUCCEEDED(hr) && p->src.Start(p->clock, p->sink)) p->running.store(true);
            else p->failed.store(true);
        });
    }
    void StopPreview(std::function<void()> then = {}) {
        auto p = std::move(preview);
        preview_has_frame = false;
        if (!p && !then) return;
        Async::Run([p, then = std::move(then)]() mutable {
            if (p) {
                p->src.Stop();
                p.reset();
            }
            if (then) then();
        });
    }

    static SizeI ParseSize(const std::string& s) {
        int w = 1280, h = 720;
        if (std::sscanf(s.c_str(), "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) return SizeI{1280, 720};
        return SizeI{w, h};
    }

    // --- recording control ------------------------------------------------------------------------------------
    recorder::SessionConfig MakeConfig() const {
        recorder::SessionConfig c;
        const capture::DisplayInfo* d = CurrentDisplay();
        if (d) c.display = *d;
        c.region = source == 0 ? RectI{} : region;
        const auto& r = settings.recording;
        c.fps = r.fps > 0 ? r.fps : 60;
        if (r.fps == 0 && d) c.fps = static_cast<int>(d->refresh_hz);
        c.codec = r.codec == "hevc" && hevc_hw ? media::VideoCodec::Hevc : media::VideoCodec::H264;
        c.quality = r.quality;
        c.mic = r.mic;
        c.mic_device = W(r.mic_device);
        c.system_audio = r.system_audio;
        c.system_device = system_device;
        c.webcam = !r.webcam_device.empty();
        c.webcam_synthetic = r.webcam_device == kSyntheticCameraId;
        if (!c.webcam_synthetic) c.webcam_device = W(r.webcam_device);
        c.webcam_size = ParseSize(r.webcam_resolution);
        c.output_dir = r.output_dir;
        c.countdown_s = opt.countdown_override >= 0 ? opt.countdown_override : r.countdown_s;
        c.keystrokes = r.keystrokes == "off" ? recorder::KeystrokeMode::Off
                       : r.keystrokes == "all" ? recorder::KeystrokeMode::All
                                               : recorder::KeystrokeMode::ShortcutsOnly;
        c.cursor_hz = r.cursor_hz;
        c.faults = opt.faults;
        return c;
    }

    RectI CaptureRect() const {
        const capture::DisplayInfo* d = CurrentDisplay();
        if (!d) return RectI{0, 0, 1920, 1080};
        return source == 0 ? d->rect : capture::ClampRegionToDisplay(*d, region);
    }

    // --- arming (P05 5.8 hotkey latency) ------------------------------------------------------------------------
    // With countdown 0 the session prepare (encoder creation, 300-800 ms) would sit between the hotkey and the first
    // frame, so the idle bar keeps the session armed for the current setup (RecordingSession::Arm: screen capturer and
    // encoder only; no microphone, camera or duplication is opened while idle). A frame (the bar draws only on input
    // or animation) that finds the armed setup out of date schedules a re-arm kArmDelayMs later; so do a finished
    // recording and display changes. Nothing runs while the bar is idle. With a countdown the prepare overlaps the
    // countdown, so nothing is armed.
    bool arm_timer = false;   // kArmTimer is pending
    int EffectiveCountdown() const {
        return opt.countdown_override >= 0 ? opt.countdown_override : settings.recording.countdown_s;
    }
    bool ArmOutOfDate() const {
        if (EffectiveCountdown() > 0 || opt.faults.Any()) return session.IsArmed();   // fault runs never arm
        return displays.empty() || !session.IsArmedFor(MakeConfig());
    }
    void ScheduleArm() {
        if (!win || arm_timer) return;
        arm_timer = SetTimer(win->Hwnd(), kArmTimer, kArmDelayMs, nullptr) != 0;
    }
    void ArmIfIdle() {
        if (win) KillTimer(win->Hwnd(), kArmTimer);
        arm_timer = false;
        const int countdown_s = EffectiveCountdown();
        const bool can_arm = dev && !displays.empty() && state == State::Idle && !preparing && !session.IsActive() &&
                             !countdown.IsRunning() && (source == 0 || !region.IsEmpty()) && countdown_s <= 0 &&
                             !opt.faults.Any();
        if (!can_arm) {
            if (session.IsArmed() && state == State::Idle && !preparing) session.Disarm();
            return;
        }
        const recorder::SessionConfig cfg = MakeConfig();
        if (session.IsArmedFor(cfg)) return;
        if (session.IsArmed()) session.Disarm();
        session.Arm(*dev, cfg, this);
    }

    void BeginRecording(int64_t trig) {
        if (state != State::Idle || session.IsActive() || preparing) return;
        if (displays.empty()) {
            displays = capture::EnumerateDisplays(dev->Factory());   // first launch race: a few ms, once
            if (displays.empty()) return;
        }
        if (source != 0 && region.IsEmpty()) {
            start_after_region = true;
            OpenRegionOverlay(source == 2 ? recorder::RegionOverlay::Mode::Window : recorder::RegionOverlay::Mode::Region);
            return;
        }
        close_popups = true;
        StopMonitor();
        trigger_qpc = trig ? trig : Clock::QpcNow();
        const recorder::SessionConfig cfg = MakeConfig();
        if (win) KillTimer(win->Hwnd(), kArmTimer);
        arm_timer = false;
        if (!preview && session.IsArmedFor(cfg)) {
            // Armed for exactly this setup: StartNow opens audio/camera after the capture started.
        } else {
            if (session.IsArmed()) session.Disarm();
            preparing = true;
            // The idle camera preview owns the device: release it before the session opens the camera.
            recorder::RecordingSession* s = &session;
            platform::D3DDevice* d = dev;
            recorder::SessionListener* l = this;
            if (preview) StopPreview([s, d, cfg, l]() { s->PrepareAsync(*d, cfg, l); });
            else session.PrepareAsync(*dev, cfg, this);
        }
        session.StartCountdown();
        const capture::DisplayInfo* disp = CurrentDisplay();
        const int secs = cfg.countdown_s;
        countdown_left = secs;
        if (secs <= 0) {
            session.StartNow(trigger_qpc);
            return;
        }
        std::string err;
        hotkeys.Register(win->Hwnd(), kHkEsc, 0, VK_ESCAPE, &err);   // Esc cancels even when another app has focus
        countdown.Start(CaptureRect(), disp ? disp->dpi : 96, secs, &Impl::CountdownDone, &Impl::CountdownTick, this);
    }

    static void CountdownDone(void* user, bool completed) {
        Impl* s = static_cast<Impl*>(user);
        s->hotkeys.Unregister(s->win->Hwnd(), kHkEsc);
        if (completed) s->session.StartNow(Clock::QpcNow());   // hotkey latency excludes the countdown
        else s->session.Cancel();
        s->Animate();
    }
    static void CountdownTick(void* user, int left) {
        Impl* s = static_cast<Impl*>(user);
        s->countdown_left = left;
        s->Animate(0.1f);
    }

    void Toggle(int64_t trig) {
        if (countdown.IsRunning()) {
            countdown.Cancel();
            return;
        }
        if (state == State::Recording || state == State::Paused) session.Stop();
        else if (state == State::Idle) BeginRecording(trig);
    }

    void OpenRegionOverlay(recorder::RegionOverlay::Mode mode) {
        if (displays.empty()) displays = capture::EnumerateDisplays(dev->Factory());
        const RectI* initial = region.IsEmpty() || mode == recorder::RegionOverlay::Mode::Window ? nullptr : &region;
        region_overlay.Open(displays, mode, &Impl::RegionDone, this, initial);
    }
    static void RegionDone(void* user, const recorder::RegionOverlay::Result& r) {
        Impl* s = static_cast<Impl*>(user);
        const bool start = s->start_after_region;
        s->start_after_region = false;
        if (r.confirmed) {
            s->region = r.region;
            s->region_display = r.display_index;
            if (start) s->BeginRecording(Clock::QpcNow());
        } else if (s->region.IsEmpty()) {
            s->source = 0;   // nothing selected: back to the monitor
        }
        s->Animate();
    }

    bool RegisterHotkeysChecked(std::string* error) {
        hotkeys.UnregisterAll();
        const struct { int id; Hotkey hk; } list[] = {{kHkToggle, settings.hotkeys.toggle}, {kHkPause, settings.hotkeys.pause},
                                                     {kHkMarker, settings.hotkeys.marker}, {kHkCancel, settings.hotkeys.cancel}};
        bool ok = true;
        for (const auto& e : list) {
            if (!e.hk.IsSet()) continue;
            std::string err;
            if (!hotkeys.Register(win->Hwnd(), e.id, e.hk.modifiers, e.hk.vk, &err)) {
                if (error && error->empty()) *error = err;
                ok = false;
            }
        }
        return ok;
    }

    void RegisterHotkeys() {
        hotkeys.UnregisterAll();
        const struct { int id; Hotkey hk; } list[] = {{kHkToggle, settings.hotkeys.toggle}, {kHkPause, settings.hotkeys.pause},
                                                     {kHkMarker, settings.hotkeys.marker}, {kHkCancel, settings.hotkeys.cancel}};
        for (const auto& e : list) {
            if (!e.hk.IsSet()) continue;
            std::string err;
            if (!hotkeys.Register(win->Hwnd(), e.id, e.hk.modifiers, e.hk.vk, &err)) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), str::kToastHotkeyConflict, err.c_str());
                PushToast(buf, ToastKind::Warning, {}, 8);
            }
        }
    }

    void TrayMenu(POINT at) {
        const bool rec = state == State::Recording || state == State::Paused;
        platform::TrayIcon::MenuItem items[] = {
            {kMenuRecord, str::kTrayRecord, state == State::Idle},
            {kMenuPause, state == State::Paused ? str::kTrayResume : str::kTrayPause, rec},
            {kMenuStop, str::kTrayStop, rec},
            {0},
            {kMenuShow, str::kTrayShowBar, true, visible},
            {kMenuEditor, str::kTrayOpenEditor, true},
            {kMenuSettings, str::kTraySettings, true},
            {0},
            {kMenuQuit, str::kTrayQuit, true},
        };
        switch (platform::TrayIcon::ShowMenu(win->Hwnd(), at, items, std::size(items))) {
        case kMenuRecord: Toggle(Clock::QpcNow()); break;
        case kMenuPause: session.TogglePause(); break;
        case kMenuStop: session.Stop(); break;
        case kMenuShow:
            ShowBar(!visible);
            if (visible) FocusForKeyboard();
            break;
        case kMenuEditor: if (hooks.open_editor) hooks.open_editor(last_bundle); break;
        case kMenuSettings: OpenSettings(); break;
        case kMenuQuit: Quit(); break;
        default: break;
        }
        Animate();
    }

    void OpenSettings() {
        if (hooks.open_settings) {
            hooks.open_settings();
            return;
        }
        if (!settings_win) return;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(win->Hwnd(), MONITOR_DEFAULTTOPRIMARY), &mi);
        const float w = std::min(Dp(840), static_cast<float>(mi.rcWork.right - mi.rcWork.left) - Dp(32));
        const float h = std::min(Dp(580), static_cast<float>(mi.rcWork.bottom - mi.rcWork.top) - Dp(32));
        settings_win->SetHostRect(ImVec2((mi.rcWork.left + mi.rcWork.right - w) * 0.5f, (mi.rcWork.top + mi.rcWork.bottom - h) * 0.5f),
                                  ImVec2(w, h));
        if (!settings_win->IsOpen()) settings_win->Open(settings);
        settings_focus = true;
        Animate();
    }

    void Quit() {
        if (session.IsActive()) {
            quit_after_finish = true;
            if (countdown.IsRunning()) countdown.Cancel();
            else session.Stop();
            return;
        }
        if (hooks.quit) hooks.quit();
    }

    // --- frame ----------------------------------------------------------------------------------------------------
    void UpdateMeters() {
        const float t = NowS();
        const float dt = std::clamp(t - last_meter_t, 0.0f, 0.5f);
        last_meter_t = t;
        float mic_db = -120, sys_db = -120;
        if (state == State::Recording || state == State::Paused) {
            session.LiveLevels(&mic_db, &sys_db);
        } else if (monitor_audio) {
            (monitor_for == Popup::Mic ? mic_db : sys_db) = monitor_audio->Peak();
        }
        shown_mic = std::max(mic_db, shown_mic - 20.0f * dt);
        shown_sys = std::max(sys_db, shown_sys - 20.0f * dt);
    }

    void DrawBackground(ImDrawList* dl, ImVec2 a, ImVec2 b) {
        dl->AddRectFilled(a, b, col::bg1);
        // DWM rounds the window (DWMWCP_ROUND, 8 px) and draws its border in the border token colour.
        (void)b;
    }

    void Frame() {
        if (state == State::Idle && !preparing && ArmOutOfDate()) ScheduleArm();
        UpdateMeters();
        UpdateWindowSize();
        if (preview && preview->failed.load()) {
            preview.reset();
            PushToast(std::string(str::kBarNoCamera), ToastKind::Warning);
        }

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoScrollWithMouse;   // Tab/arrows/Enter reach every control (P11 11.6)
        ImGui::Begin("##recorder_bar", nullptr, flags);
        ImGui::PopStyleVar(4);
        open_fps = ImGui::IsPopupOpen("##pop_fps");
        open_mic = ImGui::IsPopupOpen("##pop_mic");
        open_sys = ImGui::IsPopupOpen("##pop_sys");
        open_cam = ImGui::IsPopupOpen("##pop_cam");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
        DrawBackground(dl, a, b);
        const bool compact = state != State::Idle || size_to_w == kRecW;
        if (compact) DrawRecording(dl, a, b);
        else DrawIdle(dl, a, b);
        DrawPopups();
        UpdateNavLabel();
        ImGui::End();
        DrawToasts(DrawErrorPanel());
        DrawCameraHover();
        DrawSettings();
    }

    void DrawSettings() {
        if (!settings_win || !settings_win->IsOpen()) return;
        if (ImGuiWindow* w = ImGui::FindWindowByName(SettingsWindow::WindowName())) {
            ImGuiViewport* vp = w->Viewport;
            if (vp && vp != ImGui::GetMainViewport() && vp->PlatformWindowCreated) {
                if (vp->PlatformRequestClose) {
                    vp->PlatformRequestClose = false;
                    settings_win->Close();
                    return;
                }
                settings_win->SetHostRect(vp->Pos, vp->Size);   // follow OS moves/resizes of the window
                if (settings_focus) {
                    settings_focus = false;
                    if (vp->PlatformHandle) SetForegroundWindow(static_cast<HWND>(vp->PlatformHandle));
                }
            }
        }
        ImGuiWindowClass cls;
        cls.ParentViewportId = 0;   // a normal top-level window, not owned by the topmost bar
        cls.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&cls);
        settings_win->Draw();
    }

    // Idle bar ---------------------------------------------------------------------------------------------------
    void DrawIdle(ImDrawList* dl, ImVec2 a, ImVec2 b) {
        const float h = Dp(32);
        const float y = std::floor((a.y + b.y - h) * 0.5f);
        float x = a.x + Dp(8);
        const float gap = Dp(4);

        // Grip (native drag via WM_NCHITTEST).
        {
            const float w = Dp(14);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::Dummy(ImVec2(w, h));
            GripGlyph(dl, ImVec2(x + w * 0.5f, y + h * 0.5f), col::text_faint);
            SetGrip(ImVec2(x - Dp(6), a.y), ImVec2(x + w + Dp(2), b.y));
            x += w + gap;
        }
        VSeparator(dl, x, y + Dp(6), y + h - Dp(6));
        x += gap + Dp(1);

        // Source segmented control: [Monitor N v][region][window].
        {
            const float pad = Dp(3);
            char label[48];
            std::snprintf(label, sizeof(label), str::kBarMonitor, monitor + 1);
            float mon_w;
            {
                FontScope f(FontRole::Label);
                mon_w = Dp(26) + ImGui::CalcTextSize(label).x + Dp(18);
            }
            const float seg_w[3] = {mon_w, Dp(30), Dp(30)};
            const float total = pad * 2 + seg_w[0] + seg_w[1] + seg_w[2];
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + total, y + h), col::bg0, Dp(kRadiusControl + 2));
            dl->AddRect(ImVec2(x, y), ImVec2(x + total, y + h), col::border, Dp(kRadiusControl + 2));
            float sx = x + pad;
            for (int i = 0; i < 3; ++i) {
                ImGui::SetCursorScreenPos(ImVec2(sx, y + pad));
                bool hovered = false;
                ImGui::PushID(i);
                if (i == 0 && focus_first) {   // shown from the keyboard (tray Enter): Tab starts here
                    ImGui::SetKeyboardFocusHere();
                    focus_first = false;
                }
                const bool pressed = Chip("##src", ImVec2(seg_w[i], h - pad * 2), source == i, &hovered);
                NoteNav(i == 0 ? "source-monitor" : i == 1 ? "source-region" : "source-window");
                ImGui::PopID();
                const ImU32 fg = source == i || hovered ? col::text : col::text_dim;
                const ImVec2 sa(sx, y + pad), sb(sx + seg_w[i], y + h - pad);
                const ImVec2 c((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f);
                if (i == 0) {
                    Icon(dl, ImVec2(sa.x + Dp(13), c.y), fg, ICON_MONITOR);
                    FontScope f(FontRole::Label);
                    const ImVec2 ts = ImGui::CalcTextSize(label);
                    dl->AddText(ImVec2(sa.x + Dp(24), std::floor(c.y - ts.y * 0.5f)), fg, label);
                    Icon(dl, ImVec2(sb.x - Dp(9), c.y + Dp(1)), col::text_faint, ICON_CHEVRON_DOWN, FontRole::Label);
                    ItemTooltip(str::kBarMonitorTip);
                    if (pressed) {
                        source = 0;
                        OpenPopupNow(Popup::Monitor);
                    }
                } else if (i == 1) {
                    RegionGlyph(dl, c, fg);
                    ItemTooltip(str::kBarRegionTip);
                    if (pressed) {
                        source = 1;
                        OpenRegionOverlay(recorder::RegionOverlay::Mode::Region);
                    }
                } else {
                    WindowGlyph(dl, c, fg);
                    ItemTooltip(str::kBarWindowTip);
                    if (pressed) {
                        source = 2;
                        region = RectI{};
                        OpenRegionOverlay(recorder::RegionOverlay::Mode::Window);
                    }
                }
                sx += seg_w[i];
            }
            monitor_anchor_x = x + seg_w[0] * 0.5f;
            x += total + gap;
        }
        VSeparator(dl, x, y + Dp(6), y + h - Dp(6));
        x += gap + Dp(1);

        // fps chip.
        {
            char label[24];
            const int fps = settings.recording.fps;
            if (fps == 0) std::snprintf(label, sizeof(label), "%s", str::kBarFpsNative);
            else std::snprintf(label, sizeof(label), str::kBarFpsSuffix, fps);
            float tw;
            {
                FontScope f(FontRole::Label);
                tw = ImGui::CalcTextSize(label).x;
            }
            const float w = Dp(8) + tw + Dp(16);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            bool hovered = false;
            const bool pressed = Chip("##fps", ImVec2(w, h), open_fps, &hovered);
            NoteNav("fps");
            {
                FontScope f(FontRole::Label);
                dl->AddText(ImVec2(x + Dp(8), std::floor(y + (h - ImGui::GetFontSize()) * 0.5f)), hovered ? col::text : col::text_dim, label);
            }
            Icon(dl, ImVec2(x + w - Dp(9), y + h * 0.5f + Dp(1)), col::text_faint, ICON_CHEVRON_DOWN, FontRole::Label);
            ItemTooltip(str::kBarFpsTip);
            if (pressed) OpenPopupNow(Popup::Fps);
            fps_anchor_x = x + w * 0.5f;
            x += w + gap;
        }

        // Mic, system, camera chips: toggle area (icon + meter) + chevron.
        x = AudioChip(dl, x, y, h, Popup::Mic) + gap;
        x = AudioChip(dl, x, y, h, Popup::System) + gap;
        x = CameraChip(dl, x, y, h) + gap;

        // Right side: REC, settings, close, laid out from the right edge but submitted left to right so that Tab
        // follows the visual order (P11 11.6 keyboard pass).
        const float icon_w = Dp(28);
        const float close_x = b.x - Dp(8) - icon_w;
        const float settings_x = close_x - Dp(2) - icon_w;
        float rx = settings_x - gap;
        {
            float tw;
            {
                FontScope f(FontRole::Strong);
                tw = ImGui::CalcTextSize(str::kBarRec).x;
            }
            const float w = Dp(14) + Dp(8) + Dp(6) + tw + Dp(14);
            rx -= w;
            const float rh = Dp(32);
            const float ry = std::floor((a.y + b.y - rh) * 0.5f);
            ImGui::SetCursorScreenPos(ImVec2(rx, ry));
            bool hovered = false;
            const bool busy = preparing || state != State::Idle;
            const ImU32 fill = busy ? WithAlpha(col::record, 0.5f) : hovered_rec ? Mix(col::record, Rgb(0xFFFFFF), 0.12f) : col::record;
            const bool pressed = Chip("##rec", ImVec2(w, rh), false, &hovered, fill);
            NoteNav("rec");
            hovered_rec = hovered;
            if (hovered) PrewarmEncoder();
            dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + w, ry + rh), fill, Dp(rh * 0.5f));
            const ImVec2 dot(rx + Dp(14) + Dp(4), ry + rh * 0.5f);
            dl->AddCircleFilled(dot, Dp(5), Rgb(0xFFFFFF), 20);
            {
                FontScope f(FontRole::Strong);
                dl->AddText(ImVec2(dot.x + Dp(10), std::floor(ry + (rh - ImGui::GetFontSize()) * 0.5f)), Rgb(0xFFFFFF), str::kBarRec);
            }
            {
                char tip[96];
                std::snprintf(tip, sizeof(tip), "%s", str::kBarRecTip);
                ItemTooltip(tip, settings.hotkeys.toggle.IsSet() ? HotkeyToString(settings.hotkeys.toggle).c_str() : nullptr);
            }
            if (pressed && !busy) BeginRecording(Clock::QpcNow());
        }
        ImGui::SetCursorScreenPos(ImVec2(settings_x, y + (h - icon_w) * 0.5f));
        if (IconButton("##settings", ICON_SETTINGS, str::kBarSettingsTip, false, nullptr, ImVec2(icon_w, icon_w))) OpenSettings();
        NoteNav("settings");
        ImGui::SetCursorScreenPos(ImVec2(close_x, y + (h - icon_w) * 0.5f));
        if (IconButton("##close", ICON_CLOSE, str::kBarCloseTip, false, nullptr, ImVec2(icon_w, icon_w))) ShowBar(false);
        NoteNav("close");
        (void)x;
    }

    float AudioChip(ImDrawList* dl, float x, float y, float h, Popup which) {
        const bool is_mic = which == Popup::Mic;
        bool& on = is_mic ? settings.recording.mic : settings.recording.system_audio;
        const bool available = is_mic ? !mics.empty() || !devices_listed : !outputs.empty() || !devices_listed;
        const float w = Dp(34), cw = Dp(16);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool hovered = false;
        ImGui::PushID(is_mic ? "mic" : "sys");
        const bool pressed = Chip("##toggle", ImVec2(w, h), false, &hovered);
        NoteNav(is_mic ? "mic" : "system");
        const ImU32 fg = !available ? col::text_faint : on ? (hovered ? col::text : col::text_dim) : col::text_faint;
        const ImVec2 ic(x + w * 0.5f, y + Dp(13));
        Icon(dl, ic, fg, is_mic ? ICON_MICROPHONE : ICON_VOLUME);
        if (!on || !available) Strike(dl, ic, Dp(6), fg);
        const float level = is_mic ? shown_mic : shown_sys;
        const bool live = on && available && (state != State::Idle || monitor_for == which);
        MiniMeter(dl, ImVec2(x + Dp(7), y + h - Dp(8)), ImVec2(w - Dp(14), Dp(3)), live ? level : -120.0f, on && available);
        ItemTooltip(is_mic ? str::kBarMicTip : str::kBarSystemTip);
        if (pressed && available) {
            on = !on;
            SaveSettingsAsync();
            if (!on && monitor_for == which) StopMonitor();
        }
        ImGui::SetCursorScreenPos(ImVec2(x + w, y));
        bool hov2 = false;
        const bool chev = Chip("##chev", ImVec2(cw, h), is_mic ? open_mic : open_sys, &hov2);
        NoteNav(is_mic ? "mic-devices" : "system-devices");
        Icon(dl, ImVec2(x + w + cw * 0.5f, y + h * 0.5f + Dp(1)), hov2 ? col::text : col::text_faint, ICON_CHEVRON_DOWN, FontRole::Label);
        ItemTooltip(str::kBarDevicesTip);
        if (chev) OpenPopupNow(which);
        ImGui::PopID();
        (is_mic ? mic_anchor_x : sys_anchor_x) = x + (w + cw) * 0.5f;
        return x + w + cw;
    }

    float CameraChip(ImDrawList* dl, float x, float y, float h) {
        const bool on = !settings.recording.webcam_device.empty();
        float tw = 0;
        if (!on) {
            FontScope f(FontRole::Label);
            tw = ImGui::CalcTextSize(str::kBarCameraOff).x;
        }
        const float w = on ? Dp(34) : Dp(28) + tw + Dp(6), cw = Dp(16);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool hovered = false;
        const bool pressed = Chip("##cam", ImVec2(w, h), false, &hovered);
        NoteNav("camera");
        camera_hovered = hovered && on;
        const ImU32 fg = on ? (hovered ? col::text : col::accent_hover) : hovered ? col::text_dim : col::text_faint;
        if (on) {
            Icon(dl, ImVec2(x + w * 0.5f, y + h * 0.5f), fg, ICON_CAMERA);
            dl->AddCircleFilled(ImVec2(x + w - Dp(8), y + Dp(9)), Dp(2.5f), col::success, 12);
        } else {
            Icon(dl, ImVec2(x + Dp(14), y + h * 0.5f), fg, ICON_CAMERA);
            FontScope f(FontRole::Label);
            dl->AddText(ImVec2(x + Dp(26), std::floor(y + (h - ImGui::GetFontSize()) * 0.5f)), fg, str::kBarCameraOff);
        }
        if (!on) ItemTooltip(str::kBarCameraTip);
        if (pressed) {
            if (on) {
                settings.recording.webcam_device.clear();
                StopPreview();
            } else if (!cameras.empty()) {
                settings.recording.webcam_device = WideToUtf8(cameras[0].symbolic_link);
            } else {
                OpenPopupNow(Popup::Camera);
            }
            SaveSettingsAsync();
        }
        ImGui::SetCursorScreenPos(ImVec2(x + w, y));
        bool hov2 = false;
        const bool chev = Chip("##camchev", ImVec2(cw, h), open_cam, &hov2);
        NoteNav("camera-devices");
        Icon(dl, ImVec2(x + w + cw * 0.5f, y + h * 0.5f + Dp(1)), hov2 ? col::text : col::text_faint, ICON_CHEVRON_DOWN, FontRole::Label);
        ItemTooltip(str::kBarDevicesTip);
        if (chev) OpenPopupNow(Popup::Camera);
        cam_anchor_x = x + (w + cw) * 0.5f;
        cam_chip_min = ImVec2(x, y);
        cam_chip_max = ImVec2(x + w, y + h);
        return x + w + cw;
    }

    // Recording bar ----------------------------------------------------------------------------------------------
    void DrawRecording(ImDrawList* dl, ImVec2 a, ImVec2 b) {
        const float h = Dp(28);
        const float y = std::floor((a.y + b.y - h) * 0.5f);
        float x = a.x + Dp(12);
        const float gap = Dp(4);
        const bool paused = state == State::Paused;
        const bool recording = state == State::Recording || paused;

        // Pulsing dot (1.2 s loop) + timer / status.
        {
            const float phase = std::fmod(NowS() - rec_started_at, 1.2f) / 1.2f;
            const float pulse = recording && !paused ? 0.55f + 0.45f * std::cos(phase * 6.2831853f) : 1.0f;
            const ImU32 dot_col = paused ? col::warn : state == State::Finalizing ? col::text_faint : WithAlpha(col::record, pulse);
            const ImVec2 c(x + Dp(5), y + h * 0.5f);
            if (recording && !paused) dl->AddCircleFilled(c, Dp(5) + Dp(3) * (1 - pulse), WithAlpha(col::record, 0.18f), 20);
            dl->AddCircleFilled(c, Dp(5), dot_col, 20);
            char text[48];
            const char* status = text;
            if (state == State::Countdown) std::snprintf(text, sizeof(text), "%d", std::max(countdown_left, 1));
            else if (state == State::Starting || (state == State::Idle && preparing)) status = str::kBarPreparing;
            else if (state == State::Finalizing || state == State::Error) status = str::kBarFinalizing;
            else FormatClock(text, sizeof(text), stats.elapsed_us);
            FontScope f(state == State::Recording || paused ? FontRole::Strong : FontRole::Label);
            const ImU32 tc = paused && std::fmod(NowS(), 1.0f) > 0.5f ? col::text_dim : col::text;
            dl->AddText(ImVec2(x + Dp(16), std::floor(y + (h - ImGui::GetFontSize()) * 0.5f)), tc, status);
            if (state == State::Finalizing) {
                const float sa = NowS() * 6.0f;
                const ImVec2 sc(x + Dp(16) + ImGui::CalcTextSize(status).x + Dp(12), y + h * 0.5f);
                dl->PathArcTo(sc, Dp(5), sa, sa + 4.2f, 16);
                dl->PathStroke(col::accent, Dp(1.6f));
            }
            x += Dp(16) + Dp(66) + gap;
        }
        VSeparator(dl, x, y + Dp(5), y + h - Dp(5));
        x += gap + Dp(1);
        const float bw = Dp(28);
        ImGui::BeginDisabled(!recording);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (IconButton("##pause", paused ? ICON_PLAY : ICON_PAUSE, paused ? str::kBarResumeTip : str::kBarPauseTip, paused,
                       nullptr, ImVec2(bw, h)))
            session.TogglePause();
        x += bw + Dp(2);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (IconButton("##stop", ICON_STOP, str::kBarStopTip, false, nullptr, ImVec2(bw, h))) session.Stop();
        x += bw + Dp(2);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (IconButton("##marker", ICON_FLAG, str::kBarMarkerTip, false, nullptr, ImVec2(bw, h))) MarkerNow();
        ImGui::EndDisabled();
        if (state == State::Countdown) {   // the stop button cancels the countdown
            ImGui::SetCursorScreenPos(ImVec2(x - bw - Dp(2), y));
            if (IconButton("##cancel", ICON_STOP, str::kBarStopTip, false, nullptr, ImVec2(bw, h))) countdown.Cancel();
        }
        x += bw + gap;
        VSeparator(dl, x, y + Dp(5), y + h - Dp(5));
        x += gap + Dp(1);
        // Mic (or system) meter.
        {
            const bool mic = stats.has_mic || (!stats.has_system && settings.recording.mic);
            const float level = mic ? shown_mic : shown_sys;
            const bool on = mic ? stats.has_mic : stats.has_system;
            Icon(dl, ImVec2(x + Dp(8), y + h * 0.5f), on ? col::text_dim : col::text_faint, mic ? ICON_MICROPHONE : ICON_VOLUME);
            if (!on) Strike(dl, ImVec2(x + Dp(8), y + h * 0.5f), Dp(6), col::text_faint);
            MiniMeter(dl, ImVec2(x + Dp(19), y + h * 0.5f - Dp(2)), ImVec2(Dp(22), Dp(4)), level, on);
            x += Dp(44);
        }
        // Grip on the right.
        const float gx = b.x - Dp(12) - Dp(12);
        GripGlyph(dl, ImVec2(gx + Dp(6), y + h * 0.5f), col::text_faint);
        SetGrip(ImVec2(gx - Dp(6), a.y), ImVec2(b.x, b.y));
        (void)x;
    }

    void SetGrip(ImVec2 min, ImVec2 max) {
        const ImVec2 o = ImGui::GetMainViewport()->Pos;
        grip_px = RECT{static_cast<LONG>(min.x - o.x), static_cast<LONG>(min.y - o.y), static_cast<LONG>(max.x - o.x),
                       static_cast<LONG>(max.y - o.y)};
    }

    // Popovers ---------------------------------------------------------------------------------------------------
    float monitor_anchor_x = 0, fps_anchor_x = 0, mic_anchor_x = 0, sys_anchor_x = 0, cam_anchor_x = 0;
    ImVec2 cam_chip_min{}, cam_chip_max{};
    bool camera_hovered = false, hovered_rec = false, close_popups = false, settings_focus = false;
    bool caps_started = false, prewarmed = false;
    bool open_fps = false, open_mic = false, open_sys = false, open_cam = false;

    static const char* PopupId(Popup p) {
        switch (p) {
        case Popup::Monitor: return "##pop_monitor";
        case Popup::Fps: return "##pop_fps";
        case Popup::Mic: return "##pop_mic";
        case Popup::System: return "##pop_sys";
        case Popup::Camera: return "##pop_cam";
        default: return nullptr;
        }
    }

    // Popup ids depend on the ID stack: requests are queued and opened at the bar window's root in DrawPopups.
    void OpenPopupNow(Popup p) {
        open_request = p;
        Animate();
    }

    // Places the next popover above the bar (below it when the bar sits at the top of the screen).
    void PlacePopover(float anchor_x) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(win->Hwnd(), MONITOR_DEFAULTTONEAREST), &mi);
        const bool below = vp->Pos.y - mi.rcWork.top < Px(260);
        const float y = below ? vp->Pos.y + vp->Size.y + Dp(8) : vp->Pos.y - Dp(8);
        ImGui::SetNextWindowPos(ImVec2(anchor_x, y), ImGuiCond_Always, ImVec2(0.5f, below ? 0.0f : 1.0f));
    }

    bool RowItem(const char* label, bool selected, const char* right = nullptr, bool enabled = true) {
        ImGui::BeginDisabled(!enabled);
        const float w = std::max(ImGui::GetContentRegionAvail().x, Dp(240));
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::Selectable(label, false, ImGuiSelectableFlags_None, ImVec2(w, Dp(26)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected) Icon(dl, ImVec2(pos.x + w - Dp(10), pos.y + Dp(13)), col::accent, ICON_CHECK, FontRole::Label);
        if (right) {
            FontScope f(FontRole::Label);
            const ImVec2 ts = ImGui::CalcTextSize(right);
            dl->AddText(ImVec2(pos.x + w - Dp(24) - ts.x, pos.y + (Dp(26) - ts.y) * 0.5f), col::text_faint, right);
        }
        ImGui::EndDisabled();
        return pressed;
    }

    void PopoverTitle(const char* title) {
        FontScope f(FontRole::Strong);
        ImGui::TextUnformatted(title);
        ImGui::Dummy(ImVec2(0, Dp(2)));
    }

    void DrawPopups() {
        if (open_request != Popup::None) {
            if (const char* id = PopupId(open_request)) ImGui::OpenPopup(id);
            open_request = Popup::None;
        }
        if (close_popups) {
            ImGui::ClosePopupsOverWindow(nullptr, false);
            close_popups = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Dp(8, 6));
        // Monitors.
        PlacePopover(monitor_anchor_x);
        if (BeginPopover("##pop_monitor")) {
            PopoverTitle(str::kBarMonitorsTitle);
            DrawMonitorLayout();
            for (const auto& d : displays) {
                char label[160], right[64];
                std::snprintf(label, sizeof(label), "%d   %s##m%d", d.index + 1,
                              d.friendly_name.empty() ? WideToUtf8(d.device_name).c_str() : d.friendly_name.c_str(), d.index);
                std::snprintf(right, sizeof(right), "%d x %d  %u Hz%s", d.rect.w, d.rect.h, d.refresh_hz,
                              d.primary ? "  \xC2\xB7  primary" : "");
                if (RowItem(label, source == 0 && monitor == d.index, right)) {
                    monitor = d.index;
                    source = 0;
                    ImGui::CloseCurrentPopup();
                }
            }
            {
                FontScope f(FontRole::Label);
                ImGui::TextColored(ToVec4(col::text_faint), "%s", str::kBarMonitorHint);
            }
            EndPopover();
        }
        // fps / quality / codec.
        PlacePopover(fps_anchor_x);
        if (BeginPopover("##pop_fps")) {
            auto& r = settings.recording;
            PopoverTitle(str::kBarFpsTitle);
            static const char* fps_items[] = {"15", "24", "30", "60", str::kBarFpsNative};
            int fps_idx = 3;
            for (int i = 0; i < 5; ++i)
                if (kFpsChoices[i] == r.fps) fps_idx = i;
            if (SegmentedControl("##fpsseg", &fps_idx, fps_items, 5, Dp(52))) {
                r.fps = kFpsChoices[fps_idx];
                SaveSettingsAsync();
            }
            ImGui::Dummy(ImVec2(0, Dp(4)));
            PopoverTitle(str::kBarQualityTitle);
            static const char* q_items[] = {str::kBarQualityGood, str::kBarQualityHigh, str::kBarQualityBest};
            int q_idx = r.quality >= 80 ? 2 : r.quality >= 66 ? 1 : 0;
            if (SegmentedControl("##qseg", &q_idx, q_items, 3, Dp(86.6f))) {
                r.quality = kQualityValues[q_idx];
                SaveSettingsAsync();
            }
            ImGui::Dummy(ImVec2(0, Dp(4)));
            PopoverTitle(str::kBarCodecTitle);
            static const char* c_items[] = {str::kBarCodecH264, str::kBarCodecHevc};
            int c_idx = r.codec == "hevc" ? 1 : 0;
            ImGui::BeginDisabled(!hevc_hw);
            if (SegmentedControl("##cseg", &c_idx, c_items, 2, Dp(130))) {
                r.codec = c_idx == 1 ? "hevc" : "h264";
                SaveSettingsAsync();
            }
            ImGui::EndDisabled();
            if (!hevc_hw) {
                FontScope f(FontRole::Label);
                ImGui::TextColored(ToVec4(col::text_faint), "%s", str::kBarHevcUnavailable);
            }
            EndPopover();
        }
        // Mic and system device lists (with a live meter while open).
        for (Popup p : {Popup::Mic, Popup::System}) {
            const bool is_mic = p == Popup::Mic;
            PlacePopover(is_mic ? mic_anchor_x : sys_anchor_x);
            if (BeginPopover(PopupId(p))) {
                StartMonitor(p);
                bool& on = is_mic ? settings.recording.mic : settings.recording.system_audio;
                {
                    FontScope f(FontRole::Strong);
                    ImGui::TextUnformatted(is_mic ? str::kBarMicTip : str::kBarSystemTip);
                }
                ImGui::SameLine(Dp(240) - Dp(30));
                if (ToggleSwitch("##on", &on)) {
                    SaveSettingsAsync();
                    StopMonitor();
                }
                const auto& list = is_mic ? mics : outputs;
                if (!devices_listed) {
                    ImGui::Dummy(Dp(240, 26));   // the lists arrive from the IO pool in a few ms
                } else if (list.empty()) {
                    ImGui::TextColored(ToVec4(col::text_faint), "%s", is_mic ? str::kBarNoMic : str::kBarNoOutput);
                } else {
                    const std::wstring current = is_mic ? W(settings.recording.mic_device) : system_device;
                    if (RowItem(str::kBarDefaultDevice, current.empty())) SelectDevice(is_mic, L"");
                    for (size_t i = 0; i < list.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        if (RowItem(list[i].name.c_str(), current == list[i].id)) SelectDevice(is_mic, list[i].id);
                        ImGui::PopID();
                    }
                    ImGui::Dummy(ImVec2(0, Dp(2)));
                    MeterBar("##meter", on ? (is_mic ? shown_mic : shown_sys) : -120.0f, ImVec2(Dp(240), Dp(6)));
                }
                EndPopover();
            } else if (monitor_for == p) {
                StopMonitor();
            }
        }
        // Camera devices + resolution + preview.
        PlacePopover(cam_anchor_x);
        if (BeginPopover("##pop_cam")) {
            preview_wanted_until = NowS() + 0.5f;
            if (!settings.recording.webcam_device.empty()) StartPreview();
            PopoverTitle(str::kBarCameraTip);
            if (cameras.empty()) {
                ImGui::TextColored(ToVec4(col::text_faint), "%s", str::kBarNoCamera);
            } else {
                if (RowItem(str::kBarCameraOff, settings.recording.webcam_device.empty())) {
                    settings.recording.webcam_device.clear();
                    StopPreview();
                    SaveSettingsAsync();
                }
                for (size_t i = 0; i < cameras.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const std::string id = WideToUtf8(cameras[i].symbolic_link);
                    if (RowItem(cameras[i].name.c_str(), settings.recording.webcam_device == id)) {
                        if (settings.recording.webcam_device != id) StopPreview();
                        settings.recording.webcam_device = id;
                        SaveSettingsAsync();
                    }
                    ImGui::PopID();
                }
                const capture::WebcamInfo* cur = nullptr;
                for (const auto& c : cameras)
                    if (WideToUtf8(c.symbolic_link) == settings.recording.webcam_device) cur = &c;
                if (cur && !cur->sizes.empty()) {
                    ImGui::Separator();
                    for (const SizeI& s : cur->sizes) {
                        char label[32];
                        std::snprintf(label, sizeof(label), "%d x %d", s.w, s.h);
                        if (RowItem(label, ParseSize(settings.recording.webcam_resolution).w == s.w &&
                                               ParseSize(settings.recording.webcam_resolution).h == s.h)) {
                            settings.recording.webcam_resolution = std::to_string(s.w) + "x" + std::to_string(s.h);
                            StopPreview();
                            SaveSettingsAsync();
                        }
                    }
                }
                if (cur) DrawPreviewImage();
            }
            EndPopover();
        }
        ImGui::PopStyleVar();
        if (preview && NowS() > preview_wanted_until && !camera_hovered) StopPreview();
    }

    void MarkerNow() {
        if (state == State::Recording) session.AddMarker(str::kBarMarkerLabel);
    }

    void SelectDevice(bool is_mic, const std::wstring& id) {
        if (is_mic) settings.recording.mic_device = WideToUtf8(id);
        else system_device = id;
        SaveSettingsAsync();
        StopMonitor();
    }

    void DrawMonitorLayout() {
        if (displays.empty()) return;
        RectI all = displays[0].rect;
        for (const auto& d : displays) {
            const int l = std::min(all.x, d.rect.x), t = std::min(all.y, d.rect.y);
            const int r = std::max(all.Right(), d.rect.Right()), btm = std::max(all.Bottom(), d.rect.Bottom());
            all = RectI{l, t, r - l, btm - t};
        }
        const float box_w = Dp(240), box_h = Dp(70);
        const float s = std::min(box_w / all.w, box_h / all.h);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float ox = o.x + (box_w - all.w * s) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (const auto& d : displays) {
            const ImVec2 a(ox + (d.rect.x - all.x) * s + Dp(2), o.y + (d.rect.y - all.y) * s + Dp(2));
            const ImVec2 b(ox + (d.rect.Right() - all.x) * s - Dp(2), o.y + (d.rect.Bottom() - all.y) * s - Dp(2));
            const bool sel = source == 0 ? monitor == d.index : region_display == d.index;
            dl->AddRectFilled(a, b, sel ? col::accent_soft : col::bg3, Dp(4));
            dl->AddRect(a, b, sel ? col::accent : col::border, Dp(4), sel ? Dp(1.5f) : 1.0f);
            char n[8];
            std::snprintf(n, sizeof(n), "%d", d.index + 1);
            FontScope f(FontRole::Strong);
            DrawCentered(dl, a, b, sel ? col::text : col::text_dim, n);
            if (source != 0 && region_display == d.index && !region.IsEmpty()) {
                const ImVec2 ra(ox + (region.x - all.x) * s, o.y + (region.y - all.y) * s);
                const ImVec2 rb(ox + (region.Right() - all.x) * s, o.y + (region.Bottom() - all.y) * s);
                dl->AddRect(ra, rb, col::record, 0, 1.0f);
            }
        }
        ImGui::Dummy(ImVec2(box_w, all.h * s + Dp(4)));
    }

    void DrawPreviewImage() {
        const ImVec2 size = Dp(160, 90);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), col::bg0, Dp(6));
        if (preview_has_frame && preview_srv) {
            dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(preview_srv.Get()))), pos,
                                ImVec2(pos.x + size.x, pos.y + size.y), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, Dp(6));
        } else {
            FontScope f(FontRole::Label);
            DrawCentered(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), col::text_faint, str::kBarCameraStarting);
        }
        ImGui::Dummy(size);
    }

    // Live preview on hover of the camera chip (docs/05: "when on, a 160x90 live preview popover appears on hover").
    void DrawCameraHover() {
        if (!camera_hovered || open_cam) return;
        StartPreview();
        preview_wanted_until = NowS() + 0.5f;
        PlacePopover(cam_anchor_x);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusCard));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg2));
        ImGui::Begin("##cam_hover", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                                                 ImGuiWindowFlags_NoDocking);
        DrawPreviewImage();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }

    // Keyboard diagnostics (--bar-keys): remembers which control holds the nav cursor this frame.
    void NoteNav(const char* label) {
        if (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible) frame_nav_label = label;
    }
    void UpdateNavLabel() {
        nav_label = frame_nav_label;
        frame_nav_label.clear();
    }

    // Error panel (P11 11.5): the ErrorBanner in its own capture-excluded viewport at the bottom-right of the work
    // area, where the toasts go; it stays until dismissed or resolved. Returns the height it takes (0 when hidden).
    float DrawErrorPanel() {
        if (!error_banner.Visible()) return 0;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(win->Hwnd(), MONITOR_DEFAULTTOPRIMARY), &mi);
        const float w = Dp(440);
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(mi.rcWork.right) - Dp(16), static_cast<float>(mi.rcWork.bottom) - Dp(16)),
                                ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);   // 0: fit the height every frame
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(6, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusCard));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Dp(8, 8));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg2));
        ImGui::Begin("##bar_error", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
        error_banner.Draw("##bar_error_banner");
        const float h = ImGui::GetWindowHeight();
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
        if (const int a = error_banner.TakeAction()) RunErrorAction(a);
        return error_banner.Visible() ? h + Dp(8) : 0;
    }

    // Toasts (bottom-right of the work area of the bar's monitor, newest at the bottom), above the error panel.
    void DrawToasts(float bottom_offset) {
        if (toasts.empty()) return;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(win->Hwnd(), MONITOR_DEFAULTTOPRIMARY), &mi);
        float y = static_cast<float>(mi.rcWork.bottom) - Dp(16) - bottom_offset;
        const float x = static_cast<float>(mi.rcWork.right) - Dp(16);
        const float now = NowS();
        int remove_id = 0;
        for (int i = static_cast<int>(toasts.size()) - 1; i >= 0; --i) {
            ToastItem& t = toasts[static_cast<size_t>(i)];
            const float age = now - t.shown_at;
            if (age > t.seconds) {
                remove_id = t.id;
                continue;
            }
            const float in = EaseOut(std::clamp(age * 1000.0f / kMotionMs, 0.0f, 1.0f));
            const float out = std::clamp((t.seconds - age) * 1000.0f / kMotionMs, 0.0f, 1.0f);
            ImGui::SetNextWindowPos(ImVec2(x, y + (1 - in) * Dp(12)), ImGuiCond_Always, ImVec2(1, 1));
            ImGui::SetNextWindowBgAlpha(1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(in, out));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Dp(14, 12));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Dp(kRadiusCard));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Dp(8, 8));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ToVec4(col::bg2));
            char name[32];
            std::snprintf(name, sizeof(name), "##toast%d", t.id);
            // Not NoNav: the toast buttons are reachable with Ctrl+Tab / Tab (P11 11.6); NoFocusOnAppearing keeps a
            // new toast from taking the keyboard away.
            ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImU32 accent = t.kind == ToastKind::Success ? col::success : t.kind == ToastKind::Warning ? col::warn
                                 : t.kind == ToastKind::Error ? col::danger : col::accent;
            const char* icon = t.kind == ToastKind::Success ? ICON_CHECK : t.kind == ToastKind::Warning ? ICON_WARNING
                               : t.kind == ToastKind::Error ? ICON_ERROR : ICON_INFO;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            Icon(dl, ImVec2(p.x + Dp(8), p.y + Dp(9)), accent, icon);
            ImGui::SetCursorScreenPos(ImVec2(p.x + Dp(24), p.y));
            ImGui::PushTextWrapPos(p.x + Dp(340));
            ImGui::TextUnformatted(t.text.c_str());
            ImGui::PopTextWrapPos();
            if (!t.bundle.empty()) {
                ImGui::SetCursorScreenPos(ImVec2(p.x + Dp(24), ImGui::GetCursorScreenPos().y + Dp(2)));
                if (Button(str::kToastOpenInEditor, ButtonKind::Primary)) {
                    if (hooks.open_editor) hooks.open_editor(t.bundle);
                    remove_id = t.id;
                }
                ImGui::SameLine();
                if (Button(str::kToastShowInFolder)) fs::ShowInExplorer(t.bundle);
                ImGui::SameLine();
                if (Button(str::kToastDelete, ButtonKind::Danger)) {
                    const std::string path = t.bundle;
                    std::weak_ptr<bool> weak = alive;
                    Impl* self = this;
                    Async::Run([path, weak, self]() {
                        const bool ok = fs::DeleteTree(path);
                        Async::PostToUi([weak, self, ok]() {
                            if (!weak.expired() && ok) self->PushToast(str::kToastDeleted, ToastKind::Info, {}, 4);
                        });
                    });
                    remove_id = t.id;
                }
            }
            if (ImGui::IsWindowHovered()) t.shown_at = std::min(t.shown_at + ImGui::GetIO().DeltaTime, now);   // hover pauses
            const float hgt = ImGui::GetWindowHeight();
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(4);
            y -= hgt + Dp(8);
        }
        if (remove_id)
            toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [remove_id](const ToastItem& t) { return t.id == remove_id; }),
                         toasts.end());
    }

    // --- GPU: camera preview -------------------------------------------------------------------------------------
    void RenderGpu(ID3D11DeviceContext* ctx) {
        if (!preview || !preview->running.load()) return;
        const uint64_t serial = preview->src.PreviewSerial();
        if (serial == preview_serial && preview_has_frame) return;
        ID3D11ShaderResourceView *y = nullptr, *uv = nullptr;
        SizeI size{};
        if (!preview->src.LatestPreviewPlanes(&y, &uv, &size)) return;
        if (!preview_tex) {
            if (FAILED(blit.Init(*dev))) return;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 320;
            td.Height = 180;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev->Device()->CreateTexture2D(&td, nullptr, &preview_tex))) return;
            dev->Device()->CreateRenderTargetView(preview_tex.Get(), nullptr, &preview_rtv);
            dev->Device()->CreateShaderResourceView(preview_tex.Get(), nullptr, &preview_srv);
        }
        D3D11_VIEWPORT viewport{0, 0, 320, 180, 0, 1};
        // Mirror nothing, letterbox nothing: the preview is 16:9 like the default camera mode.
        if (SUCCEEDED(blit.Draw(ctx, y, uv, size, preview_rtv.Get(), viewport, true))) {
            preview_serial = serial;
            preview_has_frame = true;
        }
    }

    int RedrawIntervalMs() const {
        if (NowS() < anim_until || size_anim_t0 >= 0) return 16;
        if (settings_win && settings_win->IsOpen() && settings_win->WantsRedraw()) return 16;
        if (state != State::Idle || preparing) return 33;   // timer, pulse, meters
        if (monitor_audio || preview) return 33;            // live meter / camera preview visible
        if (!toasts.empty()) return 250;                    // expiry
        return 0;
    }
};

// ======================================================================================================================

RecorderBar::RecorderBar() : m(std::make_unique<Impl>()) {}
RecorderBar::~RecorderBar() { Shutdown(); }

bool RecorderBar::Init(platform::D3DDevice& dev, platform::Window& window, const Settings& settings,
                       std::string settings_path, const Options& options, Hooks hooks) {
    Impl& s = *m;
    s.dev = &dev;
    s.win = &window;
    s.settings = settings;
    s.settings_path = std::move(settings_path);
    s.opt = options;
    s.hooks = std::move(hooks);
    s.visible = !options.start_hidden;

    HWND hwnd = window.Hwnd();
    window.SetDarkTitleBar(true);
    window.SetRoundedCorners(true);
    const COLORREF border = RGB(0x2C, 0x32, 0x3E);   // DWMWA_BORDER_COLOR = border token
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    if (options.exclude_from_capture) window.SetExcludeFromCapture(true);
    s.InitAnchor();
    s.ApplyWindowRect(kIdleW, kIdleH);

    HICON app_icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
    if (!app_icon) app_icon = LoadIconW(nullptr, IDI_APPLICATION);
    s.icon_normal = app_icon;
    s.icon_recording = platform::TrayIcon::CreateRecordingIcon(app_icon);
    if (!s.tray.Add(hwnd, s.icon_normal, W(str::kTrayTip).c_str(), kTrayMsg)) CT_LOG_WARN("tray icon could not be added");
    s.RegisterHotkeys();
    s.DiscoverAsync();

    s.settings_win = std::make_unique<SettingsWindow>(s.settings_path);
    s.settings_win->SetFillViewport(true);
    s.settings_win->SetGpuName(dev.AdapterName());
    Impl* self = &s;
    s.settings_win->on_changed = [self](const Settings& changed) {
        const Settings::Hotkeys keep = self->settings.hotkeys;   // hotkeys come through reregister_hotkeys
        self->settings = changed;
        self->settings.hotkeys = keep;
        self->Animate();
    };
    s.settings_win->suspend_hotkeys = [self]() { self->hotkeys.UnregisterAll(); };
    s.settings_win->reregister_hotkeys = [self](const Settings::Hotkeys& h, std::string* error) {
        self->settings.hotkeys = h;
        return self->RegisterHotkeysChecked(error);
    };
    s.settings_win->request_redraw = [hwnd]() { PostMessageW(hwnd, WM_NULL, 0, 0); };
    return true;
}

void RecorderBar::Shutdown() {
    Impl& s = *m;
    if (!s.win) return;
    *s.alive = false;
    s.alive = std::make_shared<bool>(false);
    if (s.countdown.IsRunning()) s.countdown.Cancel();
    s.region_overlay.Close();
    s.frame.Hide();
    s.session.Cancel();
    s.session.WaitIdle(30'000);
    s.StopMonitor();
    if (s.preview) {
        s.preview->src.Stop();
        s.preview.reset();
    }
    s.settings_win.reset();   // flushes a pending save
    s.hotkeys.UnregisterAll();
    s.tray.Remove();
    if (s.icon_recording) DestroyIcon(s.icon_recording);
    s.icon_recording = nullptr;
    s.preview_srv.Reset();
    s.preview_rtv.Reset();
    s.preview_tex.Reset();
    s.blit.Shutdown();
    s.win = nullptr;
}

void RecorderBar::Frame() { m->Frame(); }
void RecorderBar::AfterFirstFrame() { m->DiscoverCapsAsync(); }
void RecorderBar::RenderGpu(ID3D11DeviceContext* ctx) { m->RenderGpu(ctx); }
int RecorderBar::RedrawIntervalMs() const { return m->RedrawIntervalMs(); }

LRESULT RecorderBar::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, bool& handled) {
    Impl& s = *m;
    if (!s.win) return 0;
    if (msg == platform::TrayIcon::TaskbarCreatedMessage()) {
        s.tray.Readd();
        return 0;
    }
    switch (msg) {
    case WM_NCHITTEST: {
        POINT p{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ScreenToClient(hwnd, &p);
        if (PtInRect(&s.grip_px, p)) {
            handled = true;
            return HTCAPTION;
        }
        break;
    }
    case WM_NCLBUTTONDBLCLK:
        handled = true;   // no maximize on the grip
        return 0;
    case WM_MOVING: {
        RECT* rc = reinterpret_cast<RECT*>(lparam);
        s.Snap(*rc);
        handled = true;
        return TRUE;
    }
    case WM_EXITSIZEMOVE:
        s.RememberPosition();
        break;
    case WM_HOTKEY:
        handled = true;
        switch (static_cast<int>(wparam)) {
        case kHkToggle: s.Toggle(Clock::QpcNow()); break;
        case kHkPause: s.session.TogglePause(); break;
        case kHkMarker: AddMarker(); break;
        case kHkCancel: CancelRecording(); break;
        case kHkEsc: if (s.countdown.IsRunning()) s.countdown.Cancel(); break;
        default: break;
        }
        s.Animate();
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && s.countdown.IsRunning()) {
            s.countdown.Cancel();
            handled = true;
            return 0;
        }
        break;
    case WM_TIMER:
        if (wparam == kArmTimer) {
            s.ArmIfIdle();
            handled = true;
            return 0;
        }
        break;
    case WM_DISPLAYCHANGE:
        // Monitors added, removed or re-arranged: the display list and an armed setup are out of date.
        if (s.session.IsArmed() && s.state == State::Idle) s.session.Disarm();
        s.DiscoverAsync();
        s.ScheduleArm();
        break;
    case WM_POWERBROADCAST:
        // RecordingSession pauses on suspend itself (RegisterSuspendResumeNotification, also headless); toggling
        // here as well would resume it again.
        break;
    case WM_CLOSE:
        s.ShowBar(false);   // close hides to tray; Quit is in the tray menu
        handled = true;
        return 0;
    case WM_COPYDATA:
        s.ShowBar(true);
        break;
    default:
        if (msg == kTrayMsg) {
            const platform::TrayIcon::Event e = platform::TrayIcon::Decode(wparam, lparam);
            if (e.what == WM_LBUTTONUP || e.what == NIN_SELECT || e.what == NIN_KEYSELECT) {
                s.ShowBar(!s.visible);
                if (s.visible && e.what == NIN_KEYSELECT) s.FocusForKeyboard();
            }
            else if (e.what == WM_CONTEXTMENU || e.what == WM_RBUTTONUP) s.TrayMenu(e.anchor);
            handled = true;
            return 0;
        }
        break;
    }
    return 0;
}

void RecorderBar::ApplySettings(const Settings& settings) {
    m->settings = settings;
    m->RegisterHotkeys();
    m->Animate();
}

const Settings& RecorderBar::CurrentSettings() const { return m->settings; }
void RecorderBar::OpenPopup(Popup p) { m->open_request = p; m->Animate(); }
void RecorderBar::ToggleRecording(int64_t trigger_qpc) { m->Toggle(trigger_qpc ? trigger_qpc : Clock::QpcNow()); }
void RecorderBar::TogglePause() { m->session.TogglePause(); }
void RecorderBar::AddMarker() { m->MarkerNow(); }
void RecorderBar::CancelRecording() {
    if (m->countdown.IsRunning()) m->countdown.Cancel();
    else if (m->session.IsActive()) m->session.Cancel();
}
void RecorderBar::ShowSavedToast(const std::string& bundle_path) { m->ShowSavedToastImpl(bundle_path); }
bool RecorderBar::IsRecording() const { return m->state != State::Idle || m->preparing; }
bool RecorderBar::IsArmed() const { return m->session.IsArmed() && m->session.IsPrepared(); }
bool RecorderBar::IsIdle() const { return m->state == State::Idle && !m->preparing && !m->session.IsActive(); }
double RecorderBar::LastStartLatencyMs() const {
    const recorder::State st = m->session.GetState();
    return st == State::Recording || st == State::Paused ? m->session.LastStartLatencyMs() : -1.0;
}
int64_t RecorderBar::LastTriggerQpc() const { return m->trigger_qpc; }
void RecorderBar::ShowBar(bool show) { m->ShowBar(show); }
void RecorderBar::PushToast(const std::string& text, float seconds) { m->PushToast(text, ToastKind::Info, {}, seconds); }
void RecorderBar::OpenSettings() { m->OpenSettings(); }
bool RecorderBar::ErrorVisible() const { return m->error_banner.Visible(); }
std::string RecorderBar::ErrorText() const { return m->error_banner.Message(); }
int RecorderBar::ErrorAction(int index) const {
    // The banner keeps labels only; the ids come from the view the bar showed, mirrored here.
    return m->error_banner.ActionIdAt(index);
}
void RecorderBar::RunErrorAction(int action) { m->RunErrorAction(action); }
void RecorderBar::ClearError() { m->error_banner.Clear(); }
int RecorderBar::LastFailureCause() const { return m->last_failure_cause; }
void RecorderBar::SetFaults(const recorder::SessionFaults& faults) { m->opt.faults = faults; }
void RecorderBar::ChooseOutputFolder(const std::string& folder) { m->SetOutputFolder(folder); }
std::string RecorderBar::LastBundle() const { return m->last_bundle; }
std::string RecorderBar::NavItemForTest() const { return m->nav_label; }
void RecorderBar::ReleaseDeviceObjects() {
    Impl& s = *m;
    if (s.preview) {
        s.preview->src.Stop();
        s.preview.reset();
    }
    s.preview_srv.Reset();
    s.preview_rtv.Reset();
    s.preview_tex.Reset();
    s.preview_has_frame = false;
    s.blit.Shutdown();
}

} // namespace ct::ui
