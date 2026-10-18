#pragma once
// ct/ui/recorder_bar.h
// The recorder bar (docs/05 section 2, P05 task 5.6): the content and logic of the frameless, topmost, capture-excluded
// pill window. Idle (560x64 dip): grip, source segmented control (Monitor N / Region / Window), fps/quality/codec
// popover, mic and system toggles with device lists and live meters, camera toggle with device list and a hover
// preview, REC, settings, close-to-tray. Recording (260x44 dip, animated 160 ms around the same bottom-centre anchor):
// pulsing dot + timer, pause, stop, marker, mic meter, grip. Drives recorder::RecordingSession (REC -> countdown overlay
// -> StartNow), the RegionOverlay, CountdownOverlay and RecordingFrame, the hotkeys (registered on the bar's window, so
// they work while it is hidden), the tray icon and its menu, and the "Saved" toast (Open in editor / Show in folder /
// Delete). Popovers and toasts are ImGui windows in their own platform windows (multi-viewport), also excluded from
// capture.
// Thread model: UI thread (STA). Slow work (device lists, encoder caps, camera preview start) runs on ct::Async and
// comes back through Async::PostToUi. Lifetime: Init after the ImGui context and the window exist; Shutdown before
// they are destroyed.

#include <ct/core/config.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/win.h>
#include <ct/platform/window.h>
#include <ct/recorder/recording_session.h>

#include <functional>
#include <memory>
#include <string>

struct ID3D11DeviceContext;

namespace ct::ui {

class RecorderBar {
public:
    struct Hooks {
        std::function<void(const std::string& bundle_path)> open_editor;   // empty path = the editor without a file
        std::function<void()> open_settings;                              // empty: the built-in ui::SettingsWindow
        std::function<void()> quit;                                       // leave the message loop
        // P11 11.5: the recording failed because the graphics device was lost (TDR). The session is idle; the app
        // recreates its D3D device (and swapchain, ImGui renderer) before the next frame, after ReleaseDeviceObjects.
        std::function<void()> recreate_device;
    };
    struct Options {
        bool start_hidden = false;      // --tray
        bool exclude_from_capture = true;   // false for screenshots of the bar
        bool synthetic_camera = false;  // lists a generated "Synthetic camera" (tests without hardware)
        int countdown_override = -1;    // >= 0 replaces settings.recording.countdown_s
        recorder::SessionFaults faults; // P11 11.5 fault injection for every recording (--fault, tests)
    };

    RecorderBar();
    ~RecorderBar();
    RecorderBar(const RecorderBar&) = delete;
    RecorderBar& operator=(const RecorderBar&) = delete;

    bool Init(platform::D3DDevice& dev, platform::Window& window, const Settings& settings, std::string settings_path,
              const Options& options, Hooks hooks);
    void Shutdown();

    // Builds the ImGui frame (bar window, popovers, toasts). Between NewFrame and Render.
    void Frame();
    // Call once after the first present: starts the slow discovery (encoder caps, camera modes) on the IO pool.
    void AfterFirstFrame();
    // GPU work before ImGui renders (camera preview NV12 -> RGBA), with D3DDevice::Lock held by the caller.
    void RenderGpu(ID3D11DeviceContext* ctx);
    // 0 = event driven (no redraw until input); otherwise the bar wants a frame every `interval_ms` (recording timer,
    // live meters, animations).
    int RedrawIntervalMs() const;
    // Window messages not consumed by ImGui (WM_HOTKEY, tray callback, WM_NCHITTEST for the grip, WM_MOVING snap,
    // WM_EXITSIZEMOVE, WM_POWERBROADCAST, WM_CLOSE, WM_COPYDATA, Esc during the countdown).
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, bool& handled);

    // The settings window changed the settings: re-register hotkeys, apply recording defaults.
    void ApplySettings(const Settings& settings);
    const Settings& CurrentSettings() const;

    // Programmatic control (tray, hotkeys, tests and benchmarks).
    enum class Popup { None, Monitor, Fps, Mic, System, Camera };
    void OpenPopup(Popup p);
    void ToggleRecording(int64_t trigger_qpc = 0);   // REC / F9: countdown + start, or stop
    void TogglePause();
    void AddMarker();
    void CancelRecording();
    void ShowSavedToast(const std::string& bundle_path);   // also what OnFinished calls
    bool IsRecording() const;                              // countdown .. finalizing
    bool IsIdle() const;
    double LastStartLatencyMs() const;
    int64_t LastTriggerQpc() const;                        // QPC of the last REC / hotkey trigger
    void ShowBar(bool show);
    void PushToast(const std::string& text, float seconds = 6);   // info toast (first run)
    void OpenSettings();   // hooks.open_settings, else the built-in SettingsWindow

    // P11 11.5 error UX. The red error panel (bottom-right, above the toasts) shows a recording failure or a
    // microphone problem with its recovery actions (ui::ErrorView).
    bool ErrorVisible() const;
    std::string ErrorText() const;
    int ErrorAction(int index) const;          // the kAction* of the n-th button, 0 past the end
    void RunErrorAction(int action);           // as if the button were clicked
    void ClearError();
    int LastFailureCause() const;              // recorder::FailureCause of the last failure, -1 = none
    // "Choose another folder..." after the folder picker returned `folder`: saves it as the output folder and
    // clears the panel.
    void ChooseOutputFolder(const std::string& folder);
    std::string LastBundle() const;            // the last kept recording ("" = none yet)
    // Device loss: release the bar's own GPU objects (camera preview) before the app recreates the device.
    void ReleaseDeviceObjects();
    // Diagnostics: change the faults of the next recordings.
    void SetFaults(const recorder::SessionFaults& faults);
    // Keyboard navigation diagnostics: the label of the item ImGui's nav cursor is on ("" when none).
    std::string NavItemForTest() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ct::ui
