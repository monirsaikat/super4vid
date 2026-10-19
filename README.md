# super4vid

A native Windows screen recorder and video editor. It records your screen, microphone, system audio and webcam,
then lets you edit on a timeline with the modern "Screen Studio" look: a re-rendered smooth cursor, automatic
zoom-ins on clicks, and padded backgrounds with rounded corners and shadows.

Written in C++20 on Win32, Direct3D 11, Media Foundation and WASAPI, with Dear ImGui for the interface. It ships as
one executable with no runtimes or redistributables.

> Working title. Version 1.0 is under development; see [Status](#status).

## Highlights

| Area | What you get |
|---|---|
| Recording | Full monitor, region or window rectangle, any DPI, HDR tone-mapped to SDR. 15-60 fps (native up to 144). Hardware H.264 or HEVC with automatic software fallback. |
| Audio | Microphone and system audio as separate 48 kHz AAC tracks, live meters, survives device changes. |
| Cursor and keys | Recorded as data, not pixels, so the editor redraws the cursor sharp, smoothed and at any size. Keystrokes are "shortcuts only" by default. |
| Workflow | `F9` start/stop, `F10` pause, `F8` marker, 3-2-1 countdown, tray icon. The recorder bar never shows in the recording. |
| Editor | Unlimited tracks; trim, split, ripple, slip, snap, speed; audio gain, fades, normalize, noise gate; markers; unlimited undo, autosave and crash recovery. |
| Look | Auto-Zoom from clicks and typing, Auto-Follow cursor, padding, gradient/solid/wallpaper backgrounds, rounded corners, shadows, aspect presets 16:9, 9:16, 1:1, 4:5. |
| Annotations | Text, callouts, arrows, shapes, highlight, blur/pixelate, keystroke badges, webcam picture-in-picture, transitions. |
| Export | MP4 H.264/HEVC (720p to 4K), GIF with its own encoder, PNG stills, M4A audio. Runs in the background with progress and cancel. |
| Performance targets | Starts in under 0.5 s, 1080p60 recording under 10 % CPU, zero CPU pixel copies, zero redraws when idle. |
| Privacy | Nothing leaves the machine. No telemetry. |

Requires Windows 10 version 2004 (build 19041) or later, x64.

## Status

Snapshot of [PROGRESS.md](PROGRESS.md) on 2026-10-17. PROGRESS.md is the source of truth.

| Phase | Area | Status |
|---|---|---|
| P00 | Toolchain, skeleton app, logging, crash dumps | Done; some manual checks by a person still open |
| P01 | Core library | Done |
| P02 | Screen capture | Tasks done; 4K budget and lock/unlock check still open |
| P03 | Audio capture | Tasks done; manual device hot-swap check still open |
| P04 | Encode and mux (MP4, AAC) | Not started |
| P05 | Recorder app (bar, hotkeys, overlay, webcam, `.ctrec`) | Not started |
| P06 | Decode and playback | Not started |
| P07 | Project model, undo, autosave, Auto-Zoom | Done |
| P08 | Compositor | Not started |
| P09 | Editor UI | Not started |
| P10 | Export | Not started |
| P11 | Performance and polish | Not started |
| P12 | Installer and release | Installer, packaging and CI done; release checks open |

What runs today: the app window, `capture_bench` and `audio_bench`, the test suites, and `build.cmd package`.
Recording to a file and the editor are not available yet.

## Build

You need Visual Studio 2022 Build Tools (MSVC 14.44+) with the Windows 11 SDK 10.0.26100. Inno Setup 6 is only
needed for packaging.

```
build.cmd doctor     check the toolchain
build.cmd release    build Release -> out/build/ninja-release/bin/super4vid.exe
build.cmd debug      build Debug   -> out/build/ninja-debug/bin/
build.cmd test       build both configs and run all tests
build.cmd package    installer, portable zip and symbols -> out/dist/
```

On a machine with little RAM, run `set CMAKE_BUILD_PARALLEL_LEVEL=2` first.

## Documentation

| Read | For |
|---|---|
| [docs/user-guide.md](docs/user-guide.md) | Using super4vid: install, record, edit, export, troubleshooting |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Working on the code: build, layout, conventions, tests, commits |
| [MASTER_PLAN.md](MASTER_PLAN.md) | Scope, architecture, phases and performance budgets |
| [docs/](docs/) | Detailed specs: toolchain, conventions, architecture, file formats, UI, testing |
| [DECISIONS.md](DECISIONS.md) | Every deviation from the specs, with reasons |

## Licence

Dear ImGui is MIT-licensed (`third_party/imgui/LICENSE.txt`). The product licence is in `installer/LICENSE.txt`
and is a placeholder until the owner chooses one.
