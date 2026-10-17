# Camtesia — Master Build Plan

> **Entry point for every agent or model working in this repository.**
> Read this file first, every session. Then read `PROGRESS.md`, then the one phase file you were assigned
> (`docs/phases/Pxx-*.md`). Load other docs only when a phase file tells you to.

---

## 1. What we are building

Camtesia is a **native Windows screen recorder and video editor** in the spirit of Camtasia, finished with the
modern "Screen Studio" look: recordings get smooth re-rendered cursors, automatic zoom-ins on clicks, padded
backgrounds with rounded corners and shadows, keystroke badges, and a clean dark editor.

It is a **single executable (~15 MB)** that starts in under half a second, records the screen, microphone, system
audio and webcam through a **GPU pipeline with zero CPU pixel copies**, and edits/exports on a timeline.

Working title only: "Camtesia" is one letter from TechSmith's trademark "Camtasia". The code uses the neutral
namespace `ct` and prefix `CT_`, so renaming before a public release touches only strings and resources.

## 2. Non-negotiables

| Topic | Rule |
|---|---|
| Platform | Windows 10 2004 (build 19041) and later, x64 only. Windows 11 is the primary test target. |
| Language | C++20, MSVC v143 (14.44+). No C#, no Rust, no scripting runtimes in the shipped product. |
| Exceptions / RTTI | **Off** (`/EHs-c- /GR- /D_HAS_EXCEPTIONS=0`). Errors are `HRESULT`/`bool` return values. |
| Frameworks | **None.** No Qt, WPF, WinUI, Electron, .NET, Boost, FFmpeg, OpenCV, SDL. |
| Allowed third party | Dear ImGui (docking branch, MIT) vendored in `third_party/imgui`. That is the entire list. |
| Windows APIs | Win32, Direct3D 11, DXGI 1.6, Direct2D 1.1, DirectWrite, WIC, Media Foundation, WASAPI, MMCSS, DWM, Shell, DbgHelp, TraceLogging. All from the Windows SDK 10.0.26100. |
| Build | CMake >= 3.25 + Ninja + MSVC. One command (`build.cmd release`) builds everything incl. tests. |
| Runtime deps | Static CRT (`/MT`). The exe runs on a clean Windows install with no redistributables. |
| Performance | Budgets in section 8 are acceptance criteria, not wishes. A phase is not done if its budget fails. |
| Quality gate | `/W4 /WX` on all first-party code. Debug build runs clean under the D3D11 debug layer. |
| Privacy | Keystroke recording defaults to "shortcuts only". Nothing ever leaves the machine. No telemetry. |

## 3. Feature scope v1.0

**Recorder**
- Capture: full monitor, custom region (drag or snap to window), any monitor, any DPI, HDR monitors tone-mapped to SDR.
- Frame rates 15/24/30/60 (and monitor-native up to 144 when hardware allows). Constant frame rate output.
- Hardware H.264 (default) or HEVC via Media Foundation; software fallback automatically.
- Microphone + system audio as **separate** tracks, 48 kHz AAC. Live level meters. Device hot-swap survives.
- Webcam as a separate synchronized file (PIP later in the editor).
- Cursor **recorded as data** (position at 120 Hz, clicks, wheel, cursor shapes) so the editor re-renders it crisp, smoothed and scalable.
- Keystrokes recorded as data (shortcuts-only by default; opt-in all keys).
- Hotkeys (default F9 start/stop, F10 pause), 3-2-1 countdown, system tray, recording bar hidden from the capture.
- Output: a `Name.ctrec` folder bundle (`screen.mp4`, optional `cam.mp4`, `events.ctev`, `meta.json`).

**Editor**
- Project `.ctproj` (JSON). Import `.ctrec`, MP4/MOV/M4A/MP3/WAV/PNG/JPG.
- Timeline: unlimited tracks, clips with trim/split/ripple/snap/slip, speed change, audio gain/fades/mute, markers.
- Camera: manual zoom/pan keyframes with easing, **Auto-Zoom** generated from click events, **Auto-Follow** cursor.
- Cursor: size, smoothing, click ripple/highlight, hide when idle, spotlight.
- Background: padding, solid/gradient/wallpaper, rounded corners, drop shadow, aspect-ratio presets (16:9, 9:16, 1:1, 4:5).
- Annotations: text, callout, arrow, rectangle, ellipse, highlight, blur/pixelate region, keystroke badges. All keyframeable opacity/position.
- Webcam PIP: corner presets, size, circle/rounded mask, shadow.
- Transitions: cut, crossfade, dip to color, slide.
- Preview: 1080p60 real time, scrub, J/K/L, loop region. Undo/redo unlimited, autosave, crash recovery.

**Export**
- MP4 H.264 / HEVC (hardware), presets 720p/1080p/1440p/4K, CBR/VBR/quality, AAC 128-320 kbps.
- GIF (own encoder, palette + dithering), PNG still, audio-only M4A.
- Background export with progress and cancel.

**Distribution**
- Inno Setup installer (`CamtesiaSetup-x.y.z.exe`), portable ZIP, file association for `.ctproj`, Start menu shortcut, clean uninstall, version info, icon, PDB symbols archived per release.

**Explicitly out of scope for v1.0:** macOS/Linux, plugins, cloud, stock media, motion tracking, multicam, speech-to-text captions, timeline nesting, 10-bit/HDR output, audio effects beyond gain/fade/normalize/noise gate.

## 4. Architecture at a glance

```
                       +----------------------- camtesia.exe (one process) ----------------------------+
                       |                                                                              |
  Desktop --DXGI dup-->| capture --BGRA tex--> convert(HLSL)--NV12 tex--> media::Encoder --> MP4 sink |
  Cursor/keys -hooks-->| events --------------------------------------------------------> events.ctev |
  Mic / System -WASAPI>| audio --float48k--> resample/mix --PCM16--> AAC ---------------> MP4 sink    |
  Webcam --MF reader-->| capture --NV12 tex-------------------------------> Encoder --> cam.mp4       |
                       |                            ^ session clock = QPC epoch, all streams stamped  |
                       |   recorder (session state machine, hotkeys, tray, bar, overlay)              |
                       |------------------------------------------------------------------------------|
                       |   project (model, JSON, undo)  --> render::Compositor(t) --> D3D11 RTV       |
  .ctrec/.mp4 --MF---->|   playback (decoders, frame cache, audio engine, clock)  --> preview/export  |
                       |   export (offline loop: Compositor + audio mix -> Encoder -> MP4/GIF)        |
                       |------------------------------------------------------------------------------|
                       |   ui (Win32 window + D3D11 flip swapchain + Dear ImGui, theme, timeline)     |
                       |   core (strings, time, log, queues, pools, json, fs)  platform (win32, com)  |
                       +------------------------------------------------------------------------------+
```

Key design decisions (details in `docs/03-architecture.md`):
- **One D3D11 device** shared by capture, conversion, encoder (via `IMFDXGIDeviceManager`), decoder, compositor and UI. Textures never round-trip through system memory.
- **One clock.** Every sample (video, audio, events) is stamped from `QueryPerformanceCounter` relative to the session epoch, in microseconds. Media Foundation gets 100 ns units only at its boundary.
- **Deterministic compositor.** `RenderFrame(project, t_us) -> texture` is the single function used by both the preview and the exporter. Pixel-identical output.
- **Record data, not pixels, for cursor and keys.** The cursor is not baked into the video.
- **Snapshot undo.** Every edit serializes the project to JSON into an undo stack. Simple and bulletproof.
- **Event-driven UI.** Zero redraws when idle. Wait on the swapchain waitable object + input events.

## 5. Repository layout

```
camtesia/
|- MASTER_PLAN.md            <- you are here
|- PROGRESS.md               <- task checklist; the only place task status lives
|- DECISIONS.md              <- every deviation from the spec, with reason
|- CMakeLists.txt  CMakePresets.json  build.cmd  build.ps1
|- docs/
|  |- 01-toolchain-and-build.md   02-conventions.md   03-architecture.md
|  |- 04-file-formats.md          05-ui-design.md     06-testing-and-performance.md
|  |- 07-api-reference-map.md     (which Microsoft Learn page to read for which API)
|  '- phases/P00 ... P12, PX-optional.md
|- src/
|  |- core/       ct_core       strings, time, log, assert, alloc, spsc queue, pool, json, fs, thread, config
|  |- events/     ct_events     .ctev reader/writer, EventLog time queries (pure; shared by recorder, project, render)
|  |- platform/   ct_platform   win32 window, dpi, d3d device, com/mf init, hotkeys, hooks, tray, paths, crash
|  |- capture/    ct_capture    dxgi duplication, region, cursor shapes, event recorder, webcam source
|  |- audio/      ct_audio      wasapi capture/render, resampler, mixer, meters, device notifications
|  |- media/      ct_media      mf encoder+sink writer, mf source reader, media info, gif encoder, thumbnails
|  |- recorder/   ct_recorder   RecordingSession state machine, output bundle writer
|  |- project/    ct_project    model types, (de)serialization, undo, autosave, auto-zoom generator
|  |- render/     ct_render     compositor, shader library, text (D2D/DWrite), cursor renderer, effects
|  |- playback/   ct_playback   playback clock, decoder cache, audio engine (WASAPI render)
|  |- export/     ct_export     export jobs (mp4/gif/png/m4a), presets
|  |- ui/         ct_ui         imgui shell, theme, widgets, timeline widget, panels, recorder bar, overlay
|  '- app/        camtesia.exe  main, modes, single instance, resources, manifest
|- shaders/        *.hlsl -> compiled by fxc into headers at build time
|- third_party/imgui/
|- assets/         icon.ico, presets/*.json, test media
|- tests/          one test exe per module + tests/data
|- tools/          scripts (icon generation, bench runners, release)
'- installer/      camtesia.iss, installer assets
```

Each `src/<module>` is a static library `ct_<module>` with a `CMakeLists.txt`, `include/ct/<module>/*.h` public
headers and `src/*.cpp`. Dependencies only point downward in this order:
`core <- {events, platform} <- {capture, audio, media} <- {recorder, playback} <- {project, render} <- export <- ui <- app`
(`events` depends on `core` only; `project` depends on `core` and `events`).

## 6. Phase index

Work strictly in order unless the table says phases are independent. Each phase file is self-contained.

| # | Phase file | Produces | Depends on | Size |
|---|---|---|---|---|
| P00 | `P00-toolchain-skeleton.md` | Builds, Win32 window, D3D11 swapchain, ImGui hello, logging, crash dumps, tests run | - | S |
| P01 | `P01-core-library.md` | strings/utf, time, SPSC queue, pools, JSON, fs, threads, config, ETW | P00 | M |
| P02 | `P02-screen-capture.md` | D3D device service, DXGI Desktop Duplication, region/monitor model, BGRA->NV12 GPU convert, cursor shape capture | P01 | L |
| P03 | `P03-audio-capture.md` | WASAPI mic + loopback, resampler, meters, device change handling | P01 | M (independent of P02) |
| P04 | `P04-encode-mux.md` | MF hardware encoder + MP4 sink writer, AAC, multi-stream, texture pool, software fallback | P02, P03 | L |
| P05 | `P05-recorder-app.md` | RecordingSession, event recorder (hooks), hotkeys, countdown, region overlay, recorder bar, tray, webcam, `.ctrec` bundle | P04 | L |
| P06 | `P06-decode-playback.md` | MF source reader (HW decode to textures), audio decode, seek, frame cache, A/V clock, WASAPI render | P02 | L (independent of P03-P05) |
| P07 | `P07-project-model.md` | Model types, JSON (de)serialization, undo/redo, autosave, import `.ctrec`, auto-zoom generator | P01 + `ct_events` (from P05, or created in P07) | M (independent of P02-P06) |
| P08 | `P08-compositor.md` | Deterministic frame renderer: layers, transforms, rounded corners, shadows, cursor, annotations via D2D, transitions, camera | P06, P07 | L |
| P09 | `P09-editor-ui.md` | Editor window: panels, timeline widget, preview, properties, shortcuts, theme | P08 | XL |
| P10 | `P10-export.md` | Offline render + encode, presets, GIF encoder, progress/cancel | P08, P04 | M |
| P11 | `P11-performance-polish.md` | Budgets verified with measurements, idle power, memory, HiDPI, multi-monitor, error UX, settings | P05, P09, P10 | M |
| P12 | `P12-installer-release.md` | Inno Setup installer, portable ZIP, version stamping, CI workflow, release checklist | P11 | S |
| PX | `PX-optional.md` | Windows.Graphics.Capture window capture, fragmented MP4 crash safety, per-process audio, custom title bar, auto-update | after P12 | - |

Suggested parallelization if several agents work at once: {P02, P03, P07} after P01; {P06} after P02; {P04} after P02+P03.

## 7. Operating protocol for agents

1. **Read order each session:** this file -> `PROGRESS.md` -> your phase file -> only the docs your phase file lists under "Read first".
2. **Pick the first unchecked task** of your phase in `PROGRESS.md`. Do not reorder tasks. Do not start the next phase before every acceptance criterion of the current one is ticked.
3. **For each task:** read the interface skeleton in the phase file -> implement -> `build.cmd debug` and `build.cmd release` both green with zero warnings -> `build.cmd test` green -> do the "Manual check" listed -> commit with message `Pxx: <what> (task x.y)` -> tick the box in `PROGRESS.md` in the same commit.
4. **Never:** add a dependency, disable a warning globally, use exceptions, allocate on the capture/audio hot paths after start, call blocking Media Foundation or file APIs on the UI thread, leave a `TODO` without a matching unchecked item in `PROGRESS.md`.
5. **If the spec cannot be followed** (API not available, behaviour differs): first try the documented fallback in the phase file. If none fits, choose the closest option from section 9, implement it, and append an entry to `DECISIONS.md` (`date, phase/task, what the spec said, what you did, why`). Do not silently diverge.
6. **If a Windows API fails twice:** log the `HRESULT` through `ct::HrToString`, check the D3D11 debug layer / MF trace output in Debug, open the Microsoft Learn page listed in `docs/07-api-reference-map.md`, verify every attribute/flag against it, then retry. Only then record a decision.
7. **Keep context small.** Phase files repeat what you need; do not load the whole `docs/` folder. Headers are the API contract between phases: do not change a public header signature from an earlier phase without a `DECISIONS.md` entry.
8. **Definition of done for a phase:** all tasks ticked, all tests pass in Debug and Release, all acceptance criteria measured and the numbers written next to the criterion in `PROGRESS.md`, no new warnings, `DECISIONS.md` updated, Debug run shows no D3D11 debug-layer errors and no leaked COM objects at exit.

## 8. Performance budgets (acceptance criteria)

Measured on the reference machine (Ryzen 7 5700G, integrated Radeon, 1080p and 4K monitors) in Release. Numbers go into `PROGRESS.md`.

| Metric | Budget |
|---|---|
| Cold start -> recorder bar visible | < 500 ms (warm < 300 ms) |
| Idle CPU, bar visible, not recording | < 0.5 %, 0 redraws/s |
| Hotkey -> first captured frame | < 150 ms (excluding countdown) |
| Recording 1920x1080@60, H.264 HW, mic+system | total CPU < 10 % of the machine, GPU < 15 %, 0 dropped frames over 10 min, RSS < 250 MB |
| Recording 3840x2160@60, H.264 HW | total CPU < 25 %, 0 dropped frames over 5 min of normal desktop use |
| A/V sync drift after 60 min recording | < 20 ms (measured with the clap test in `docs/06`) |
| Editor UI | 60 fps while dragging on a timeline with 1,000 clips; 0 redraws when idle |
| Scrub latency | < 50 ms cached frame, < 150 ms cold seek at 1080p H.264 |
| Preview playback | 1080p60 with 3 video layers + cursor + 2 annotations, no dropped frames |
| Export 1080p60 H.264 HW, 3 layers | >= 1.5x real time |
| Memory, editor with a 30 min 1080p recording open | RSS < 600 MB |
| Binary size / installer / installed | exe < 20 MB, installer < 15 MB, installed < 60 MB |

## 9. Decision defaults (use these when the spec is silent)

| Question | Default |
|---|---|
| Video codec/profile | H.264 High profile, level auto, 4:2:0 8-bit, BT.709 limited range, GOP = 2 s, B-frames off for recording, on for export if supported |
| Rate control | Recording: Quality mode 70 with VBR peak cap (1080p60: 12 Mbps, 4K60: 40 Mbps). Export: user choice, default VBR 1080p 10 Mbps |
| Recording fps | 60 if the monitor refresh >= 60 Hz else 30 |
| Audio | 48 kHz, stereo, float internally, AAC 192 kbps per track |
| Canvas | Matches the first imported recording's size; fps matches too |
| Fonts | UI: "Segoe UI Variable" falling back to "Segoe UI"; mono: "Cascadia Mono" falling back to "Consolas". Annotations default "Segoe UI" 48 px. No bundled fonts. |
| Icons | Segoe Fluent Icons (Win11) falling back to Segoe MDL2 Assets (Win10). No icon files. |
| Paths | Recordings: `%USERPROFILE%\Videos\Camtesia\`; settings: `%APPDATA%\Camtesia\settings.json`; logs+crash dumps+autosave: `%LOCALAPPDATA%\Camtesia\` |
| Hotkeys | F9 start/stop, F10 pause/resume, F8 add marker, Ctrl+Shift+F9 cancel. Configurable. |
| Timestamps | `int64_t` microseconds everywhere; 100 ns only inside `ct_media` at MF calls |
| Threads | One owner per resource, SPSC queues between, `SRWLOCK` for rarely-shared state |
| Unknown HRESULT | Log it, surface a plain-language message in the UI, never crash, never lose an in-progress recording silently |
| UI language | English only in v1.0, but all user-visible strings live in `src/ui/strings.h` |

## 10. Document map

| File | Read when |
|---|---|
| `docs/01-toolchain-and-build.md` | P00 and whenever a build command or flag is in question |
| `docs/02-conventions.md` | P00 and before writing any code in any phase |
| `docs/03-architecture.md` | P02, P04, P05, P06, P08 (threads, clocks, data flow, failure handling) |
| `docs/04-file-formats.md` | P05 (`.ctev`, `.ctrec`, `meta.json`), P07 (`.ctproj`, settings) |
| `docs/05-ui-design.md` | P05 (recorder bar/overlay) and P09 (editor) |
| `docs/06-testing-and-performance.md` | P00 (harness), P11 (measurement procedures), every phase's test section |
| `docs/07-api-reference-map.md` | whenever a Windows API behaves unexpectedly |
