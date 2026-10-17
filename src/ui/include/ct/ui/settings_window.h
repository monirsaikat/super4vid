#pragma once
// ct/ui/settings_window.h
// The settings window (P11 11.6): a modal-style window with a section list on the left (Recording, Audio, Camera,
// Hotkeys, Editor, Export, Privacy, About). Every change applies immediately: the window edits its own copy of
// Settings, calls on_changed(settings) and schedules a debounced save (500 ms of quiet, docs/03 section 11) that runs
// Settings::Save on a background thread. Hotkeys are captured with a key recorder, checked for duplicates and for
// conflicts with other apps by a test registration (HotkeyManager), then re-registered through a callback.
// Device lists (microphones, cameras), the encoder report and user presets are enumerated on the IO pool, never on
// the UI thread.
//
// Use (recorder bar gear button, editor Preferences menu):
//     ui::SettingsWindow settings_window(ui::SettingsFilePath());
//     settings_window.on_changed = [&](const Settings& s) { app_settings = s; ... };
//     settings_window.Open(app_settings);                 // on the gear click
//     settings_window.Draw();                             // every frame, inside NewFrame/Render
//     if (settings_window.WantsRedraw()) keep rendering;  // spinner / async results / pending save
//
// Thread model: everything except the request_redraw callback is UI thread. request_redraw may be called from an IO
// pool thread (enumeration finished) and must be thread-safe (set a flag + SetEvent). SettingsSaver and the pure
// helpers below are thread-safe as documented.
// Lifetime: create after ImGui::CreateContext; destroying the window flushes a pending save synchronously.

#include <ct/core/config.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>

#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ct::ui {

// ---- Debounce (pure; unit tested) ---------------------------------------------------------------------------------
// Each Touch re-arms the deadline to now + delay; Due once the deadline passed without another Touch.
class Debouncer {
public:
    explicit Debouncer(int64_t delay_ms = 500) : m_delay_ms(delay_ms) {}
    void Touch(int64_t now_ms) { m_deadline_ms = now_ms + m_delay_ms; m_pending = true; }
    bool Pending() const { return m_pending; }
    bool Due(int64_t now_ms) const { return m_pending && now_ms >= m_deadline_ms; }
    // Milliseconds until Due (0 when due), -1 when nothing is pending.
    int64_t RemainingMs(int64_t now_ms) const {
        if (!m_pending) return -1;
        return now_ms >= m_deadline_ms ? 0 : m_deadline_ms - now_ms;
    }
    void Clear() { m_pending = false; }
    int64_t DelayMs() const { return m_delay_ms; }

private:
    int64_t m_delay_ms;
    int64_t m_deadline_ms = 0;
    bool m_pending = false;
};

// ---- Debounced background save ----------------------------------------------------------------------------------
// Schedule() stores a copy and re-arms the debounce; the "ct-settings-save" thread writes the latest copy with
// Settings::Save once 500 ms passed without another Schedule. Flush() saves a pending copy now (blocking).
// Thread model: all members thread-safe. The destructor flushes and joins the thread.
class SettingsSaver {
public:
    explicit SettingsSaver(std::string path, int64_t delay_ms = 500);
    ~SettingsSaver();
    SettingsSaver(const SettingsSaver&) = delete;
    SettingsSaver& operator=(const SettingsSaver&) = delete;

    void Schedule(const Settings& s);
    void Flush();
    bool Pending() const;
    uint32_t SaveCount() const { return m_saves.load(); }   // successful writes (tests)
    std::string LastError() const;                          // last Save failure, empty after a success
    const std::string& Path() const { return m_path; }

private:
    void Loop();
    bool SaveLocked();   // m_lock held; writes m_pending and clears it

    std::string m_path;
    mutable SrwLock m_lock;
    Debouncer m_debounce;
    Settings m_pending;
    std::string m_last_error;
    bool m_quit = false;
    Event m_wake;
    Thread m_thread;
    std::atomic<uint32_t> m_saves{0};
};

// ---- Hotkeys (pure parts unit tested) ---------------------------------------------------------------------------
enum class HotkeyAction { Toggle, Pause, Marker, Cancel };
constexpr int kHotkeyActionCount = 4;
const char* HotkeyActionLabel(HotkeyAction a);   // str::kSetHk*
Hotkey& HotkeyFor(Settings::Hotkeys& h, HotkeyAction a);
Hotkey HotkeyFor(const Settings::Hotkeys& h, HotkeyAction a);

// ImGui key -> Win32 VK for the keys ParseHotkey knows (A-Z, 0-9, F1-F24, Num0-9, Space, Enter, Tab, Esc,
// Backspace, Delete, Insert, Home, End, PageUp/Down, arrows, PrintScreen, Pause). 0 for modifiers and other keys.
unsigned int VkFromImGuiKey(ImGuiKey key);

// One step of the key recorder: a key went down with these modifiers held.
enum class ChordResult {
    Pending,          // only modifiers so far (or an unsupported key): keep listening
    Captured,         // *out is the new chord
    Cleared,          // Backspace/Delete without modifiers: the hotkey becomes "None"
    Cancelled,        // Esc without modifiers: keep the old value
    NeedsModifier,    // a letter/digit/navigation key without Ctrl/Alt/Shift/Win (would steal typing system-wide)
};
// mods: MOD_ALT 0x1 | MOD_CONTROL 0x2 | MOD_SHIFT 0x4 | MOD_WIN 0x8. F1-F24, Pause, PrintScreen and Insert may be
// used without a modifier.
ChordResult CaptureChord(unsigned int vk, unsigned int mods, Hotkey* out);
// Text shown on the recorder: "Ctrl+Shift+F9"; str::kSetHkNone for {0,0}.
std::string HotkeyLabel(Hotkey hk);

// Conflict check for assigning `candidate` to `action`. Order: unset (always ok), duplicate of another of our four
// actions (message str::kSetHkDuplicate), unchanged (ok, no probe), then `probe` (a test registration; it returns
// false with a user-presentable message when another app owns the chord). Returns true when the chord may be used.
using HotkeyProbe = std::function<bool(Hotkey candidate, std::string* error)>;
bool CheckHotkey(const Settings::Hotkeys& current, HotkeyAction action, Hotkey candidate, const HotkeyProbe& probe,
                 std::string* error);
// The default probe: platform::HotkeyManager::Register on a thread-bound (null HWND) id, then Unregister. Call on the
// UI thread while the app's own hotkeys are suspended (see SettingsWindow::suspend_hotkeys).
bool SystemHotkeyProbe(Hotkey candidate, std::string* error);

// ---- Diagnostics (About) ------------------------------------------------------------------------------------------
// Version, commit, build date, OS build, GPU, portable flag, folders, encoder report: the "Copy diagnostics" text.
std::string BuildDiagnostics(const std::string& gpu_name, const std::string& encoder_report);

// Icon glyphs the window uses beyond icons.h (Keyboard E765, Refresh E72C); checked against both icon fonts by tests.
constexpr int kSettingsExtraIconCount = 2;
extern const char* const kSettingsExtraIcons[kSettingsExtraIconCount];

// ---- The window -----------------------------------------------------------------------------------------------------
enum class SettingsSection { Recording, Audio, Camera, Hotkeys, Editor, Export, Privacy, About };
constexpr int kSettingsSectionCount = 8;

class SettingsWindow {
public:
    explicit SettingsWindow(std::string settings_path);
    ~SettingsWindow();
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Shows the window with a copy of `current` and starts device/encoder enumeration on the IO pool.
    void Open(const Settings& current, SettingsSection section = SettingsSection::Recording);
    void Close();          // Done / Esc / the close button; flushing is left to the debounce
    bool IsOpen() const { return m_open; }
    // Builds the window for this frame (no-op while closed). Centered in the main viewport, 760x540 DIP, dims the
    // rest of the viewport (modal style, but non-blocking for the caller's other windows' rendering).
    void Draw();
    // True while something animates or waits: enumeration running, a hotkey being recorded, a save pending.
    bool WantsRedraw() const;

    const Settings& Current() const { return m_settings; }
    void SetSection(SettingsSection s) { m_section = s; }
    SettingsSection Section() const { return m_section; }
    void SetGpuName(std::string name) { m_gpu_name = std::move(name); }   // D3DDevice::AdapterName()
    // Embedded mode for --settings-demo: the window fills the whole viewport and has no backdrop.
    void SetFillViewport(bool fill) { m_fill_viewport = fill; }
    SettingsSaver& Saver() { return *m_saver; }

    // Called on the UI thread after every applied change (the recorder bar / editor update their copy).
    std::function<void(const Settings&)> on_changed;
    // Called when a hotkey recorder starts listening: the owner unregisters its global hotkeys so the keys reach
    // the window (RegisterHotKey would swallow F9) and so the conflict probe does not hit our own registration.
    std::function<void()> suspend_hotkeys;
    // Called when the recorder stops (with or without a change): register `hotkeys` again. Return false with a
    // message when one fails; the window shows it under that row. Null: nothing to re-register (demo).
    std::function<bool(const Settings::Hotkeys& hotkeys, std::string* error)> reregister_hotkeys;
    // Conflict probe; defaults to SystemHotkeyProbe.
    HotkeyProbe probe;
    // Thread-safe; called from an IO pool thread when async results arrive (wake the UI loop).
    std::function<void()> request_redraw;

    // `Camtesia.exe --settings-demo [--section recording|audio|camera|hotkeys|editor|export|privacy|about]`: loads
    // SettingsFilePath() (small file, at startup), fills the viewport, opens at `section`.
    static std::unique_ptr<SettingsWindow> CreateDemo(std::string_view section, std::string gpu_name,
                                                      std::function<void()> request_redraw);

    // Test hooks (no ImGui needed): apply a change as the widgets do, feed a recorded chord.
    void ApplyForTest(const std::function<void(Settings&)>& edit) { Apply(edit); }
    bool AssignHotkeyForTest(HotkeyAction a, Hotkey hk, std::string* error) { return AssignHotkey(a, hk, error); }

private:
    struct AsyncState;   // shared with IO pool jobs (outlives the window if a job is still running)

    void Apply(const std::function<void(Settings&)>& edit);
    bool AssignHotkey(HotkeyAction a, Hotkey hk, std::string* error);
    void StartEnumeration(bool cameras_with_modes);
    void PollAsync();
    bool AsyncBusy() const;
    void BeginCapture(HotkeyAction a);
    void EndCapture();
    void PollCapture();

    void DrawSidebar(float width);
    void DrawRecording();
    void DrawAudio();
    void DrawCamera();
    void DrawHotkeys();
    void DrawEditor();
    void DrawExport();
    void DrawPrivacy();
    void DrawAbout();
    void SectionTitle(const char* title, const char* desc);
    bool FolderRow(const char* label, std::string& value, const char* placeholder, const char* id);

    std::string m_path;
    Settings m_settings;
    std::unique_ptr<SettingsSaver> m_saver;
    std::shared_ptr<AsyncState> m_async;
    SettingsSection m_section = SettingsSection::Recording;
    bool m_open = false;
    bool m_fill_viewport = false;
    bool m_focus_next = false;
    std::string m_gpu_name;

    // Snapshot of the async results (copied on the UI thread).
    struct Mic { std::string id, name; bool is_default = false; };
    struct Cam { std::string link, name; std::vector<std::string> sizes; };   // sizes "WxH", largest first
    struct PresetItem { std::string id, name; bool user = false; };
    std::vector<Mic> m_mics;
    std::vector<Cam> m_cams;
    std::vector<PresetItem> m_presets;
    std::string m_encoder_report;
    bool m_hevc_available = true;
    bool m_devices_ready = false, m_cams_ready = false, m_encoders_ready = false, m_presets_ready = false;
    uint32_t m_seen_generation = 0;

    // Hotkey recorder.
    int m_capturing = -1;                       // HotkeyAction index, -1 = idle
    std::string m_hotkey_error[kHotkeyActionCount];
    std::string m_capture_hint;
    double m_copied_at = -100.0;                // "Diagnostics copied" feedback
};

} // namespace ct::ui
