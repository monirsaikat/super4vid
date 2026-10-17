# PROGRESS

Single source of truth for task status. Tick a box only in the same commit that completes the task.
Write measured numbers next to acceptance criteria when you tick them (e.g. `- [x] cold start < 500 ms — measured 310 ms`).

Legend: `[ ]` not started · `[~]` in progress (one per agent at a time) · `[x]` done · `[-]` dropped (needs a DECISIONS.md entry)

## P00 Toolchain & skeleton
- [x] 0.1 Toolchain verified (`build.cmd doctor` prints all green) — MSVC 14.44.35207, CMake 3.31.6, Ninja 1.12.1, SDK 10.0.26100.0; Inno Setup MISSING (deferred to P12, see DECISIONS)
- [x] 0.2 Repository scaffold, root CMake, presets, `build.cmd`/`build.ps1`
- [x] 0.3 `ct_core` stub + `ct_test` harness + first test runs under ctest
- [x] 0.4 Dear ImGui vendored and compiled as `imgui` target (W3, no WX) — v1.93.0-docking, 0 warnings
- [x] 0.5 `camtesia.exe`: Win32 window, D3D11 device, flip-model swapchain, ImGui demo renders, resizes, HiDPI correct
- [x] 0.6 Logging to file + debugger, crash minidump writer, single-instance mutex
- [~] 0.7 Acceptance: Debug and Release build with 0 warnings; exe idles at 0 redraws/s; crash test produces a `.dmp`
  - [x] clean `build.cmd debug` / `release`: 0 warnings, 0 errors; `build.cmd test`: ct_core_tests passes in both configs (2026-10-17)
  - [x] idle: `--stats` printed `redraws/s: 0` for 10 consecutive seconds with no input; process CPU time over 12 s idle = 0.0 ms (0.000 %)
  - [x] `--crash-test` -> `%LOCALAPPDATA%\Camtesia\crashes\crash-*.dmp` (~940 KB) in Debug and Release, exit code 0xC0000409; dbghelp resolves the dump's exception address to `main.cpp:285` (the null write) with the matching PDB
  - [x] second instance exits after 44 ms; the first logs `forwarded command line: ... --open "C:\some file.ctproj"`
  - [x] Debug exit: the debug-layer live-object report lists only `Live ID3D11Device` (copied into the log); CRT leak check is clean (a planted leak was reported on stderr, so the check works)
  - [x] time-to-window (process start -> first Present), Release: 114-190 ms; Release exe 837 KB
  - [x] seen in screenshots: dark title bar, app icon, ImGui window (demo in Debug only); programmatic resize and minimize/restore re-render correctly; min client size clamps at 640x400; a synthetic WM_DPICHANGED (144 dpi) rescales fonts and padding to 150 %
  - [ ] HUMAN: at real 150 % display scaling, text is crisp (not bitmap-stretched) and the title bar is dark
  - [ ] HUMAN: drag the window between monitors with different DPI; fonts and padding rescale and the window keeps a sensible size
  - [ ] HUMAN: drag-resize with the mouse (live resize) shows no stretching, flicker or black frames
  - [ ] HUMAN: Task Manager shows 0 % CPU **and 0 % GPU** for Camtesia.exe while idle for 10 s (CPU verified by measurement; GPU not measured)
  - [ ] HUMAN: open a `.dmp` in WinDbg or Visual Studio and confirm it shows the faulting line with call stack (only checked with dbghelp line lookup)
  - [ ] HUMAN: ImGui keyboard navigation and text input (incl. IME) work in the Debug demo window

## P01 Core library
- [x] 1.1 strings (utf8<->utf16, format, split, trim, path ops)
- [x] 1.2 time (QPC clock, `Now()`, `SleepUntil`, high-resolution waitable timer, `Stopwatch`)
- [x] 1.3 containers (`SpscQueue<T>`, `ObjectPool<T>`, `RingBuffer`, `Arena`)
- [x] 1.4 threads (`Thread` wrapper with name + priority + MMCSS class, `Event`, `SrwLock`)
- [x] 1.5 json (parser + writer, DOM, round-trip tests incl. unicode and 1e308)
- [x] 1.6 fs (atomic write, read all, enumerate, known folders, temp files)
- [x] 1.7 config (settings load/save/migrate with defaults)
- [x] 1.8 trace (TraceLogging provider + scoped spans) and `--stats` counters
- [x] 1.9 Acceptance: all `ct_core_tests` pass in Debug/Release; SPSC queue benchmark >= 20 M ops/s
  - [x] `ct_core_tests --repeat 3`: 53 tests x 3 = 159 passed, 0 failed, 0 skipped in Debug and in Release; `build.cmd debug`/`release` 0 warnings (2026-10-17)
  - [x] bench (`--bench`, Release): SPSC push+pop single-thread ping 870-880 M pairs/s (1.15 ns/pair; budget >= 20 M/s); JSON parse of a 1.15 MB document avg 8.4 ms, best 7.8 ms (137 MB/s; budget < 20 ms), Dump 8.9 ms and byte-identical; `Format` 1 M calls 430 ms (430 ns/call)
  - [x] logger caller cost: 100,000 lines from a TIME_CRITICAL thread 25-27 ms in Release (253-273 ns/line; budget < 50 ms), 139-143 ms in Debug; all lines written or counted as dropped (0 dropped)
  - [x] HighResTimer, 1,000 x 2 ms waits (200 us spin): p99 390-640 us over the runs above (500 us target missed in some rounds; the kernel timer fires up to ~1 ms late, see DECISIONS.md); with a 1 ms spin p99 < 170 us

## P02 Screen capture
- [x] 2.1 `D3DDevice` service (device, context lock, debug layer in Debug, feature checks, DXGI device manager) — AMD Radeon (5700G iGPU): NV12 RT yes, FP16 RT yes, video device yes
- [x] 2.2 Monitor/region model (`DisplayInfo`, virtual screen, DPI, HDR flag) — reference machine: 2 x 1920x1080 SDR (HP M22f 60 Hz primary, HP 22fw 59 Hz), 96 dpi, one adapter
- [x] 2.3 `DesktopDuplicator`: acquire loop, crop copy, timeout/repeat, access-lost recovery — first S_OK 22-26 ms after Init (Debug, debug layer), content verified by readback; letterbox path tested
- [x] 2.4 `ColorConvert`: BGRA->NV12 HLSL pass (+ HDR FP16 tone-map variant), Video Processor fallback — GPU vs CPU reference max |dY| 1, |dUV| 1 (all 4 rotations, 10,000 samples); VP fallback max 1/1 on a smooth gradient
- [x] 2.5 `CursorTracker`: 120 Hz position poll + shape capture (color, mask, animated), dedupe by hash — arrow 32x32 hotspot (0,0); CaptureShape ~0.16 ms (Debug), 0 GDI objects leaked over 2,000 captures
- [x] 2.6 Capture pacing: CFR tick emitter, frame repeat, drop accounting, stats — 3 s @ 60 fps 1080p: 180 ticks, 0 drops, 0 late (Debug and Release); pause/resume gapless (max step 16667 us)
- [~] 2.7 Acceptance: `capture_bench --monitor 0 --fps 60 --seconds 30` shows 0 drops, CPU < 3 %, GPU < 8 % at 1080p
  - [x] `build.cmd debug`/`release` 0 warnings; `build.cmd test`: ct_core_tests + ct_capture_tests (26 tests) pass in Debug and Release (2026-10-17)
  - [x] 1080p60, monitor 0 (HP M22f 1920x1080 @ 60 Hz), Release, `--animate` (60 fps animation over 75 % of the monitor as the video stand-in), 3 runs of 30 s: 1801/1801/1801 ticks, 1789/1782/1780 new frames, dropped_pool_empty 0, dropped_sink 0, late_ticks 0; acquire avg 0.085/0.118/0.087 ms (max 0.92); convert avg 0.015-0.019 ms CPU (GPU time of the compute conversion ~1.0 ms at the iGPU's low clocks); process CPU 0.13-0.20 % of the machine (2.0-3.2 % of one core); GPU (Task Manager counter, busiest engine) avg 7.8 / 6.4 / 7.0 %, max 10.2 %; RSS 28-31 MB (peak 50 MB)
  - [x] same with the desktop as it was (browser, 791 new frames / 1009 repeats): 0 drops, 0 late, CPU 0.20 %, GPU 5.4 %, RSS 28 MB
  - [x] Debug + D3D11 debug layer (`--debug-layer`, 5 s animated and a 30 fps region on monitor 1 with `--cursor`): no debug-layer messages, exit report lists only the device; `--dump-frame 100` PNG checked visually (correct colours)
  - [x] budget was missed with the spec's design (20-27 % GPU); fixed with zero-copy conversion, a compute converter and texture re-send for repeats (DECISIONS.md 2.7)
  - [ ] not measurable here: 4K (CPU < 5 %, GPU < 15 %) — the reference machine has only 1080p monitors
  - [ ] HUMAN: lock (Win+L) and unlock during `capture_bench --monitor 0 --fps 60 --seconds 30`: summary shows `access_lost >= 1`, `reinits >= 1`, ticks keep counting, new frames resume within 1 s of unlocking, no crash

## P03 Audio capture
- [x] 3.1 Device enumeration + default device notifications — enumerates 1 render endpoint (Realtek), 0 capture endpoints on this machine; notification delivery on a real default change is part of the 3.5 human check
- [x] 3.2 `WasapiCapture` (mic, event-driven) -> float 48 kHz stereo with QPC timestamps — StreamAligner unit scenarios pass (perfect, pad from 0, 15 ms gap = 720 frames, 3 ms overlap = 144 frames, +/-50 ppm over 10 min converges with <= 1 frame per 10 ms, discontinuity); this machine has no capture endpoint, so `wasapi_capture_default_mic_1s` SKIPs (the shared capture engine is exercised by the loopback tests in 3.3)
- [x] 3.3 `WasapiLoopback` (system audio, polled) + silence-fill for gaps + silent render keep-alive — 2 s loopback of a -12 dBFS tone: inserted 0 after the first packet, delivered frames == last device frame time (+-1 packet), peak -12.00 dB, drift 0.05 ms; keep-alive alone keeps ~100 packets/s flowing; pause shifts the timeline (0 inserted, 74 frames dropped at the boundary)
- [x] 3.4 `Resampler` (polyphase windowed-sinc), `Mixer`, `Meters` (peak/RMS, atomics) — 44.1->48 kHz SNR 91.3 dB (-6 dBFS 1 kHz; 85.2 dB against the unfitted ideal sine, zero group delay), 96->48 kHz alias at 30 kHz -90.4 dB, 18.5 us per 10 ms stereo block (Release); AVX2 kernels bit-exact with scalar for MixAdd/FloatToPcm16/Pcm16ToFloat/channel conversion/PeakAbs on 1 M values, MixAdd 57 ns per 480-frame stereo block; "Mixer" = `dsp::MixAdd`/`ApplyGainRamp` (see DECISIONS)
- [~] 3.5 Device hot-swap and format change without timeline gaps
  - [x] automated: listener (default change / removal / state change), AUDCLNT_E_DEVICE_INVALIDATED and `SwitchDevice` share one reopen path on the capture thread; reopen of the loopback + keep-alive took 22.9 ms and became 960 frames of silence at the right position (stream end == device clock +-1 packet afterwards); a missing endpoint keeps time running with silence, reports `OnAudioError` once, retries every 500 ms and recovers (`hotswap_*` tests)
  - [ ] human: unplug a USB mic mid-recording (expect silence then the new default mic, no shift) and change the default output device while recording system audio (this machine has no mic and one output)
- [x] 3.6 Acceptance: `audio_bench` records 60 s of a 1 kHz test tone through loopback: no gaps, drift < 5 ms, CPU < 1 % — `audio_bench --seconds 60 --tone` (Release, 2026-10-17): gaps 0 (longest quiet run 0.02 ms), frequency 999.9899 Hz, stream end vs last device frame (QPC) -0.993 ms, max |drift| 1.003 ms (device clock ~25 ppm slow: 29 single frames inserted, 0 dropped, 0 ring overflows), process CPU 0.055 % of the machine (16 logical CPUs) = 0.885 % of one core, peak -12.00 dBFS; 2,882,506 frames for a 60.037 s run (the +15.6 ms vs the stop time is the loopback presentation offset, see DECISIONS); rerun after the pause/resume review fix: gaps 0, 999.9899 Hz, -0.981 ms, max |drift| 1.003 ms, 29 inserted, CPU 0.081 % of the machine = 1.30 % of one core (machine busy with other agents' builds; one run with another app playing showed peak -7.34 dBFS / 1000.107 Hz and was discarded)

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
- [x] 7.0 ct_events module (`.ctev` writer/reader, `EventLog` time queries; created here because P05 is not built yet)
- [x] 7.1 Types (`Project`, `Media`, `Track`, `Clip`, `Keyframe<T>`, `Annotation`, `Transition`, `Camera`, `CursorStyle`)
- [x] 7.2 JSON (de)serialization with versioning + defaults + unknown-key tolerance
- [x] 7.3 Edit operations (add/move/trim/split/ripple/delete/speed) with validation + invariants test
- [x] 7.4 Undo/redo (snapshot stack, coalescing), dirty flag, autosave, crash recovery file
- [x] 7.5 `.ctrec` import -> project (tracks, cursor track from `.ctev`, keystrokes)
- [x] 7.6 Auto-Zoom generator from click/typing events; Auto-Follow
- [x] 7.7 Acceptance: 100 % of model tests pass; 10,000 random edits keep invariants; round-trip JSON byte-identical — ct_project_tests 49/49 and ct_events_tests 19/19 pass in Debug and Release with `--repeat 3` (2026-10-17); fuzz 10,000 ops in Release (Validate after each, undo of each applied op restores the JSON, redo checked every 10th) 5.3-6.8 s over 4 runs (budget < 10 s; Debug runs 2,000 ops in ~27 s); full/minimal/sample.ctproj round-trip byte-identical, v0 migrates, docs/04 sample re-serializes stably; ToJson 22-33 us and FromJson 105 us for the 8-clip fixture (Release)

## P08 Compositor
- [x] 8.1 Render target management, canvas/background pass, color space utilities — RenderContext (all pipeline objects in Init, temp RTs pooled per size/format, D2D device on the shared device with BGRA_SUPPORT asserted), solid/gradient/image background, Readback; golden harness implemented in ct_test (WIC, max diff 2 + PSNR 45 dB, --update-golden, .actual/.diff on failure)
- [x] 8.2 Layer pass: textured quad with transform, crop, opacity, rounded corners (SDF), shadow — NV12 (BT.709 limited) or RGBA, rotation/mirror uv, 1 px analytic SDF edge in target pixels, shadow mask at 1/4 res + separable blur (downsample above sigma 8) + composite over the bounds only, cached (0 mask renders for an unchanged frame)
- [x] 8.3 Camera (zoom/pan keyframes, easing, auto-zoom playback) — Camera::At -> CameraTransform applied to layers, cursor and annotations (golden camera_zoom_2x)
- [x] 8.4 Cursor renderer from `.ctev` (interpolation + smoothing, shapes atlas, click effects) — SmoothedPosition (120 Hz resampled Gaussian, click snap), 2048 shelf-packed atlas (2 max) with built-in arrow, ripple/highlight/spotlight/drop shadow, hide-when-idle fade (golden cursor_ripple_t150ms)
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
