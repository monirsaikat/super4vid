# P05 · Recorder app (session, events, hotkeys, overlays, bar, tray, webcam, bundle)

**Goal:** the complete recording product: start from the bar or a hotkey, count down, record screen + mic +
system + webcam + cursor/key events into a `.ctrec` bundle, pause/resume, stop, with the bar and overlays hidden
from the capture, and survive crashes with minimal loss.

**Read first:** `docs/03-architecture.md` sections 2, 3, 4, 10, 11; `docs/04` sections 1, 2; `docs/05` sections 1-3; `docs/07` (Win32 rows).
**Depends on:** P04. **Size:** L (about 5,000 lines).

## Deliverables

```
src/recorder/include/ct/recorder/{session_clock.h,recording_session.h,bundle_writer.h,event_recorder.h,keys.h}
src/events/include/ct/events/{ctev.h,event_log.h}  src/events/src/{ctev.cpp,event_log.cpp}   (module ct_events: pure, depends on ct_core only; shared with P07/P08)
src/capture/include/ct/capture/webcam_source.h   src/capture/src/webcam_source.cpp
src/platform/include/ct/platform/{hotkeys.h,hooks_thread.h,tray_icon.h,layered_window.h,nv12_blit.h}
src/ui/include/ct/ui/{theme.h,strings.h,widgets.h,recorder_bar.h,countdown_overlay.h,region_overlay.h,recording_frame.h,toast.h}
src/app/main.cpp (modes: default bar, --record headless, --selftest)
tests/recorder/*.cpp
```

## Interfaces

```cpp
namespace ct::recorder {

// Shared by capture, audio and events so every stream agrees on media time (pause-adjusted).
struct SessionClock {
    int64_t epoch_qpc = 0;  std::atomic<int64_t> pause_offset_us{0};  std::atomic<bool> paused{false};  int64_t pause_started_qpc = 0;
    TimeUs NowMediaUs() const;                       // QpcToUs(now, epoch) - pause_offset
    TimeUs HnsToMediaUs(uint64_t qpc_hns) const;     // for WASAPI positions
    void   Pause();  void Resume();                  // updates pause_offset_us atomically
};

enum class KeystrokeMode { Off, ShortcutsOnly, All };
enum class State { Idle, Countdown, Starting, Recording, Paused, Finalizing, Error };

struct SessionConfig {
    capture::DisplayInfo display;  RectI region;  int fps = 60;  media::VideoCodec codec = media::VideoCodec::H264;  int quality = 70;  bool hdr_tonemap = true;
    bool mic = true;  std::wstring mic_device;  bool system_audio = true;
    bool webcam = false;  std::wstring webcam_device;  SizeI webcam_size{1280, 720};
    std::string output_dir;  int countdown_s = 3;  KeystrokeMode keystrokes = KeystrokeMode::ShortcutsOnly;  int cursor_hz = 120;
};
struct LiveStats { TimeUs elapsed_us; uint64_t file_bytes; uint64_t dropped_frames; float mic_peak_db, sys_peak_db; uint64_t free_disk_bytes; bool hardware_encoder; std::string encoder_name; };
struct SessionListener {   // all callbacks on the UI thread (posted)
    virtual void OnStateChanged(State) = 0;
    virtual void OnCountdown(int seconds_left) = 0;
    virtual void OnTick(const LiveStats&) = 0;                        // 4 Hz
    virtual void OnFinished(const std::string& bundle_path) = 0;
    virtual void OnError(const std::string& message, HRESULT hr) = 0;
    virtual ~SessionListener() = default;
};

class RecordingSession {
public:
    HRESULT Prepare(platform::D3DDevice&, const SessionConfig&, SessionListener*);   // builds every component; no capture yet. < 400 ms, runs on the IO pool
    void StartCountdown();  void StartNow();          // StartNow: epoch = QpcNow(); audio.Start; writer.Start; capturer.Start; webcam.Start; events.Start -> Recording
    void TogglePause();  void AddMarker(std::string label);
    void Stop();                                      // -> Finalizing (worker thread) -> Idle + OnFinished
    void Cancel();                                    // Stop then delete the bundle
    void EmergencyFinalize();                         // crash filter: writer.Abort(), events.Flush(), meta with "incomplete": true
    State GetState() const;  const LiveStats& Stats() const;  const std::string& BundlePath() const;  const SessionClock& Clock() const;
};

class BundleWriter {
public:
    bool Create(std::string_view output_dir, std::string& bundle_path_out);   // "<dir>\<LocalFileStamp>.ctrec\" (suffix " (2)" on collision)
    std::string ScreenPath() const;  std::string CamPath() const;  std::string EventsPath() const;  std::string MetaPath() const;
    bool WriteMeta(const Json&);  bool Delete();
};

// .ctev writer/reader (docs/04 section 2) live in namespace ct::events (module ct_events, header ct/events/ctev.h); shown here for reference.
class EventWriter { public: bool Open(std::string_view path, const EventHeader&); void Write(const EventRecord&); void Flush(); void Close(); };   // 64 KB buffer; IO thread flushes every 1 s
class EventReader { public: bool Load(std::string_view path, EventLogData& out, std::string* error); };   // typed arrays sorted by t_us; skips unknown types

class EventRecorder {   // hooks thread + cursor tracker + focus hook + IO thread
public:
    struct Config { RectI region; KeystrokeMode keystrokes; int cursor_hz; };
    bool Start(const Config&, std::string_view events_path, const SessionClock&);
    void SetPaused(bool);  void Marker(std::string_view label);  void Flush();  void Stop();
    struct Live { std::atomic<int32_t> cursor_x{0}, cursor_y{0}; char last_key_label[32]; };
    const Live& LiveInfo() const;
};
namespace keys {
    bool IsShortcut(uint16_t vk, uint8_t modifiers);                  // Ctrl/Alt/Win held, or F1..F24, or Esc
    void LabelFor(uint16_t vk, uint16_t scancode, bool extended, char out[32]);   // GetKeyNameTextW -> UTF-8, "Ctrl", "Shift", "A", "Enter", "Space"
    uint32_t CharFor(uint16_t vk, uint16_t scancode, const BYTE keystate[256]);   // ToUnicodeEx with the "do not change state" flag (1 << 2); 0 if none
}
} // namespace ct::recorder

namespace ct::capture {
struct WebcamInfo { std::wstring symbolic_link; std::string name; std::vector<SizeI> sizes; };
class WebcamSource {
public:
    static HRESULT Enumerate(std::vector<WebcamInfo>&);             // MFEnumDeviceSources VIDCAP + native types per device
    struct Config { std::wstring symbolic_link; SizeI preferred_size; int preferred_fps = 30; };
    HRESULT Init(platform::D3DDevice&, const Config&);              // source reader with D3D manager, advanced video processing, output NV12
    platform::TexturePool& Nv12Pool();                               // for the cam Mp4Writer
    bool Start(const recorder::SessionClock&, FrameSink&);  void Stop();   // "ct-webcam" thread, blocking ReadSample loop
    SizeI ActualSize() const;  int ActualFps() const;
    ID3D11ShaderResourceView* LatestPreviewSrv();                    // RGBA preview texture updated per frame via platform::Nv12Blit (UI thread reads under the device lock)
};
}

namespace ct::platform {
class HotkeyManager { public: bool Register(HWND, int id, UINT modifiers, UINT vk, std::string* conflict_error); void UnregisterAll(); };   // MOD_NOREPEAT; WM_HOTKEY handled by the window
class TrayIcon { public: bool Add(HWND, HICON, const wchar_t* tip, UINT callback_msg); void SetIcon(HICON); void SetTip(const wchar_t*); void Notify(const wchar_t* title, const wchar_t* text); void Remove(); };   // NOTIFYICON_VERSION_4; menu via TrackPopupMenuEx after SetForegroundWindow
class LayeredWindow {   // per-pixel alpha top-level window drawn with Direct2D (ID2D1DCRenderTarget over a 32-bit DIB) and pushed with UpdateLayeredWindow
public:
    bool Create(const RectI& virtual_px, bool click_through, bool topmost, bool exclude_from_capture);
    ID2D1RenderTarget* BeginDraw();  void EndDrawAndPresent(BYTE alpha = 255);   // clears to transparent, caller draws, presents dirty rect
    HWND Hwnd() const;  void Move(const RectI&);  void Show(bool);  void Destroy();
};
class Nv12Blit { public: HRESULT Init(D3DDevice&); HRESULT ToRgba(ID3D11DeviceContext*, ID3D11Texture2D* nv12, ID3D11Texture2D* rgba_rt); };   // Y/UV SRVs (R8 / R8G8) + BT.709 limited->RGB pixel shader; reused by P06/P08
}
```

## 5.1 Session state machine
```
Idle --Prepare()--> Idle(prepared) --StartCountdown()--> Countdown --(timer 1 s steps)--> Starting --StartNow()--> Recording
Recording --TogglePause()--> Paused --TogglePause()--> Recording
Recording|Paused --Stop()--> Finalizing --(worker done)--> Idle + OnFinished
any --Cancel()--> Finalizing --> Idle (bundle deleted)
any --component error--> Error -> Finalizing(best effort) -> Idle + OnError
```
`StartNow` order matters: start audio producers first (they take 10-30 ms to deliver), then `Mp4Writer::Start`, then `ScreenCapturer::Start` with the epoch, webcam, events. The first captured frame is logged with its latency from the hotkey (`Stats::T("session.hotkey_to_frame")`).

Pause: `clock.Pause()`, `capturer.Pause()`, `events.SetPaused(true)` (writes PAUSE record), audio `SetPaused(true)` (keeps devices running so resume is instant). Resume reverses. Media files have no gaps; `meta.json.pauses` records them.

Stop: on a worker (`Async::Run`): `capturer.Stop()` -> `webcam.Stop()` -> audio `Stop()` -> `writer.Finalize()` and `cam_writer.Finalize()` -> `events.Stop()` -> `WriteMeta` -> post `OnFinished`. The UI shows "Finalizing..." with a spinner until then. `CrashHandler::RegisterEmergency(EmergencyFinalize)` is set in `Prepare` and cleared in `Stop`.

Disk watch: every tick check `fs::FreeSpaceBytes(output_dir)`: < 1 GB warn once (toast), < 200 MB auto-stop with error "Disk almost full".

## 5.2 Bundle and meta
`BundleWriter::Create` makes the folder; `WriteMeta` builds the `docs/04` section 1 JSON from the components (encoder name from `Mp4Writer::EncoderName`, device names, display info, duration from the capturer's last tick, pauses from the clock log). `--record` headless mode and `--selftest` use the same session class with a `SessionListener` that prints to the console.

## 5.3 Event recorder
`HooksThread` (`platform::hooks_thread.h`, "ct-hooks", `THREAD_PRIORITY_HIGHEST`, STA not required, MTA fine):
```
mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
kbd_hook   = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(nullptr), 0);
focus_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
timer (HighResTimer periodic at 1000/cursor_hz ms)
loop: MsgWaitForMultipleObjects({stop, timer}, QS_ALLINPUT) -> pump PeekMessage (mandatory for LL hooks and WinEvents) -> on timer: tracker.Poll(clock.NowMediaUs(), region, samples_q, shapes_q); heartbeat: PostThreadMessage(self, WM_NULL) every 250 ms; the IO thread re-installs hooks if no heartbeat for 1 s (Windows silently removes slow hooks).
Unhook everything on exit.
```
Hook procs (static, `g_recorder` pointer): `if (nCode == HC_ACTION && g_recorder->Active())` build a fixed-size `EventRecord` and `Push` to an SPSC ring (`SpscQueue<EventRecord, 65536>`), then `return CallNextHookEx(nullptr, nCode, wParam, lParam)`. **Nothing else** happens in the hook (no allocation, no logging, no GDI).
- Mouse: `WM_{L,R,M,X}BUTTON{DOWN,UP}` -> `MOUSE_BUTTON` (X button from `HIWORD(mouseData)`); `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` -> `MOUSE_WHEEL` (delta = `(short)HIWORD(mouseData)`); `WM_MOUSEMOVE` ignored (the tracker polls). Position `pt - region.origin`.
- Keyboard: `KBDLLHOOKSTRUCT`; down = `WM_KEYDOWN|WM_SYSKEYDOWN`; modifiers from `GetAsyncKeyState(VK_CONTROL|VK_SHIFT|VK_MENU|VK_LWIN|VK_RWIN) & 0x8000`; `extended = flags & LLKHF_EXTENDED`. Mode `ShortcutsOnly`: push only when `keys::IsShortcut`. Mode `All`: push everything; `utf32_char` computed on the IO thread (never inside the hook) via `keys::CharFor` only for printable keys without Ctrl/Alt. Mode `Off`: no keyboard hook installed at all.
- Focus: `FocusProc(hwnd)` -> `WINDOW_FOCUS` with `GetWindowTextW`, `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`, exe via `GetWindowThreadProcessId` + `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW`.
IO thread ("ct-events-io"): drains the ring and the tracker queues every 100 ms into `EventWriter` (shapes first, dedupe by id), derives `TYPING` records (key downs closer than 1 s form a burst), flushes to disk every 1 s and on `Flush()`. The `.ctev` header `flags` reflects the keystroke mode.

## 5.4 Hotkeys, tray, notifications
Hotkeys registered on the bar's hidden message window (so they work while the bar is hidden): toggle / pause / marker / cancel from `Settings`. On `ERROR_HOTKEY_ALREADY_REGISTERED` show a toast with the key name and keep running. Tray icon: normal and recording (red dot drawn over the app icon at runtime with GDI into a 32-bit DIB -> `CreateIconIndirect`), tooltip shows elapsed time while recording, left click shows/hides the bar, right click menu: Record / Pause / Stop / Open editor / Settings / Quit. `TrayIcon::Notify` for "Saved" when the bar is hidden.

## 5.5 Region overlay (`ui::RegionOverlay` on `platform::LayeredWindow`)
Behaviour per `docs/05` section 3. Draw with D2D: dim fill, cleared selection, 1 px white + 1 px black outline, 8 handles, size label pill. Window snapping: `EnumWindows` top-down (z-order), skip invisible, cloaked (`DWMWA_CLOAKED`), `WS_EX_TOOLWINDOW`, zero-size, our own process; rect from `DWMWA_EXTENDED_FRAME_BOUNDS` intersected with the display; take the first window containing the cursor. Output: `RectI region` (virtual px, even, >= 64x64) and the `DisplayInfo` whose rect contains the region centre; regions spanning two monitors are clamped to the monitor with the larger overlap (v1 limitation, documented in the UI).

## 5.6 Recorder bar, countdown, recording frame
- `ui::RecorderBar`: an ImGui window filling a frameless topmost `platform::Window` (560x64 dip idle, 260x44 recording, animated 160 ms), `SetExcludeFromCapture(true)`, draggable by the grip, snaps to screen edges within 16 px, remembers position. Content per `docs/05` section 2 using `ui::widgets` (segmented control, icon button, meter bar, popover). Meter values from `WasapiCapture::Levels()` at 30 Hz (the bar requests continuous redraw only while recording or a meter is visible and audio is live; otherwise 0 redraws).
- `ui::CountdownOverlay`: `LayeredWindow` centred in the region, 160 px circle `bg2` at 85 % alpha, number in 72 px semibold, 1 s steps, Esc cancels (the bar's window keeps focus for keys; the overlay is click-through).
- `ui::RecordingFrame`: click-through `LayeredWindow` drawing a 2 px `record` colored frame outside the region (excluded from capture).
- `ui::Toast`: bottom-right stack per `docs/05`, used for "Saved" with actions (Open in editor / Show in folder / Delete).

## 5.7 Webcam
`WebcamSource::Init`: `MFCreateAttributes` + `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE = VIDCAP_GUID` + `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK` -> `MFCreateDeviceSource`; reader attributes: `MF_SOURCE_READER_D3D_MANAGER`, `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = TRUE`, `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = TRUE`, `MF_SOURCE_READER_DISABLE_DXVA = FALSE`. Enumerate `GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i)`, score by |area - preferred| then fps then subtype preference NV12 > YUY2 > MJPG; `SetCurrentMediaType(native)` then `SetCurrentMediaType(NV12 partial type with the same frame size)` so the reader converts. Thread loop: `ReadSample(FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &sample)`; timestamp = `MFSampleExtension_DeviceTimestamp` (QPC in 100 ns) if present else `clock.NowMediaUs()`; buffer -> `IMFDXGIBuffer` -> texture + subresource -> `CopySubresourceRegion` into a pool slot (own NV12 pool, 6 textures) -> `sink.OnFrame` (second `Mp4Writer` with `fps = ActualFps`, `bitrate` from the table for the cam size) and `Nv12Blit::ToRgba` into the preview texture. Camera frames are variable rate; the writer accepts their real timestamps (cam.mp4 is VFR, aligned by media time).

## Tests
- Unit: `SessionClock` pause math; `EventWriter` -> `EventReader` round trip for every record type incl. padding and RLE shapes; unknown record type skipped; `keys::IsShortcut` table; `keys::LabelFor` for VK_RETURN/VK_SPACE/'A' on the US layout; TYPING burst derivation; bundle name collision suffix.
- Integration (device): `super4vid.exe --record --monitor 0 --fps 60 --seconds 3 --out <tmp>` -> bundle exists with `meta.json` (valid, duration 3.0 +- 0.1 s), `screen.mp4` probes to 60 fps and ~180 frames, `events.ctev` parses, `CURSOR_SHAPE` count >= 1; `--selftest` returns 0.
- Manual (write results in `PROGRESS.md`): bar hidden from the recording; countdown hidden; hotkey start from another app; pause 10 s then resume -> media has no gap and `pauses` lists it; unplug mic mid-recording -> continues with silence then new device; lock/unlock -> continues; kill the process with Task Manager during recording -> `events.ctev` readable up to the last second, `meta.json` absent (expected), UI next start offers nothing (v1 limitation).

## Acceptance (5.8)
All recording budgets of `MASTER_PLAN.md` section 8 measured with this build (cold start, idle redraws, hotkey latency, 1080p60 and 4K60 CPU/GPU/drops/RSS, 60 min A/V drift). Pause/resume gapless. Event loss on kill <= 2 s.

## Pitfalls
- LL hooks require a message loop on the installing thread and are removed by Windows if the callback exceeds the `LowLevelHooksTimeout` (default 300 ms); keep the callback to a queue push.
- `GetAsyncKeyState` in the hook is fine; `GetKeyboardState` is not (it is thread-local input state).
- `ToUnicodeEx` without the "do not change keyboard state" flag (`1 << 2`) breaks dead keys for the user; only use it with the flag, and never inside the hook.
- `UpdateLayeredWindow` requires `WS_EX_LAYERED` set at creation and a premultiplied 32-bit DIB; D2D's `D2D1_ALPHA_MODE_PREMULTIPLIED` matches.
- `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` fails on Windows 10 before 2004 and for layered windows on some builds; log and continue (the bar is then visible in recordings on those systems).
- `RegisterHotKey` is per-thread/window; register on the UI thread and handle `WM_HOTKEY` there.
- Webcam `ReadSample` blocks; stop by setting a flag and calling `IMFSourceReader::Flush` from another thread, then join.
- `MFEnumDeviceSources` activates nothing; `MFCreateDeviceSource` can take 300-800 ms: do it in `Prepare` on the IO pool, never on the UI thread.
