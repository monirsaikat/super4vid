# 03 · Architecture

Module boundaries, threads, clocks, data flows, memory plan, and failure handling. Phase files refer to the
section numbers here.

## 1. Module dependency graph

```
app ──► ui ──► export ──► render ──► playback ──► media ──► platform ──► core
        │        │          │  └────► project ─────────────────┘            ▲
        │        │          └────────► capture ───────────────┘            │
        └──► recorder ──► capture, audio, media                             │
                          audio ─────────────────────────────► platform ───┘
```

Rules: a module links only to modules to its right; `ui` never includes Media Foundation headers; `render` and
`playback` never include ImGui; `project` includes nothing Windows-specific except `core` and `events`.

| Module | Owns | Key public types |
|---|---|---|
| core | strings, time, log, error, containers, threads, json, fs, config, trace | `Clock`, `SpscQueue<T>`, `ObjectPool<T>`, `Json`, `Thread`, `Settings` |
| events | `.ctev` reader/writer and typed event log with time queries (pure logic, depends on `core` only; beside `platform` in the graph) | `EventHeader`, `EventRecord`, `EventWriter`, `EventReader`, `EventLog` |
| platform | Win32 window, DPI, D3D11 device + context lock, COM/MF runtime, hotkeys, hooks thread, tray, known paths, crash dumps | `Window`, `D3DDevice`, `ContextLock`, `MfRuntime`, `HotkeyManager`, `TrayIcon`, `CrashHandler` |
| capture | displays, desktop duplication, BGRA->NV12 conversion, cursor tracker, webcam source, capture pacing | `DisplayInfo`, `DesktopDuplicator`, `ColorConverter`, `CursorTracker`, `WebcamSource`, `ScreenCapturer` |
| audio | WASAPI capture/loopback/render, resampler, mixer, meters, device notifications | `AudioDevices`, `WasapiCapture`, `WasapiLoopback`, `WasapiRender`, `Resampler`, `Meters` |
| media | encoder enumeration, MP4 sink writer, source reader, media info, GIF encoder, image IO (WIC) | `EncoderCaps`, `Mp4Writer`, `VideoDecoder`, `AudioDecoder`, `MediaInfo`, `GifEncoder`, `ImageIo` |
| recorder | recording session state machine, event recorder, bundle writer | `RecordingSession`, `EventRecorder`, `BundleWriter` |
| project | model, serialization, edit ops, undo, autosave, import, auto-zoom | `Project`, `Clip`, `Keyframe<T>`, `EditOps`, `UndoStack`, `RecImporter`, `AutoZoom` |
| playback | decoder cache, prefetch, audio engine, clock | `MediaCache`, `PlaybackClock`, `AudioEngine`, `Player` |
| render | compositor and all GPU passes, D2D text, cursor renderer, effects | `Compositor`, `RenderContext`, `TextRenderer`, `CursorRenderer`, `Effects` |
| export | export jobs and presets | `ExportJob`, `ExportPreset` |
| ui | ImGui shell, theme, widgets, timeline, panels, recorder bar, overlays | `App`, `Theme`, `TimelineWidget`, `PreviewPanel`, `RecorderBar`, `RegionOverlay` |
| app | `wWinMain`, command line modes, single instance | - |

## 2. Process model and app modes

One process, one executable. Command line:

```
Camtesia.exe                         -> recorder bar (default)
Camtesia.exe --editor [file.ctproj|folder.ctrec]
Camtesia.exe --record --monitor 1 --fps 60 --out "D:\x.ctrec" [--seconds N]   (headless, for tests/benchmarks)
Camtesia.exe --stats                 -> print counters to console (attach with AllocConsole)
Camtesia.exe --selftest              -> runs the built-in smoke test (capture 3 s -> verify file) and exits 0/1
```

Single instance: `CreateMutexW(nullptr, FALSE, L"Local\\Camtesia.Instance")`. If it already exists, find the
window class `CamtesiaMain` and send the command line via `WM_COPYDATA`, then exit. Recorder bar and editor are
two top-level windows of the same process; both share the `D3DDevice`.

## 3. Threads

| Thread name | Owner | Priority / MMCSS | Loop |
|---|---|---|---|
| `ct-ui` | app | normal | `MsgWaitForMultipleObjectsEx` on {swapchain waitable, wake event}; renders ImGui only when dirty |
| `ct-capture` | ScreenCapturer | `THREAD_PRIORITY_TIME_CRITICAL`, MMCSS "Capture" | tick timer -> `Acquire` -> crop copy -> convert -> push NV12 to encoder queue |
| `ct-encode` | Mp4Writer | `THREAD_PRIORITY_ABOVE_NORMAL`, MMCSS "Playback" | pop frame/audio packets -> `WriteSample` in timestamp order |
| `ct-audio-mic` | WasapiCapture | MMCSS "Pro Audio" | event-driven `GetBuffer` -> convert -> push packets |
| `ct-audio-sys` | WasapiLoopback | MMCSS "Pro Audio" | 5 ms timer poll -> `GetBuffer` -> silence fill -> push packets |
| `ct-audio-out` | AudioEngine (editor) | MMCSS "Pro Audio" | event-driven render; mixes timeline audio |
| `ct-hooks` | EventRecorder | `THREAD_PRIORITY_HIGHEST` | `SetWindowsHookEx` LL mouse/keyboard + message loop; cursor poll timer at 120 Hz |
| `ct-events-io` | EventRecorder | normal | drains event ring buffer to `events.ctev` every 1 s |
| `ct-webcam` | WebcamSource | above normal | blocking `ReadSample` loop -> NV12 textures -> second encoder |
| `ct-decode-N` | MediaCache (one per open media) | above normal | prefetch frames around the playhead into the cache |
| `ct-io-pool` (2 threads) | core::Async | normal | thumbnails, file IO, media info, autosave |
| `ct-export` | ExportJob | below normal (user may keep editing) | offline render loop |

The UI thread never waits on any of these threads. Shutdown order is the reverse of startup, each thread gets a
stop flag + wake event and is joined with a 5 s timeout, after which `CT_FATAL` writes a dump (a hung thread is a bug).

## 4. Clocks and timestamps

```
QPC ─────────────────────────────────────────────────────────────────────────► wall time
       ▲ epoch = RecordingSession::Start()   (also stored in meta.json as qpc + UTC)
       │
video: tick n  -> t_us = n * 1e6 / fps   (tick-aligned, generated by the pacer, NOT by the desktop's present time)
audio: packet  -> t_us = (packet.qpc_100ns*100/1000) - epoch_us   (device QPC position from GetBuffer)
events:        -> t_us = NowUs() in the hook/poll thread
pause:         -> t_us of every stream is shifted by the accumulated pause duration (media time has no gaps)
```

- Why tick-aligned video: desktop duplication only delivers frames when something changed. The pacer emits a frame
  every tick; if no new desktop frame arrived, the previous NV12 texture is re-sent. The encoder therefore sees
  perfectly constant frame rate, which editors and players handle best.
- Why audio uses device positions: WASAPI's QPC positions are the ground truth for when a sample was captured;
  the first packet's position defines the audio stream offset relative to the epoch. Gaps (discontinuity flag,
  device switch) are filled with silence computed from the positions, so audio never drifts relative to video.
- Drift between the audio device clock and QPC exists (tens of ppm). Over 1 h that is up to ~100 ms. The
  recorder measures drift = (samples_received / 48000) - (last_qpc - first_qpc) every 10 s and, if it exceeds
  10 ms, inserts or drops single samples spread across the next 10 s (inaudible). This keeps A/V within budget.
- Editor/playback: the master clock is the audio render position (`IAudioClock::GetPosition`) when audio plays, else QPC.

## 5. Recording data flow (per frame, 1080p60)

```
tick (16.67 ms)
 ├─ ctx.AcquireNextFrame(0..timeout)          GPU texture, no CPU bytes
 ├─ CopySubresourceRegion(crop -> bgra_pool[i])  (also serves as "last frame" for repeats)
 ├─ ReleaseFrame()
 ├─ ColorConverter: 2 fullscreen pixel-shader passes -> nv12_pool[j]  (Y plane RTV, UV plane RTV)
 ├─ encoder_queue.push({nv12_pool[j], t_us})  (SPSC, capacity 8; if full -> count drop, recycle texture)
 └─ stats: acquire_ms, convert_ms, queue_depth, repeats, drops

ct-encode:
 ├─ pop in order; for video: MFCreateDXGISurfaceBuffer -> IMFSample(t_100ns, dur) -> WriteSample(video_idx)
 ├─ for audio packets: PCM16 interleaved -> IMFSample -> WriteSample(mic_idx / sys_idx)
 └─ interleave rule: never let video get more than 500 ms ahead of audio or vice versa (the sink writer buffers, but bounded)
```

GPU work per frame: 1 copy (region) + 2 draws. CPU work per frame: ~50 µs of API calls. The hardware encoder reads
the NV12 texture directly through the DXGI device manager.

## 6. Playback data flow

```
UI: playhead t ─► Player::Seek/Play
                 ├─ for each clip visible at t: MediaCache(media).GetFrame(clip_time) -> NV12 texture (cache hit) or request prefetch
                 ├─ Compositor::RenderFrame(project, t, target)   deterministic, GPU
                 └─ AudioEngine mixes clips' audio at the device callback; its position drives the clock
ct-decode-N: keeps [t - 0.5 s, t + 2 s] of its media decoded, sequential reads; on far seek: SetCurrentPosition then decode forward to t
```

## 7. Export data flow

```
ct-export: for n in frames: t = n*1e6/fps
   ├─ MediaCache sequential reads (fast path, no cache thrash, prefetch depth 8)
   ├─ Compositor::RenderFrame(project, t, nv12-capable RGBA target) -> ColorConverter -> NV12 -> Mp4Writer
   ├─ audio: AudioMixer::Render(t, t + frame_dur) -> float -> PCM16 -> Mp4Writer
   └─ progress = n / total; cancel flag checked every frame; on cancel delete the partial file
```

## 8. GPU resources

- One `ID3D11Device5`/`ID3D11DeviceContext4` (feature level 11_0 minimum, `D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT`), `ID3D10Multithread::SetMultithreadProtected(TRUE)`, immediate context guarded by `ContextLock` (critical section) for every use outside the UI thread.
- `IMFDXGIDeviceManager` created once, `ResetDevice` once; handed to sink writers and source readers.
- Texture pools: BGRA capture pool (3), NV12 encode pool (12, with reuse check), decoder output copies (per media, cache-capped), compositor targets (canvas + 2 ping-pong), UI swapchain (2 buffers, flip discard, waitable).
- Shaders: compiled offline by `fxc` into headers; created once in `RenderContext::Init`.
- Device removal (`DXGI_ERROR_DEVICE_REMOVED`/`RESET`): recorder finalizes the current file immediately with what it has, shows an error, recreates the device on next action. Editor recreates the device and all pools, invalidates caches, re-renders.

## 9. Memory budgets

| Component | Budget |
|---|---|
| Recording pools (1080p60) | BGRA 3 x 8 MB + NV12 12 x 3 MB = 60 MB GPU |
| Recording pools (4K60) | 3 x 33 MB + 12 x 12.4 MB = 250 MB GPU |
| Encoder queue | 8 frames |
| Audio ring buffers | 2 s per stream (48 kHz x 2 ch x 4 B x 2 s = 768 KB) |
| Event ring buffer | 64 K records (~4 MB) |
| Decoded frame cache | 256 MB total, LRU, configurable in settings |
| Undo stack | 200 snapshots or 64 MB, whichever first |
| Logger | 1 MB ring, file rotates at 10 MB, keep 5 files |

## 10. Failure matrix

| Event | Detection | Response |
|---|---|---|
| Secure desktop (UAC prompt, Ctrl+Alt+Del) | `AcquireNextFrame` -> `DXGI_ERROR_ACCESS_LOST`, `DuplicateOutput` -> `E_ACCESSDENIED` | Keep emitting repeats of the last frame; retry `Reinit()` every 250 ms; log once |
| Resolution / mode / monitor change | `DXGI_ERROR_ACCESS_LOST`, `WM_DISPLAYCHANGE` | `Reinit()`; if size changed, keep encoding at the original size by letterboxing the new frames (never restart the file) |
| GPU device removed | `GetDeviceRemovedReason` != S_OK | Finalize file, notify user, rebuild device lazily |
| Mic unplugged | `IMMNotificationClient::OnDefaultDeviceChanged` / `AUDCLNT_E_DEVICE_INVALIDATED` | Fill silence, switch to the new default device, keep stream timestamps continuous |
| No audio playing (loopback starves) | No packets for > 20 ms | Insert silence by elapsed time (and keep a silent render stream open so the endpoint stays active) |
| Disk full / write error | Sink writer returns `HRESULT_FROM_WIN32(ERROR_DISK_FULL)` or `E_FAIL` on `WriteSample` | Stop capture, attempt `Finalize()`, show the path and free-space message |
| Encoder rejects frame rate/size (e.g. odd dimensions) | `SetInputMediaType` fails | Round region width/height down to even numbers before configuring (always), then retry with the software encoder |
| Hook thread stalls (>300 ms) | Windows silently removes LL hooks | Hook callback only enqueues; watchdog re-installs the hook if no `WM_NULL` heartbeat arrives for 1 s |
| Sleep / resume while recording | `WM_POWERBROADCAST PBT_APMSUSPEND` | Pause the session on suspend, resume on `PBT_APMRESUMEAUTOMATIC` as paused, user resumes manually |
| Crash | `SetUnhandledExceptionFilter` | Write minidump, flush logs and `events.ctev`, attempt `Mp4Writer::Finalize()` from the filter (best effort), exit |
| App killed while recording | n/a | `events.ctev` loses <= 1 s (flushed every second). MP4 via the sink writer loses the whole file (no moov) unless the optional fragmented-MP4 path (PX) is enabled. Document this in the UI ("Recovering…" is not possible for v1 files). |

## 11. Settings and paths

- Settings file: `%APPDATA%\Camtesia\settings.json` (schema in `docs/04`). Loaded at start, saved on change (debounced 500 ms; the settings window calls Save on every change and the debounce coalesces them), atomic write (temp + `ReplaceFileW`).
- Recordings: `%USERPROFILE%\Videos\Camtesia\<yyyy-MM-dd HH-mm-ss>.ctrec\`. Project autosave: `%LOCALAPPDATA%\Camtesia\autosave\<project-id>.ctproj`.
- Logs: `%LOCALAPPDATA%\Camtesia\logs\`. Crash dumps: `%LOCALAPPDATA%\Camtesia\crashes\`.
- Portable mode: if `portable.txt` exists next to the exe, all of the above move under `<exe dir>\data\` (including user presets and `editor_layout.ini`).
