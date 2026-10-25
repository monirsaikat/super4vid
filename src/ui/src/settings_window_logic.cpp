// Settings window logic without drawing (P11 11.6): debounced saver thread, hotkey chord capture and conflict
// checks, diagnostics text, and the IO pool enumeration of devices / encoders / presets.
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/settings_window.h>

#include <ct/audio/devices.h>
#include <ct/capture/webcam_source.h>
#include <ct/core/async.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/export/preset.h>
#include <ct/media/encoder_caps.h>
#include <ct/platform/hotkeys.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>
#include <ct/ui/error_ux.h>
#include <ct/ui/first_run.h>
#include <ct/ui/level_monitor.h>
#include <ct/ui/strings.h>
#include <ct/version.h>

#include <algorithm>

namespace ct::ui {

namespace {
int64_t NowMs() { return static_cast<int64_t>(GetTickCount64()); }
}

// ---- SettingsSaver ------------------------------------------------------------------------------------------------

SettingsSaver::SettingsSaver(std::string path, int64_t delay_ms) : m_path(std::move(path)), m_debounce(delay_ms) {
    Thread::Options o;
    o.name = "ct-settings-save";
    o.com_mta = false;
    m_thread.Start(o, [this] { Loop(); });
}

SettingsSaver::~SettingsSaver() {
    {
        ExclusiveGuard g(m_lock);
        m_quit = true;
    }
    m_wake.Set();
    m_thread.Join();
    Flush();
}

void SettingsSaver::Schedule(const Settings& s) {
    {
        ExclusiveGuard g(m_lock);
        m_pending = s;
        m_debounce.Touch(NowMs());
    }
    m_wake.Set();
}

bool SettingsSaver::Pending() const {
    ExclusiveGuard g(m_lock);
    return m_debounce.Pending();
}

std::string SettingsSaver::LastError() const {
    ExclusiveGuard g(m_lock);
    return m_last_error;
}

void SettingsSaver::Flush() {
    ExclusiveGuard g(m_lock);
    if (m_debounce.Pending()) SaveLocked();
}

// Runs with m_lock held, so a Flush and the thread never write the file (and its .tmp) at the same time. The write
// is a few KB; Schedule() on the UI thread can wait for it only when it lands exactly during a save.
bool SettingsSaver::SaveLocked() {
    m_debounce.Clear();
    if (m_path.empty()) return false;
    Err err;
    if (!m_pending.Save(m_path, &err)) {
        m_last_error = err.message;
        CT_LOG_WARN("settings save failed: %s", err.message.c_str());
        return false;
    }
    m_last_error.clear();
    m_saves.fetch_add(1);
    return true;
}

void SettingsSaver::Loop() {
    for (;;) {
        int64_t wait = -1;
        {
            ExclusiveGuard g(m_lock);
            if (m_quit) return;
            const int64_t now = NowMs();
            if (m_debounce.Due(now)) {
                SaveLocked();
                continue;
            }
            wait = m_debounce.RemainingMs(now);
        }
        m_wake.Wait(wait < 0 ? INFINITE : static_cast<uint32_t>(wait));
    }
}

// ---- Hotkeys --------------------------------------------------------------------------------------------------------

const char* HotkeyActionLabel(HotkeyAction a) {
    switch (a) {
    case HotkeyAction::Toggle: return str::kSetHkToggle;
    case HotkeyAction::Pause: return str::kSetHkPause;
    case HotkeyAction::Marker: return str::kSetHkMarker;
    case HotkeyAction::Cancel: return str::kSetHkCancel;
    }
    return "";
}

Hotkey& HotkeyFor(Settings::Hotkeys& h, HotkeyAction a) {
    switch (a) {
    case HotkeyAction::Pause: return h.pause;
    case HotkeyAction::Marker: return h.marker;
    case HotkeyAction::Cancel: return h.cancel;
    default: return h.toggle;
    }
}

Hotkey HotkeyFor(const Settings::Hotkeys& h, HotkeyAction a) { return HotkeyFor(const_cast<Settings::Hotkeys&>(h), a); }

unsigned int VkFromImGuiKey(ImGuiKey k) {
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return 'A' + static_cast<unsigned>(k - ImGuiKey_A);
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return '0' + static_cast<unsigned>(k - ImGuiKey_0);
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) return VK_F1 + static_cast<unsigned>(k - ImGuiKey_F1);
    if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return VK_NUMPAD0 + static_cast<unsigned>(k - ImGuiKey_Keypad0);
    switch (k) {
    case ImGuiKey_Space: return VK_SPACE;
    case ImGuiKey_Enter: case ImGuiKey_KeypadEnter: return VK_RETURN;
    case ImGuiKey_Tab: return VK_TAB;
    case ImGuiKey_Escape: return VK_ESCAPE;
    case ImGuiKey_Backspace: return VK_BACK;
    case ImGuiKey_Delete: return VK_DELETE;
    case ImGuiKey_Insert: return VK_INSERT;
    case ImGuiKey_Home: return VK_HOME;
    case ImGuiKey_End: return VK_END;
    case ImGuiKey_PageUp: return VK_PRIOR;
    case ImGuiKey_PageDown: return VK_NEXT;
    case ImGuiKey_LeftArrow: return VK_LEFT;
    case ImGuiKey_RightArrow: return VK_RIGHT;
    case ImGuiKey_UpArrow: return VK_UP;
    case ImGuiKey_DownArrow: return VK_DOWN;
    case ImGuiKey_PrintScreen: return VK_SNAPSHOT;
    case ImGuiKey_Pause: return VK_PAUSE;
    default: return 0;
    }
}

ChordResult CaptureChord(unsigned int vk, unsigned int mods, Hotkey* out) {
    if (vk == 0) return ChordResult::Pending;
    mods &= MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    if (mods == 0 && vk == VK_ESCAPE) return ChordResult::Cancelled;
    if (mods == 0 && (vk == VK_BACK || vk == VK_DELETE)) return ChordResult::Cleared;
    const bool standalone = (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SNAPSHOT || vk == VK_INSERT;
    // Shift alone is not enough for typing keys: Shift+A is just "A" to every other app.
    if (!standalone && (mods & (MOD_ALT | MOD_CONTROL | MOD_WIN)) == 0) return ChordResult::NeedsModifier;
    if (out) *out = Hotkey{mods, vk};
    return ChordResult::Captured;
}

std::string HotkeyLabel(Hotkey hk) { return hk.IsSet() ? HotkeyToString(hk) : std::string(str::kSetHkNone); }

bool CheckHotkey(const Settings::Hotkeys& current, HotkeyAction action, Hotkey candidate, const HotkeyProbe& probe,
                 std::string* error) {
    if (!candidate.IsSet()) return true;
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        const auto other = static_cast<HotkeyAction>(i);
        if (other == action) continue;
        if (HotkeyFor(current, other) == candidate) {
            if (error)
                *error = FillPlaceholder(FillPlaceholder(str::kSetHkDuplicate, HotkeyLabel(candidate)),
                                         HotkeyActionLabel(other));
            return false;
        }
    }
    if (HotkeyFor(current, action) == candidate) return true;
    if (probe && !probe(candidate, error)) return false;
    return true;
}

bool SystemHotkeyProbe(Hotkey candidate, std::string* error) {
    constexpr int kProbeId = 0xBF00;   // RegisterHotKey ids for applications: 0x0000-0xBFFF
    platform::HotkeyManager probe;
    if (!probe.Register(nullptr, kProbeId, candidate.modifiers, candidate.vk, error)) return false;
    probe.Unregister(nullptr, kProbeId);
    return true;
}

// ---- Diagnostics ------------------------------------------------------------------------------------------------------

namespace {
std::string OsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return "Windows";
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (!fn || fn(&v) != 0) return "Windows";
    return Format("Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}
} // namespace

std::string BuildDiagnostics(const std::string& gpu_name, const std::string& encoder_report) {
    std::string d;
    AppendFormat(d, "super4vid %s (%s, built %s)\n", CT_VERSION_STRING, CT_GIT_HASH, CT_BUILD_DATE);
    AppendFormat(d, "OS: %s\n", OsVersion().c_str());
    AppendFormat(d, "GPU: %s\n", gpu_name.empty() ? "unknown" : gpu_name.c_str());
    AppendFormat(d, "Portable: %s\n", platform::Paths::Portable() ? "yes" : "no");
    AppendFormat(d, "Settings: %s\n", platform::Paths::SettingsDir().c_str());
    AppendFormat(d, "Logs: %s\n", platform::Paths::LogsDir().c_str());
    AppendFormat(d, "Recordings: %s\n", platform::Paths::DefaultRecordingsDir().c_str());
    d += "Encoders:\n";
    d += encoder_report.empty() ? std::string("  (not checked yet)\n") : encoder_report;
    if (!d.empty() && d.back() != '\n') d += '\n';
    return d;
}

// ---- Async enumeration --------------------------------------------------------------------------------------------------

struct SettingsWindow::AsyncState {
    SrwLock lock;
    std::atomic<uint32_t> generation{0};
    std::atomic<int> running{0};
    std::vector<Mic> mics;
    std::vector<Out> outs;
    std::vector<Cam> cams;
    std::vector<PresetItem> user_presets;
    std::string encoder_report;
    bool hevc = true;
    bool mics_ready = false, cams_ready = false, encoders_ready = false, presets_ready = false;
    std::function<void()> redraw;

    // The redraw callback is called and cleared under the lock, so a job finishing after the window is gone
    // never calls into its owner.
    void Finish() {
        generation.fetch_add(1);
        running.fetch_sub(1);
        ExclusiveGuard g(lock);
        if (redraw) redraw();
    }
};

void SettingsWindow::StartEnumeration(bool cameras_with_modes) {
    auto st = m_async;
    {
        ExclusiveGuard g(st->lock);
        st->redraw = request_redraw;
    }
    // Microphones and playback endpoints: fast (tens of ms), one enumerator for both.
    st->running.fetch_add(1);
    Async::Run([st] {
        std::vector<Mic> mics;
        std::vector<Out> outs;
        audio::AudioDevices devices;
        const HRESULT hr = devices.Init();
        if (SUCCEEDED(hr)) {
            for (const audio::DeviceInfo& d : devices.List(true)) mics.push_back({WideToUtf8(d.id), d.name, d.is_default});
            for (const audio::DeviceInfo& d : devices.List(false)) outs.push_back({WideToUtf8(d.id), d.name, d.is_default});
            devices.Shutdown();
        } else {
            CT_LOG_WARN("settings: audio device enumeration failed: %s", HrToString(hr).c_str());
        }
        {
            ExclusiveGuard g(st->lock);
            st->mics = std::move(mics);
            st->outs = std::move(outs);
            st->mics_ready = true;
        }
        st->Finish();
    });
    // Cameras: opening each device to list its modes takes 300-800 ms.
    st->running.fetch_add(1);
    Async::Run([st, cameras_with_modes] {
        std::vector<capture::WebcamInfo> infos;
        const HRESULT hr = capture::WebcamSource::Enumerate(infos, cameras_with_modes);
        if (FAILED(hr)) CT_LOG_WARN("settings: camera enumeration failed: %s", HrToString(hr).c_str());
        std::vector<Cam> cams;
        for (const capture::WebcamInfo& w : infos) {
            Cam c;
            c.link = WideToUtf8(w.symbolic_link);
            c.name = w.name;
            for (const SizeI& s : w.sizes) c.sizes.push_back(Format("%dx%d", s.w, s.h));
            cams.push_back(std::move(c));
        }
        {
            ExclusiveGuard g(st->lock);
            st->cams = std::move(cams);
            st->cams_ready = true;
        }
        st->Finish();
    });
    // Encoders (cached per process after the first call) and user presets.
    st->running.fetch_add(1);
    Async::Run([st] {
        std::string report = media::EncoderCaps::Report();
        const bool hevc = !media::EncoderCaps::Enumerate(media::VideoCodec::Hevc).empty();
        std::vector<PresetItem> user;
        for (const exportng::Preset& p : exportng::Preset::LoadUser()) user.push_back({p.id, p.name, true});
        {
            ExclusiveGuard g(st->lock);
            st->encoder_report = std::move(report);
            st->hevc = hevc;
            st->encoders_ready = true;
            st->user_presets = std::move(user);
            st->presets_ready = true;
        }
        st->Finish();
    });
}

std::unique_ptr<SettingsWindow> SettingsWindow::CreateDemo(std::string_view section, std::string gpu_name,
                                                           std::function<void()> redraw) {
    static const char* const kNames[kSettingsSectionCount] = {"recording", "audio",  "camera",  "hotkeys",
                                                              "editor",    "export", "privacy", "about"};
    const std::string path = SettingsFilePath();
    Settings settings;
    Err err;
    if (!settings.Load(path, &err)) CT_LOG_WARN("--settings-demo: %s", err.message.c_str());
    auto w = std::make_unique<SettingsWindow>(path);
    w->SetFillViewport(true);
    w->SetGpuName(std::move(gpu_name));
    w->request_redraw = std::move(redraw);
    int index = 0;
    for (int i = 0; i < kSettingsSectionCount; ++i)
        if (IEquals(section, kNames[i])) index = i;
    w->Open(settings, static_cast<SettingsSection>(index));
    return w;
}

bool SettingsWindow::AsyncBusy() const { return m_async->running.load() > 0; }

void SettingsWindow::PollAsync() {
    const uint32_t gen = m_async->generation.load();
    if (gen == m_seen_generation) return;
    m_seen_generation = gen;
    ExclusiveGuard g(m_async->lock);
    if (m_async->mics_ready) {
        m_mics = m_async->mics;
        m_outs = m_async->outs;
        m_devices_ready = true;
    }
    if (m_async->cams_ready) m_cams = m_async->cams;
    m_cams_ready = m_async->cams_ready;
    if (m_async->encoders_ready) {
        m_encoder_report = m_async->encoder_report;
        m_hevc_available = m_async->hevc;
        m_encoders_ready = true;
    }
    if (m_async->presets_ready) {
        m_presets.clear();
        for (const exportng::Preset& p : exportng::Preset::Builtin()) m_presets.push_back({p.id, p.name, false});
        for (const PresetItem& p : m_async->user_presets) m_presets.push_back(p);
        m_presets_ready = true;
    }
}

// ---- SettingsWindow (non-drawing parts) ------------------------------------------------------------------------------

SettingsWindow::SettingsWindow(std::string settings_path)
    : m_path(std::move(settings_path)), m_saver(std::make_unique<SettingsSaver>(m_path)),
      m_async(std::make_shared<AsyncState>()) {
    for (const exportng::Preset& p : exportng::Preset::Builtin()) m_presets.push_back({p.id, p.name, false});
}

SettingsWindow::~SettingsWindow() {
    if (m_capturing >= 0) EndCapture();
    ExclusiveGuard g(m_async->lock);
    m_async->redraw = nullptr;
}

void SettingsWindow::Open(const Settings& current, SettingsSection section) {
    m_settings = current;
    m_section = section;
    m_focus_next = true;
    for (std::string& e : m_hotkey_error) e.clear();
    if (!m_open && !AsyncBusy()) StartEnumeration(true);
    m_open = true;
}

void SettingsWindow::Close() {
    if (m_capturing >= 0) EndCapture();
    StopMeterTest();
    m_open = false;
}

void SettingsWindow::StartMeterTest(bool system_audio) {
    if (!m_meter) m_meter = std::make_unique<LevelMonitor>();
    m_meter_device = system_audio ? std::string() : m_settings.recording.mic_device;
    m_meter->Start(system_audio ? LevelMonitor::Source::SystemAudio : LevelMonitor::Source::Microphone,
                   Utf8ToWide(m_meter_device));
}

void SettingsWindow::StopMeterTest() {
    if (m_meter) m_meter->Stop();
}

bool SettingsWindow::MeterTestActive() const { return m_meter && m_meter->Active(); }

float SettingsWindow::MeterTestDb() const { return m_meter ? m_meter->PeakDb() : -120.0f; }

bool SettingsWindow::WantsRedraw() const {
    if (!m_open) return false;
    if (m_capturing >= 0 || AsyncBusy() || MeterTestActive()) return true;
    return ImGui::GetCurrentContext() && ImGui::GetTime() - m_copied_at < 2.5;
}

void SettingsWindow::Apply(const std::function<void(Settings&)>& edit) {
    edit(m_settings);
    if (on_changed) on_changed(m_settings);
    m_saver->Schedule(m_settings);
}

bool SettingsWindow::AssignHotkey(HotkeyAction a, Hotkey hk, std::string* error) {
    const HotkeyProbe& p = probe ? probe : HotkeyProbe(&SystemHotkeyProbe);
    std::string err;
    if (!CheckHotkey(m_settings.hotkeys, a, hk, p, &err)) {
        if (error) *error = err;
        return false;
    }
    if (HotkeyFor(m_settings.hotkeys, a) == hk) return true;
    Apply([a, hk](Settings& s) { HotkeyFor(s.hotkeys, a) = hk; });
    return true;
}

void SettingsWindow::BeginCapture(HotkeyAction a) {
    if (m_capturing < 0 && suspend_hotkeys) suspend_hotkeys();
    m_capturing = static_cast<int>(a);
    m_hotkey_error[m_capturing].clear();
    m_capture_hint = str::kSetHkCaptureHint;
}

void SettingsWindow::EndCapture() {
    const int was = m_capturing;
    m_capturing = -1;
    m_capture_hint.clear();
    if (was < 0 || !reregister_hotkeys) return;
    std::string err;
    if (!reregister_hotkeys(m_settings.hotkeys, &err)) m_hotkey_error[was] = err;
}

} // namespace ct::ui
