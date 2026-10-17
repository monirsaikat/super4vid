#pragma once
// ct/recorder/recording_session.h
// One recording from start to finished bundle (P05 task 5.1): builds the screen capturer, the screen.mp4 writer
// (fps clamped to what the encoder accepts), mic/system audio, the webcam + cam.mp4 writer and the event recorder;
// starts them in order on a shared SessionClock, pauses/resumes them together, stops and finalizes on a worker,
// writes meta.json (docs/04 section 1), watches free disk space and reports LiveStats at 4 Hz.
//   Idle --Prepare--> Idle(prepared) --StartCountdown--> Countdown --StartNow--> Starting --> Recording
//   Recording <--TogglePause--> Paused;  Recording|Paused --Stop--> Finalizing --> Idle + OnFinished
//   any --Cancel--> Finalizing --> Idle (bundle deleted);  component error --> Error --> Finalizing --> Idle + OnError
// Thread model: every component is created, started, paused and stopped on the session's own "ct-session" thread
// (COM MTA, as WASAPI and Media Foundation want), fed by a command queue: StartNow/TogglePause/AddMarker/Stop/
// Cancel/PrepareAsync return immediately from any thread (the UI). Prepare() posts and waits (call it from the IO
// pool or a console thread, never from the UI thread). SessionListener callbacks go through ct::Async::PostToUi,
// i.e. they run on the UI thread in the app, and synchronously on ct-session when no UI poster is installed
// (headless mode). GetState/Stats/BundlePath/Clock from any thread.
// Lifetime: the D3DDevice and the listener must outlive the session. The destructor cancels a running recording.
// EmergencyFinalize is registered once with platform::CrashHandler and is active between StartNow and Stop.

#include <ct/capture/display.h>
#include <ct/core/types.h>
#include <ct/media/encode_config.h>
#include <ct/platform/d3d_device.h>
#include <ct/recorder/event_recorder.h>
#include <ct/recorder/session_clock.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ct::recorder {

enum class State { Idle, Countdown, Starting, Recording, Paused, Finalizing, Error };
const char* StateName(State s);

struct SessionConfig {
    capture::DisplayInfo display;
    RectI region;                       // virtual-screen px; empty = the whole display
    int fps = 60;                       // clamped down (120/60/30) when the encoder refuses it
    media::VideoCodec codec = media::VideoCodec::H264;   // HEVC falls back to H.264 when it cannot be created
    int quality = 70;
    bool hdr_tonemap = true;
    bool mic = true;
    std::wstring mic_device;            // empty = default capture endpoint
    bool system_audio = true;
    std::wstring system_device;         // empty = default render endpoint
    bool webcam = false;
    std::wstring webcam_device;         // symbolic link; empty = the first camera
    SizeI webcam_size{1280, 720};
    bool webcam_synthetic = false;      // tests / --selftest: generated colour bars instead of a camera
    std::string output_dir;             // empty = platform::Paths::DefaultRecordingsDir()
    int countdown_s = 3;                // informational: the UI runs the countdown overlay
    KeystrokeMode keystrokes = KeystrokeMode::ShortcutsOnly;
    int cursor_hz = 120;
    bool events = true;                 // false: no event recorder (tests that must not install hooks)
};

struct LiveStats {
    State state = State::Idle;
    TimeUs elapsed_us = 0;              // media time (pause-adjusted)
    uint64_t file_bytes = 0;            // screen.mp4 (+ cam.mp4) on disk
    uint64_t frames = 0;                // video frames written so far
    uint64_t dropped_frames = 0;        // capture pool/sink drops + skipped late ticks + writer rejections
    float mic_peak_db = -120.0f, sys_peak_db = -120.0f;
    uint64_t free_disk_bytes = 0;
    bool hardware_encoder = false;
    std::string encoder_name;
    int fps = 0;                        // the rate actually recorded (after clamping)
    bool has_mic = false, has_system = false, has_camera = false;
};

struct SessionListener {   // all callbacks posted to the UI thread (see the thread model above)
    virtual void OnStateChanged(State) = 0;
    virtual void OnCountdown(int seconds_left) = 0;
    virtual void OnTick(const LiveStats&) = 0;                 // 4 Hz while recording/paused
    virtual void OnFinished(const std::string& bundle_path) = 0;
    virtual void OnError(const std::string& message, HRESULT hr) = 0;
    // Additions: non-fatal problems (low disk, camera lost, audio device error) and the result of PrepareAsync.
    virtual void OnWarning(const std::string& message) { (void)message; }
    virtual void OnPrepared(HRESULT hr) { (void)hr; }
    virtual ~SessionListener() = default;
};

class RecordingSession {
public:
    static constexpr uint64_t kWarnFreeBytes = 1ull << 30;          // < 1 GB: warn once
    static constexpr uint64_t kStopFreeBytes = 200ull << 20;        // < 200 MB: stop with "Disk almost full"

    RecordingSession();
    ~RecordingSession();
    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    // Builds every component (no capture yet) and creates the bundle folder. Blocks until done (typically < 400 ms
    // plus the first encoder load). Only from Idle and not prepared.
    HRESULT Prepare(platform::D3DDevice& dev, const SessionConfig& cfg, SessionListener* listener);
    void PrepareAsync(platform::D3DDevice& dev, const SessionConfig& cfg, SessionListener* listener);
    bool IsPrepared() const;

    void StartCountdown();                    // Idle(prepared or preparing) -> Countdown; OnCountdown(countdown_s)
    // Starts recording (after Prepare finishes, if it is still running). trigger_qpc: when the user asked (hotkey or
    // click); the latency to the first captured frame is logged and kept in Stats::T("session.hotkey_to_frame").
    void StartNow(int64_t trigger_qpc = 0);
    void TogglePause();
    void AddMarker(std::string label);
    void Stop();                              // -> Finalizing (on ct-session) -> Idle + OnFinished
    void Cancel();                            // Stop and delete the bundle (also releases a prepared session)
    // Crash filter: writer Abort (the writer is never destroyed afterwards), events Flush, meta.json with
    // "incomplete": true. Allocation-free apart from what Abort/Flush do internally.
    void EmergencyFinalize();

    State GetState() const;
    bool IsActive() const;                    // Countdown .. Finalizing
    LiveStats Stats() const;                  // snapshot (copy)
    std::string BundlePath() const;
    const SessionClock& Clock() const { return m_clock; }
    double LastStartLatencyMs() const;        // trigger -> first frame of the last recording, -1 if unknown
    // Peak dBFS of the mic / system streams, refreshed at 30 Hz while recording (floor otherwise). Any thread.
    void LiveLevels(float* mic_db, float* sys_db) const;
    // Blocks until the session is Idle with nothing queued (headless mode, tests). false on timeout.
    bool WaitIdle(uint32_t timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    SessionClock m_clock;
};

} // namespace ct::recorder
