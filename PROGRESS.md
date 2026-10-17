# PROGRESS

Single source of truth for task status. Tick a box only in the same commit that completes the task.
Write measured numbers next to acceptance criteria when you tick them (e.g. `- [x] cold start < 500 ms — measured 310 ms`).

Legend: `[ ]` not started · `[~]` in progress (one per agent at a time) · `[x]` done · `[-]` dropped (needs a DECISIONS.md entry)

## P00 Toolchain & skeleton
- [x] 0.1 Toolchain verified (`build.cmd doctor` prints all green) — MSVC 14.44.35207, CMake 3.31.6, Ninja 1.12.1, SDK 10.0.26100.0; Inno Setup MISSING (deferred to P12, see DECISIONS)
- [ ] 0.2 Repository scaffold, root CMake, presets, `build.cmd`/`build.ps1`
- [ ] 0.3 `ct_core` stub + `ct_test` harness + first test runs under ctest
- [ ] 0.4 Dear ImGui vendored and compiled as `imgui` target (W3, no WX)
- [ ] 0.5 `camtesia.exe`: Win32 window, D3D11 device, flip-model swapchain, ImGui demo renders, resizes, HiDPI correct
- [ ] 0.6 Logging to file + debugger, crash minidump writer, single-instance mutex
- [ ] 0.7 Acceptance: Debug and Release build with 0 warnings; exe idles at 0 redraws/s; crash test produces a `.dmp`

## P01 Core library
- [ ] 1.1 strings (utf8<->utf16, format, split, trim, path ops)
- [ ] 1.2 time (QPC clock, `Now()`, `SleepUntil`, high-resolution waitable timer, `Stopwatch`)
- [ ] 1.3 containers (`SpscQueue<T>`, `ObjectPool<T>`, `RingBuffer`, `Arena`)
- [ ] 1.4 threads (`Thread` wrapper with name + priority + MMCSS class, `Event`, `SrwLock`)
- [ ] 1.5 json (parser + writer, DOM, round-trip tests incl. unicode and 1e308)
- [ ] 1.6 fs (atomic write, read all, enumerate, known folders, temp files)
- [ ] 1.7 config (settings load/save/migrate with defaults)
- [ ] 1.8 trace (TraceLogging provider + scoped spans) and `--stats` counters
- [ ] 1.9 Acceptance: all `ct_core_tests` pass in Debug/Release; SPSC queue benchmark >= 20 M ops/s

## P02 Screen capture
- [ ] 2.1 `D3DDevice` service (device, context lock, debug layer in Debug, feature checks, DXGI device manager)
- [ ] 2.2 Monitor/region model (`DisplayInfo`, virtual screen, DPI, HDR flag)
- [ ] 2.3 `DesktopDuplicator`: acquire loop, crop copy, timeout/repeat, access-lost recovery
- [ ] 2.4 `ColorConvert`: BGRA->NV12 HLSL pass (+ HDR FP16 tone-map variant), Video Processor fallback
- [ ] 2.5 `CursorTracker`: 120 Hz position poll + shape capture (color, mask, animated), dedupe by hash
- [ ] 2.6 Capture pacing: CFR tick emitter, frame repeat, drop accounting, stats
- [ ] 2.7 Acceptance: `capture_bench --monitor 0 --fps 60 --seconds 30` shows 0 drops, CPU < 3 %, GPU < 8 % at 1080p

## P03 Audio capture
- [ ] 3.1 Device enumeration + default device notifications
- [ ] 3.2 `WasapiCapture` (mic, event-driven) -> float 48 kHz stereo with QPC timestamps
- [ ] 3.3 `WasapiLoopback` (system audio, polled) + silence-fill for gaps + silent render keep-alive
- [ ] 3.4 `Resampler` (polyphase windowed-sinc), `Mixer`, `Meters` (peak/RMS, atomics)
- [ ] 3.5 Device hot-swap and format change without timeline gaps
- [ ] 3.6 Acceptance: `audio_bench` records 60 s of a 1 kHz test tone through loopback: no gaps, drift < 5 ms, CPU < 1 %

## P04 Encode & mux
- [ ] 4.1 MF lifetime (`MfRuntime`), encoder enumeration (HW first), capability report
- [ ] 4.2 `Mp4Writer` (sink writer, D3D manager, video stream config, ICodecAPI settings)
- [ ] 4.3 Video texture pool with safe-reuse check; `WriteVideoFrame(texture, t_us)`
- [ ] 4.4 AAC audio streams (mic + system), PCM16 conversion, `WriteAudio(stream, samples, t_us)`
- [ ] 4.5 HEVC option, software fallback path, graceful error (disk full, device lost)
- [ ] 4.6 Acceptance: 10 min 1080p60 recording from `capture_bench` plays in Movies & TV and VLC, 0 dropped, A/V in sync, file size within 10 % of target bitrate

## P05 Recorder app
- [ ] 5.1 `RecordingSession` state machine (Idle->Countdown->Recording<->Paused->Finalizing)
- [ ] 5.2 `.ctrec` bundle writer + `meta.json`
- [ ] 5.3 `EventRecorder`: low-level mouse/keyboard hooks thread, cursor tracker, window focus, `.ctev` writer
- [ ] 5.4 Hotkeys, tray icon + menu, notifications
- [ ] 5.5 Region selection overlay (dim, drag, handles, window snap, monitor pick, size readout)
- [ ] 5.6 Recorder bar UI + countdown overlay, excluded from capture
- [ ] 5.7 Webcam capture -> `cam.mp4` + live preview
- [ ] 5.8 Acceptance: all section-8 recording budgets measured and met; pause/resume produces gapless media; kill during recording loses <= 2 s of events (video loss documented)

## P06 Decode & playback
- [ ] 6.1 `MediaInfo` (streams, durations, codec, size, fps, rotation)
- [ ] 6.2 `VideoDecoder` (source reader, HW NV12 textures, seek, EOS, type change)
- [ ] 6.3 `AudioDecoder` (float 48 kHz stereo, seek, EOS)
- [ ] 6.4 `FrameCache` + prefetch thread per media; NV12->RGB shader
- [ ] 6.5 `AudioEngine` (WASAPI render, master clock from `IAudioClock`, mixing callback)
- [ ] 6.6 `PlaybackClock` (audio-driven, falls back to QPC), rate 0.25-4x, frame step
- [ ] 6.7 Acceptance: `player_demo file.mp4` plays 4K60 H.264 in sync; seek-to-frame < 150 ms; CPU < 10 %

## P07 Project model
- [ ] 7.1 Types (`Project`, `Media`, `Track`, `Clip`, `Keyframe<T>`, `Annotation`, `Transition`, `Camera`, `CursorStyle`)
- [ ] 7.2 JSON (de)serialization with versioning + defaults + unknown-key tolerance
- [ ] 7.3 Edit operations (add/move/trim/split/ripple/delete/speed) with validation + invariants test
- [ ] 7.4 Undo/redo (snapshot stack, coalescing), dirty flag, autosave, crash recovery file
- [ ] 7.5 `.ctrec` import -> project (tracks, cursor track from `.ctev`, keystrokes)
- [ ] 7.6 Auto-Zoom generator from click/typing events; Auto-Follow
- [ ] 7.7 Acceptance: 100 % of model tests pass; 10,000 random edits keep invariants; round-trip JSON byte-identical

## P08 Compositor
- [ ] 8.1 Render target management, canvas/background pass, color space utilities
- [ ] 8.2 Layer pass: textured quad with transform, crop, opacity, rounded corners (SDF), shadow
- [ ] 8.3 Camera (zoom/pan keyframes, easing, auto-zoom playback)
- [ ] 8.4 Cursor renderer from `.ctev` (interpolation + smoothing, shapes atlas, click effects)
- [ ] 8.5 Annotations via Direct2D/DirectWrite into layer textures (text, callout, arrow, shapes, keystroke badge), blur/pixelate region
- [ ] 8.6 Transitions, webcam PIP masks, readback helper for tests
- [ ] 8.7 Acceptance: golden-image tests pass; `RenderFrame` 1080p with 3 layers + cursor + 2 annotations < 4 ms GPU, < 1 ms CPU

## P09 Editor UI
- [ ] 9.1 App shell: docking layout, theme, fonts, icons, DPI, menus, shortcuts table
- [ ] 9.2 Media panel (import, thumbnails via WIC, drag to timeline)
- [ ] 9.3 Preview panel (fit/zoom, safe areas, on-canvas handles for transform/annotation)
- [ ] 9.4 Timeline widget (tracks, clips, waveforms, playhead, ruler, zoom, snap, selection, trim/split/ripple, markers, context menus)
- [ ] 9.5 Properties panel (per-selection editors, keyframe toggles, camera/cursor/background/audio sections)
- [ ] 9.6 Transport (J/K/L, loop, frame step, time display), status bar, toasts, error dialogs
- [ ] 9.7 Acceptance: section-8 editor budgets met; every shortcut in `docs/05` works; no ImGui ID collisions in Debug

## P10 Export
- [ ] 10.1 Export dialog + presets (`assets/presets/*.json`)
- [ ] 10.2 Offline render loop (frame-accurate, audio mix, progress, cancel, background thread)
- [ ] 10.3 MP4 (H.264/HEVC) via `Mp4Writer`, fast-start moov
- [ ] 10.4 GIF encoder (quantization, dithering, LZW, frame diff)
- [ ] 10.5 PNG still, M4A audio-only
- [ ] 10.6 Acceptance: export speed budget met; PNG export identical to the preview; MP4 export PSNR >= 42 dB vs preview at quality 90; GIF 720p20 >= 2x real time

## P11 Performance & polish
- [ ] 11.1 Idle power: 0 redraws, timers coalesced, no busy waits (verify with WPR)
- [ ] 11.2 Startup profile < 500 ms cold; lazy MF init; prewarm encoder on hover
- [ ] 11.3 Memory audit (pools, cache limits, leak check at exit)
- [ ] 11.4 Multi-monitor, mixed DPI, monitor hot-plug, sleep/resume, UAC desktop, fullscreen games (access lost)
- [ ] 11.5 Error UX (disk full, device lost, encoder missing, mic in use) with recovery
- [ ] 11.6 Settings UI, first-run, keyboard-only navigation of main actions
- [ ] 11.7 Acceptance: every section-8 number measured and recorded here

## P12 Installer & release
- [ ] 12.1 Version stamping (`version.h` <- CMake), resource file, icon
- [ ] 12.2 Inno Setup script, portable zip, file association, uninstall
- [ ] 12.3 CI workflow (build, test, package, upload artifacts)
- [ ] 12.4 Release checklist executed on a clean Windows 10 VM and a Windows 11 machine
- [ ] 12.5 Acceptance: fresh install -> record -> edit -> export -> uninstall leaves no files except user data
