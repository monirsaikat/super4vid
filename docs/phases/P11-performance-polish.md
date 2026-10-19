# P11 · Performance and polish

**Goal:** make every number in `MASTER_PLAN.md` section 8 true and recorded, remove idle power draw, harden the
app against real-world Windows situations, finish error UX, settings, first-run, and keyboard navigation.

**Read first:** `docs/06-testing-and-performance.md` sections 5-7; `docs/03` section 10; `docs/05` sections 2, 4, 7.
**Depends on:** P05, P09, P10. **Size:** M (mostly measurement and fixes; about 2,000 lines of new code: settings window, error UX, first run).

## 11.1 Idle power
Procedure: start the app (bar only), wait 10 s, `wpr -start GeneralProfile -start Power`, 60 s idle, `wpr -stop idle.etl`. In WPA: CPU Usage (Precise) context switches for `super4vid.exe` must be < 20 per second; no periodic timers above 1 Hz except the autosave tick; GPU idle.
Fixes to apply if violated: ImGui frame only on input (`frames_requested` logic from P00); meters polled only while visible and audio running; `Log` flush thread waits on an event with a 500 ms timeout only while there is pending data; `Stats` dump only with `--stats`; `Autosave::Tick` at 1 Hz via `SetTimer` on the UI thread only when a project is dirty; no `Sleep` loops anywhere (grep for `Sleep(`); `HighResTimer` cancelled when capture stops. Same check with the editor open and paused.

## 11.2 Startup
`super4vid.exe --time-to-window` after a reboot (cold) and again (warm), 3 runs each, median in `PROGRESS.md`. Budget 500 / 300 ms. Required design: `wWinMain` creates the window and presents the first frame before anything heavy; `MfRuntime::Ensure`, `EncoderCaps::Enumerate`, `AudioDevices::Init`, `WebcamSource::Enumerate`, D2D/DWrite creation all happen on the IO pool after the first frame and fill the UI when done (placeholders until then); fonts load from the system (fast) and the atlas builds once; no synchronous file enumeration of recordings at start. "Prewarm" on hovering REC: `MfRuntime::Ensure` + `D3DDevice::InitMfManager` + `EncoderCaps` (not the audio devices, to keep the microphone LED off until recording).

## 11.3 Memory
`--stats` reports pools/caches; `CacheBudget` default 256 MB (setting), thumbnails <= 2,000 per media and <= 64 MB total, `TextRenderer::Trim(64 MB)`, undo <= 200 / 64 MB, log ring 1 MB. Leak checks: Debug exit prints nothing from CRT and `ReportLiveDeviceObjects` (allow the device and swapchain only). Long-run test: 30 min 1080p recording -> RSS flat after the first minute (`--stats` RSS column). Editor with a 30 min recording open and 10 min of scrubbing -> RSS < 600 MB.

## 11.4 Environment matrix (manual, each row ticked in `PROGRESS.md`)
| Scenario | Expected |
|---|---|
| Two monitors, 100 % + 150 % DPI, record the 150 % one | region in physical pixels, crisp output, bar scales when moved between monitors |
| Monitor unplugged while recording that monitor | `ACCESS_LOST` -> repeats, then error toast after 5 s without recovery; file finalized and playable |
| Resolution change mid-recording | letterboxed frames at the original size, no restart |
| Fullscreen exclusive game | capture continues or `ACCESS_LOST` retries; the hotkey still works (LL hooks are global) |
| UAC prompt during recording | frames repeat during the secure desktop, resume afterwards |
| Sleep / resume | session paused on `PBT_APMSUSPEND`, resume leaves it paused, toast explains |
| Default audio device change mid-recording | continuous audio, device name updated in meta |
| Disk fills up | warning at 1 GB, auto-stop at 200 MB, file playable |
| Portable mode (`portable.txt`) | all data under `<exe>\data\` |
| WARP (VM, no GPU) | software encoder at 720p30, editor usable, export completes |
| HDR monitor | tone-mapped SDR recording with correct brightness of SDR content |

## 11.5 Error UX
Every `OnError` path shows a banner/toast with: what happened (plain words), what to do, **Details** (HRESULT string, log path, "Open log folder"). Messages live in `strings.h`:
`kErrDiskFull` "Not enough disk space on <drive>. Recording stopped and saved." · `kErrNoEncoder` "No video encoder is available. Install the latest graphics driver." · `kErrMicBusy` "The microphone is in use by another app. Recording continues without it." · `kErrDeviceLost` "The graphics device was reset. The recording was saved up to this point." · `kErrAccessLost` "Windows blocked screen capture (secure desktop or display change)." · `kErrMediaMissing` "Media file not found: <path>. Relink it from the Media panel." Each message is tested by forcing the condition where feasible (invalid output dir, mic held by another process via `audio_bench`, device-removed via `D3D11_CREATE_DEVICE_DEBUG` + `dxcap -forcetdr` where available).

## 11.6 Settings window, first run, keyboard navigation
- `ui::SettingsWindow` (modal, sections in a left list): Recording (fps, codec, quality, countdown, cursor rate, keystrokes mode with a privacy note, output folder), Audio (mic device, system audio on/off, meters test), Camera (device, resolution), Hotkeys (recorders with conflict detection), Editor (autosave interval, cache MB, snap/ripple defaults, preview quality), Export (default preset, folder), Privacy (crash dumps on/off, "Open data folder"), About (version, git hash, build date, encoder report, GPU, "Copy diagnostics", licenses). Changes apply immediately and are saved through the 500 ms debounce from docs/03 section 11; hotkeys re-register.
- First run: create folders, show a one-time toast "Press F9 to start recording. Recordings are saved to Videos\super4vid", no other dialogs.
- Keyboard: bar actions reachable with Tab/Enter (ImGui nav), editor transport and timeline selection via Tab, all dialogs have default/cancel buttons bound to Enter/Esc.

## 11.7 Acceptance
Fill the whole section-8 table in `PROGRESS.md` with measured numbers and the commit hash they were measured at. Any budget missed stays an open `[~]` item with a `DECISIONS.md` note explaining the gap and the plan.

## Pitfalls
- Measuring CPU with Task Manager alone is noisy; use `GetProcessTimes` deltas printed by `--stats` over >= 60 s.
- Debug-layer builds are 3-10x slower; all numbers come from Release without the debug layer.
- A WPR trace with the GPU provider is large; stop after 60 s.
- "0 redraws" means `redraws/s` from `--stats` is 0 for a full 60 s while the mouse is outside our windows; a blinking caret in an ImGui text field forces redraws (acceptable only while a text field is focused).
