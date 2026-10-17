# DECISIONS

Append-only log of every deviation from `MASTER_PLAN.md` / `docs/`. Newest at the bottom.

Format:

```
## YYYY-MM-DD · Pxx task x.y · <short title>
Spec said: ...
Done instead: ...
Why: ... (include the HRESULT / behaviour observed, OS build, GPU)
Impact: ... (which later phases/headers are affected)
```

---

## 2026-10-17 · P00 task 0.1 · vswhere pinned to VS 2022
Spec said: `build.cmd` uses `vswhere -latest -products * -requires ...VC.Tools.x86.x64`.
Done instead: added `-version [17.0,18.0)` so the query only matches VS 2022.
Why: the reference machine also has VS 18 (2026) Community with MSVC 14.51, and `-latest` picked that one. The
non-negotiables require MSVC v143 (VS 2022, 14.44+); a toolset change has to be a deliberate decision, not an accident of what is installed.
Impact: none for code. Moving to a newer toolset later means changing this one range.

## 2026-10-17 · P00 task 0.1 · vswhere directory added to PATH before vcvars64
Spec said: `call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul`.
Done instead: `set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"` runs first.
Why: vcvars64's helper scripts call `vswhere.exe` from PATH and printed "'vswhere.exe' is not recognized" to stderr on every build.
Impact: none.

## 2026-10-17 · P00 task 0.1 · Inno Setup not installed
Spec said: install Inno Setup during 0.1 (`winget install JRSoftware.InnoSetup`).
Done instead: left it uninstalled. `build.cmd doctor` reports `Inno: MISSING`.
Why: the user said not to install it in P00. It is needed only in P12.
Impact: P12 has to install it before task 12.2.

## 2026-10-17 · P00 task 0.1 · .cmd files use CRLF
Spec said: `.editorconfig` uses LF line endings.
Done instead: LF everywhere except `*.cmd`/`*.bat`, which use CRLF (set in both `.editorconfig` and `.gitattributes`).
Why: cmd.exe misparses `goto`/`call :label` in LF-only batch files when a label lands on a 512-byte boundary.
Impact: none.

## 2026-10-17 · P00 task 0.2 · Root CMake gets `src/test` and `src/ui`; empty dirs hold `.gitkeep`
Spec said: the root `CMakeLists.txt` in docs/01 section 3 lists `third_party/imgui`, `src/core`, `src/platform`, `src/app`, `tests`.
Done instead: also `add_subdirectory(src/test)` (the `ct_test` target that `ct_add_test` links) and `src/ui` (an INTERFACE
target `ct_ui` that only exposes `ct_imconfig.h` and links `imgui` in P00). Empty `src/<module>/{include/ct/<module>,src}`
folders contain a `.gitkeep` so git keeps them. `git rev-parse` runs with `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}` and falls back to `"nogit"` (docs/01 section 8).
Why: `ct_add_test` needs a `ct_test` target, and P00 lists `ui` as one of the modules that gets a CMake target.
Impact: P09 replaces `src/ui/CMakeLists.txt` with `ct_add_module(ui ...)`, keeps the target name `ct_ui`, and deletes the `.gitkeep` files it no longer needs.

## 2026-10-17 · P00 task 0.3 · CT_FATAL goes through a handler registered by ct_platform
Spec said: `CT_FATAL(msg)` logs, flushes and calls `ct::CrashHandler::WriteDumpAndExit(msg)`.
Done instead: `<ct/core/assert.h>` declares `ct::SetFatalHandler(FatalHandler)` and `ct::Fatal(msg, file, line)`.
`CT_FATAL` calls `ct::Fatal`, which logs, flushes and calls the registered handler. `ct::platform::CrashHandler::Install()`
registers `WriteDumpAndExit`. With no handler registered (unit tests, tools), it calls `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`.
Why: `ct_core` sits below `ct_platform` in the dependency order and cannot call into it.
Impact: none for callers; `CT_FATAL` behaves as specified once the crash handler is installed.

## 2026-10-17 · P00 task 0.3 · Small additions to the core/test headers
Spec said: the exact signatures in P00 for `log.h`, `assert.h` and `ct_test.h`.
Done instead (additions only, nothing removed or renamed):
- `ct::Log::Level()` getter. `CT_LOG_CATEGORY` is wrapped in `#ifndef` so modules can define it before the include, as the spec intends, without a C4005 redefinition warning.
- Release `CT_ASSERT(cond)` is `((void)sizeof(!(cond)))` instead of `((void)0)`. The condition still is not evaluated, but variables used only in asserts no longer trigger C4189 at `/W4 /WX`.
- Asserts are keyed on `_DEBUG` (set by `/MTd`), not `CT_DEBUG`, so ImGui's `IM_ASSERT -> CT_ASSERT` is active in Debug even though the `imgui` target does not get `CT_DEBUG`.
- The `ct_test` library includes a default `main()` in its own object file (`ct_test_main.cpp`). A test exe gets it only when it defines no `main()` of its own.
- `ct::test::HrFailed()` keeps `CT_CHECK_HR(constant)` free of C4127.
- `core` gained `src/types.cpp` and `src/assert.cpp`. P00 listed only `error.cpp` and `log.cpp`.
Why: required for zero warnings and for the dependency order.
Impact: P01 keeps these when it rewrites the logger internals.

## 2026-10-17 · P00 task 0.4 · ImGui v1.93.0-docking; imconfig and CMake adjustments
Spec said: vendor the newest `*-docking` tag. `ct_imconfig.h` contains `#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS 0`.
The `imgui` target in docs/01 section 5 is used verbatim.
Done instead:
- Vendored `v1.93.0-docking` (commit b916186e, 2026-10-14), the newest docking tag on that date.
- Left `IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS` out. imgui.cpp checks `defined(IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS)`, so defining it as `0` would turn the default IME handler off, the opposite of what the spec wants.
- The `imgui` target also adds `src/core/include` as a PUBLIC include directory and links `$<LINK_ONLY:ct_core>`. `IM_ASSERT -> CT_ASSERT` needs `<ct/core/assert.h>` and `ct::AssertFailed`. `LINK_ONLY` keeps ct_core's `/W4 /WX` usage requirements out of the ImGui compile.
Why: correctness of IME input, and to get a link at all.
Impact: ImGui >= 1.92 has dynamic fonts. There is no atlas rebuild on DPI change: set `style.FontScaleDpi` (P00 does) or `io.ConfigDpiScaleFonts`, and the backend uploads glyphs on demand (`ImGuiBackendFlags_RendererHasTextures`). The docs/05 wording "rebuild the atlas at base_px * dpi / 96" maps to this mechanism.

## 2026-10-17 · P00 task 0.5 · Additions to the Window / D3DDevice / SwapChain interfaces; app details
Spec said: the exact `Window`, `D3DDevice` and `SwapChain` declarations in P00 section 0.5.
Done instead (additions only; every specified member exists with its specified signature):
- `Window::Desc` fields have defaults and two new fields: `quit_on_destroy` (main window only: WM_DESTROY posts WM_QUIT) and
  `min_client_dip` (used by WM_GETMINMAXINFO; the editor passes 640x400). `Window` also has `IsMinimized()` and
  `ConsumeResize()` (the "flag swapchain resize" from WM_SIZE), and a destructor that destroys the HWND. Non-copyable.
- `Window::WndProc` updates its own state (WM_DPICHANGED, WM_SIZE) first, then calls the handler (which calls
  `ImGui_ImplWin32_WndProcHandler` first), then runs the defaults (WM_GETMINMAXINFO, WM_DESTROY).
- `D3DDevice` has `DebugLayerActive()`. `Shutdown()` keeps the `ID3D11Debug`/`ID3D11InfoQueue` references so that
  `ReportLiveObjects()` (called after `Shutdown()`, as main.cpp's exit sequence does) still has a device to ask. It copies the
  debug-layer messages into our log, so live-object reports are visible without a debugger. It is non-const for that reason.
- `SwapChain` has `Shutdown()` (closes the waitable handle) and a destructor.
- `src/app/resource.h` holds `IDI_APP` (101). `app.rc` includes `<winresrc.h>` (from the SDK; `winres.h` ships with ATL/MFC only).
- `io.IniFilename = nullptr`: P00 writes no `imgui.ini` next to the working directory; P09 decides layout persistence.
- `--stats` / `--time-to-window` write to stdout when it is redirected to a file or pipe (scripts, CI). Otherwise they attach to
  the parent console, else `AllocConsole()`.
- `--time-to-window` prints the time from process creation to the first Present and exits. Every run logs the same number.
- The `--stats` 1 s tick is a waitable timer in the `MsgWaitForMultipleObjectsEx` handle list, not `WM_TIMER`. A message
  would request 2 frames and make "redraws/s: 0" impossible.
Why: needed for the behaviours the spec asks for (main-window quit, min size, resize flag, verifiable live-object report).
Impact: P02 extends `D3DDevice`; the added members stay.

## 2026-10-17 · P00 task 0.6 · Crash handler / single instance / paths details
Spec said: `CrashHandler`, `SingleInstance` and `Paths` declared together in `crash_handler.h`. The deliverables list separate headers.
Done instead:
- Three headers: `crash_handler.h`, `single_instance.h`, `paths.h` (the deliverables list wins).
- `CrashHandler` also has `LastDumpPath()`. `Install()` also hooks `SIGABRT` (so `abort()` writes a dump), calls `ct::SetFatalHandler`
  (see the 0.3 entry), and preloads dbghelp.dll. Until `SetDumpDir()` the dump goes to `%TEMP%`. For stack overflows a helper thread writes the dump.
  `WriteDumpAndExit` raises a custom SEH exception (0xE0C70001) inside `__try/__except(filter)`, so the dump has a real context record.
- `SingleInstance::kCopyDataCommandLine = 1` names the `dwData` value. A second instance looks for the `CamtesiaMain` window for up to
  2 s (the first instance may still be starting), calls `AllowSetForegroundWindow` for the owner, and forwards. The first instance logs
  `forwarded command line: ...`, restores itself and comes to the foreground.
- Portable mode layout (not specified): `<exe dir>\data\{logs,crashes,autosave,settings,recordings}`.
- `Paths::Init()` creates every directory, `Videos\Camtesia` included, with `SHCreateDirectoryExW`.
- `src/platform/src/utf_internal.h` has private UTF-8/UTF-16 helpers until P01's `ct::Utf8ToWide`/`WideToUtf8` exist. P01 should switch platform over to them and delete this file.
- `--stats` also prints `messages/s` and the last message ID. That shows what woke the loop when redraws/s is not 0.
Why: the spec leaves these choices open; these are the conservative options.
Impact: P05 registers its emergency callback via `RegisterEmergency`. P01 removes `utf_internal.h`.

## 2026-10-17 · P01 task 1.2 · HighResTimer holds timeBeginPeriod(1); FrameIndexAt is the inverse of FrameTimeUs
Spec said: `HighResTimer` uses `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` and spins the last 200 us; `FrameIndexAt` is "floor".
Done instead:
- `HighResTimer::Init()` also calls `timeBeginPeriod(1)`; the destructor calls `timeEndPeriod(1)`. Added `IsHighResolution()`.
- `FrameIndexAt(t)` returns the largest n with `FrameTimeUs(n) <= t` (the frame shown at t), so `FrameIndexAt(FrameTimeUs(n)) == n`.
  A plain `floor(t * num / (1e6 * den))` returns n-1 for every frame whose start time was rounded down (e.g. 60 fps frame 1 at 16666 us).
- Core headers that need `HANDLE` declare `typedef void* HANDLE;` (identical to winnt.h) instead of including `<windows.h>`;
  ct_core cannot include `<ct/platform/win.h>` (dependency order).
Why: on Win11 26100 with background load, high-resolution timers alone woke up to ~0.9 ms late (p99 430-575 us after the 200 us
spin, over the 500 us budget). With a 1 ms period, p99 is 290-380 us. The period is held only while a timer exists (recording,
playback), so the idle budget (0 redraws, 0 % CPU) is unaffected.
Impact: P02/P03/P06 pacing threads should keep one HighResTimer for the session, not create one per wait.

## 2026-10-17 · P01 task 1.3 · Container additions
Spec said: `spsc_queue.h`, `object_pool.h`, `ring_buffer.h`, `arena.h` with the listed members; no .cpp for them.
Done instead (additions only): `AudioRing::CapacityFrames()/Channels()`, `Arena::Capacity()`, deleted copy operations on all four,
`Arena` destructor (VirtualFree). `AudioRing` and `Arena` bodies live in `src/core/src/containers.cpp` (they need `<windows.h>`).
`SpscQueue::Pop` resets the slot to `T{}` for non-trivially-copyable T, so T must be default-constructible (static_assert).
`ObjectPool::Release` of an index that is not acquired asserts in Debug and is ignored in Release.
Why: keep `<windows.h>` out of public core headers; make misuse loud.
Impact: none for callers of the specified API.

## 2026-10-17 · P01 task 1.4 · Thread/sync/async details
Spec said: the `Thread`, `Event`, `SrwLock`, guard and `Async` declarations in P01 section 1.4.
Done instead (additions only):
- `Thread::MmcssApplied()` (the test needs to see that `AvSetMmThreadCharacteristicsW` returned a handle). Join after a finished
  thread closes the handle, so a `Thread` can be started again. The destructor joins a still-running thread without timeout.
- `SrwLock::TryLockExclusive()`. The guards' constructors are public (the spec's `class` sketch left them private).
  `Event`/`SrwLock` bodies live in `thread.cpp` (no `sync.cpp` in the deliverables).
- `Async`: `Run()` before `Init()` starts 2 workers; `Shutdown()` drains the queue, then joins; `Run()` after `Shutdown()` runs
  inline; `PostToUi()` without a poster runs inline. Workers are named `ct-io-pool-0`, `ct-io-pool-1`, ...
Why: testability and no lost jobs at exit.
Impact: the app (P05/P09) defines `WM_CT_INVOKE`, installs the poster after creating its main window, and in the handler does
`auto* fn = (std::function<void()>*)lparam; (*fn)(); delete fn;`. Call `Async::Shutdown()` before `Log::Shutdown()`.

## 2026-10-17 · P01 task 1.5 · Json additions and parser choices
Spec said: the `Json` class in P01 section 1.5.
Done instead (additions only): `Json(std::nullptr_t)`, a constructor template for the other integer types (`uint32_t`, `size_t`,
`long`, ...; without it they are ambiguous between int/int64_t/double/bool), `IsInteger()`, `operator==`/`!=` (deep, 1 == 1.0),
and a non-const `operator[](size_t)` (without it `j[0]` on a non-const value is ambiguous with the string_view overload).
Parser choices the spec leaves open: nesting depth 256 is accepted, 257 rejected; unpaired `\uD800`-style surrogates become U+FFFD
(not an error); `-0` is stored as a double so its sign survives; integers beyond int64 become doubles; duplicate keys are kept in
order and lookups return the first; raw bytes >= 0x80 pass through unvalidated. `Get<T>` supports bool, integers (clamped),
floating point and `std::string`.
Why: the spec's overload set does not compile for common call sites; the rest are the conservative choices.
Impact: P07 can rely on byte-identical Dump after Parse -> Dump -> Parse.

## 2026-10-17 · P01 task 1.2 (follow-up) · 500 us wake target is reported, 1 ms is enforced; HighResTimer::SetSpinUs
Spec said: `HighResTimer::WaitUntilQpc` wakes within 500 us for 1,000 waits of 2 ms (report p99), spinning the last 200 us.
Done instead: the test takes the best of up to 3 rounds, prints p50/p99/max, warns when p99 >= 500 us and fails only at >= 1 ms.
Added `HighResTimer::SetSpinUs()/SpinUs()` (default 200 us); with a 1 ms spin the test asserts p99 < 500 us (measured 0.5-165 us).
Why: on the reference machine with other work running, the high-resolution kernel timer itself fires up to ~0.9 ms after its due
time (p99 of the 200 us-spin wait: 390-620 us over 15 runs in Debug and Release; running under MMCSS "Pro Audio"/"Capture" or at
TIME_CRITICAL made no difference). The 200 us spin cannot hide that, and the spec's target made `build.cmd test` flaky. A 1 ms
default spin would cost ~6 % of a core at 60 Hz, which the capture CPU budget cannot afford.
Impact: pacing threads that need sub-500 us wakes (none so far: capture timestamps are tick-aligned, audio is event-driven) call
`SetSpinUs(1000)` and accept the CPU cost.

## 2026-10-17 · P01 task 1.6 · fs choices; known folders stay in ct::platform::Paths
Spec said: `ct::fs` signatures in P01 section 1.6; PROGRESS.md 1.6 also mentions "known folders".
Done instead: no known-folder API in `ct::fs`; `ct::platform::Paths` (P00) already resolves and creates them. `Delete` returns true
when nothing is left at the path (missing counts as success); `DeleteTree` removes read-only files and does not follow junctions.
`Relative` returns `Absolute(path)` across drives/shares and "." for equal paths; comparisons are ordinal case-insensitive.
`TempPath(ext)` accepts the extension with or without the dot. `SameVolume` compares volume GUIDs (falls back to mount paths).
`ShowInExplorer`/`OpenWithShell` initialize STA COM on the calling thread when it has no apartment.
Why: the spec leaves these open; these are the conservative choices.
Impact: none.

## 2026-10-17 · P01 task 1.7 · Settings layout
Spec said: `struct Settings` mirroring docs/04 section 4 with typed fields, `m_extra` for unknown keys, `Version()`, `Migrate()`.
Done instead:
- The "export" section is the member `export_` (`export` is a C++ keyword). Nullable strings (`mic_device`, `webcam_device`,
  `output_dir`, `last_dir`) are `std::string`, empty = JSON null. Hotkeys are stored as `Hotkey` values (strings only in the file).
- `m_extra` and the file version are private; `Extra()` and `Version()` read them. Unknown keys are kept at every nesting level
  (e.g. `recording.new_knob`, `window.editor.monitor`) and written after the known keys of their object.
- `FromJson` validates: clamps numbers (fps 1-240, quality 1-100, countdown 0-10, cursor_hz 30-1000, autosave 0-3600,
  frame cache 32-16384 MB, editor window >= 640x400) and replaces unknown enum strings / wrong types with defaults.
  `keystrokes` accepts "shortcuts" | "all" | "off". The docs sample's `"preview_quality": "full|half"` lists the options; default "full".
- A file without "version" is version 0 and migrates to 1; a file with a newer version keeps it (and its unknown keys) on save.
- `Load` of a damaged file resets to defaults and returns false with a user-presentable `Err`; `Save` creates the parent directory.
- `Hotkey` adds `IsSet()` and `operator==`; "None" parses to {0, 0}. Canonical text order is Ctrl+Alt+Shift+Win+Key.
Why: the spec leaves these choices open.
Impact: P05 (hotkeys, recording options) and P11 (settings UI) use these field names.

## 2026-10-17 · P01 task 1.8 · Logger, stats and trace details
Spec said: trace.h/stats.h as sketched; log producers memcpy into a 1 MB SRW-protected ring, a `ct-log` thread flushes; log.h unchanged.
Done instead:
- log.h: signatures unchanged; only comments updated. A full ring drops the *new* line (consumers own the read cursor) and counts it
  in `Stats::C("log.dropped")` (published by the ct-log thread). Lines before `Init()` stay in the ring (up to 1 MB). `Flush()` drains
  synchronously with a bounded 200 ms wait (crash handler). Producers also wake the writer at half-full. Rotation happens after a
  drain crosses 10 MB, so a file can exceed 10 MB by at most one drain (< 1 MB). Debugger output moved to the ct-log thread.
- The 100,000-line caller-cost test asserts < 50 ms in Release (measured 25 ms, 253 ns/line) and < 400 ms in Debug (measured 143 ms;
  /Od, /RTC1 and the debug CRT's vsnprintf dominate).
- stats.h additions: `Counter::Get`, `Gauge::Get`, `Timing::Count`, `Stats::SetScopeTimings/ScopeTimings` (CT_TRACE_SCOPE feeds
  `Stats::T(name)` only while enabled, off by default; the app turns it on for --stats/F11). Timing members are atomics.
  Dump prints entries sorted by name.
- trace.h additions: `Trace::Enabled()`, `TraceScope` (non-copyable), `CT_CONCAT`. Provider GUID {9b3e281e-aa6b-4781-a0b4-817998fa4f27}.
  Events: "Event" (Name, Value), "Scope" start/stop opcodes (Name, DurationUs on stop). Scope names must be string literals.
- Tests for log/stats/trace live in `tests/core/log_test.cpp` (not in the deliverables list).
Why: keep the caller's cost to a memcpy and make every line accounted for.
Impact: the app should call `Trace::Register()` at startup, `Stats::SetScopeTimings(true)` with --stats, and keep calling
`Log::Shutdown()` last. `Log::Tick()` may be removed from the UI loop later; it is a no-op.

## 2026-10-17 · P03 task 3.4 · Resampler/dsp/wav details; "Mixer" is the dsp kernels
Spec said: `Resampler` with `Process/Flush/Ratio/Latency`; `dsp` functions with scalar + AVX2 paths; PROGRESS 3.4 names a `Mixer`.
Done instead:
- `Resampler` places output frame j exactly at input position j * in / out (centred filter, 15 frames of silent history),
  so it has zero group delay; `Latency()` is the 16-frame look-ahead in *input* frames, not a timestamp offset. `Ratio()`
  is out/in. Added `Reset()`, `InRate()`, `OutRate()`. `Flush` must be called until it returns less than the capacity.
  Each phase row is normalized to unity DC gain. Init accepts ratios within [1/8, 8].
- `dsp` adds `UsingAvx2()`, `Kernels`, `ScalarKernels()`, `Avx2Kernels()` (tests compare both sets directly) and
  `struct GateState { envelope, gain }`. `FloatToPcm16` scales by 32768 and saturates (so `Pcm16ToFloat` = x/32768 is its
  exact inverse) instead of 32767. AVX2 paths use mul + add (never FMA) so MixAdd/FloatToPcm16 are bit-exact with
  scalar. `NoiseGate` is a per-frame recursion; its "AVX2" entry is the scalar kernel. `DownmixToStereo` for > 2 channels
  is a strided copy in both sets.
- There is no separate `Mixer` class in P03: mixing is `dsp::MixAdd` + `ApplyGainRamp`. The timeline mixer (clips, gain
  keyframes, fades) is P06's `AudioEngine` / P10's export mix, which build on these kernels.
- `WavReader` adds `Seek()`; `WavWriter` adds `FramesWritten()` and closes in its destructor.
Why: testability and exact int16 round trips; a mixer object has nothing to own until there is a timeline.
Impact: P04 converts with `dsp::FloatToPcm16`; P06/P10 implement mixing on top of `dsp`.
## 2026-10-17 · P03 tasks 3.1-3.5 · Audio API additions
Spec said: the `ct::audio` interfaces in docs/phases/P03 (AudioDevices, WasapiCapture/Loopback/Render, AudioPacket/AudioSink, CaptureConfig).
Done instead (additions only; every specified member exists with its specified signature):
- `audio_sink.h` holds `AudioPacket`, `AudioSink`, `CaptureConfig`, plus `kSampleRate = 48000`, `kChannels = 2` and
  `CaptureStats` (device packets, delivered packets/frames, leading pad, inserted/dropped frames, discontinuities, reinits,
  drift, media time of the last device frame, last reopen time, device open).
- `WasapiCapture`/`WasapiLoopback`: `GetStats()`, `SetPaused(bool, int64_t at_qpc = 0)` (the session passes the same pause/resume
  QPC to every stream; the pause duration is subtracted from media time), `SwitchDevice(id)` (move a running stream; empty = default).
  `Stop()` delivers the last partially filled packet; `Start()` may be called again after `Stop()`. `Init` must run on an MTA thread.
  `OnAudio` returning false counts the packet as dropped (`audio.mic.dropped` / `audio.sys.dropped`).
- `AudioDevices`: `NameOf(id)`, `AddListener/RemoveListener` (several streams plus the app's `SetListener` slot listen at once);
  `OnDeviceStateChanged` to a non-ACTIVE state is forwarded as `OnDeviceRemoved` (unplugging fires only that; `OnDeviceRemoved`
  itself fires on uninstall). Capture threads open endpoints with their own enumerator (no cross-apartment use).
- `WasapiRender`: `Init(id, buffer_ms, thread_name = "ct-audio-out")`, `LastError()`, `DeviceName()`, `IsRunning()`, virtual
  destructor on `Callback`. `GetClock` returns QPC *ticks* (converted from IAudioClock's 100 ns value). `device_position_frames`
  = frames written before this buffer. On a device error the render thread stops calling back and sets `LastError()`.
Why: P04/P05 need counters, a shared pause QPC and device switching from the UI; the rest is needed for correctness.
Impact: P04 consumes `AudioPacket` (always 48 kHz stereo, contiguous `t_us`); P05 uses `SetPaused(.., qpc)`, `SwitchDevice`,
`GetStats`, `Levels()`; P06 uses `WasapiRender` + `GetClock`.

## 2026-10-17 · P03 tasks 3.2/3.3/3.5 · StreamAligner thresholds, starvation and thread loops
Spec said: the aligner inserts silence for gaps > 1 ms and trims single frames every 10 ms while |drift| > 1 ms; the mic loop
waits 2000 ms; the "+50 ppm over 10 min" test expects "inserted ~ 30 frames".
Done instead:
- Exact resync (silence for a gap, dropped frames for an overlap) when |d| > 2 ms, or for the first packet, a
  DATA_DISCONTINUITY packet, the first packet after a device switch / resume / starvation fill (there at >= 1 frame).
  1 ms < |d| <= 2 ms is drift: one frame repeated or dropped per 10 ms of output. With a 1 ms threshold for both, slow drift
  reaching 1 ms would be "filled" with a 48-frame jump, which the drift test forbids.
- 50 ppm over 10 min is 30 ms = 1440 frames, so the test asserts 1392 +- 60 inserted (the aligner holds |drift| at ~1 ms); the
  spec's "~30 frames" matches what the 60 s bench measured (29 frames at ~25 ppm). `TotalInserted()` excludes the leading pad
  (`LeadingPadFrames()`). Added `FillSilenceUntil`, `FramesEmitted`, `Started` and a `start_us` argument to `Reset`.
- Starvation (failure matrix "no packets"): no device packet for 100 ms -> silence up to now - 50 ms, repeated while starving;
  a reopen that fails retries every 500 ms and reports `OnAudioError` once. The mic thread therefore waits with a 20 ms timeout.
- The loopback poll is a `HighResTimer` with `SetSpinUs(0)` armed every 5 ms (a 200 us spin would cost ~4 % of a core). The
  keep-alive render stream runs on its own thread "ct-audio-sys-keepalive" with a 50 ms buffer.
- Loopback QPC positions are presentation times: the last packet's timestamp is ~10-20 ms *after* the moment it is read. The
  device tests and audio_bench compare the delivered frames with the media time of the last device frame (|delta| < 1 packet;
  bench: -0.993 ms) and report the offset to the stop time separately (+15.6 ms in the bench).
Why: smooth drift correction without audible jumps, and a stream that keeps time when a device is gone.
Impact: P04/P05 must not assume the audio stream ends at "now"; it can run ~20 ms ahead (loopback) or behind (mic).

## 2026-10-17 · P03 task 3.2/3.6 · Paths not exercised on this machine
Spec said: device test `WasapiCapture` default mic 1 s; fallback to the mix format + `Resampler` when the 48 kHz float request
is rejected; manual unplug / default-device-change checks.
Done instead: this machine has no capture endpoint (`wasapi_capture_default_mic_1s` SKIPs; the mic shares `CaptureEngine` with
the loopback, which is tested) and its Realtek endpoint accepts float 48 kHz, so the mix-format/resampler fallback runs only in
unit tests of its parts. The unplug and default-change checks are listed for a human under 3.5 in PROGRESS.md.
Why: hardware availability.
Impact: run `ct_audio_tests --verbose` and `audio_bench --seconds 60 --tone --mic` on a machine with a USB mic before P05 sign-off.
## 2026-10-17 · P03 review fix · Pause decided by capture time (PauseTimeline)
Spec said: `SetPaused(bool)`: packets are consumed but not delivered while paused; media time = qpc - epoch - pause offset.
Done instead: `SetPaused(paused, at_qpc)` records pause intervals in `ct::audio::PauseTimeline` (new public header,
`pause_timeline.h`). The capture thread maps every device frame by its *own capture time*: frames inside [begin, end) are
dropped, the others get media = capture - (pauses that ended before them). The old code read a pause flag and an offset as
two separate atomics when the packet was read, so a packet captured before Pause() but read after it was lost (10-20 ms per
pause), and a resume racing a packet could see the old offset with paused == false (late by the whole pause, then ~P of audio
dropped as overlap). The resampler fallback now loops until a run's input is fully consumed.
Remaining effect: a frame whose capture time lies after the recorded resume time but which is read before the Resume() call
reaches the engine is dropped (the pause still looks open) and becomes silence; nothing shifts. With at_qpc = now this only
affects loopback, whose timestamps are presentation times ~10-20 ms ahead of "now".
Why: review finding (high): pause/resume must not lose or shift audio.
Impact: P05 should call `SetPaused` on every stream promptly with the same QPC; nothing else changes for callers.
## 2026-10-17 · P02 task 2.1 · D3DDevice/TexturePool/GpuTimer details
Spec said: `TexturePool` hands out indices through an SPSC queue; `SupportsNv12RenderTarget()` checks
`CheckFormatSupport(DXGI_FORMAT_NV12)`; `GpuTimer` is double-buffered.
Done instead (additions only, specified signatures unchanged):
- `TexturePool` keeps one atomic in-use flag per texture instead of an SPSC queue of free indices. Acquire scans round robin
  from the slot after the last one handed out, so the longest-released texture comes first, and skips any slot the reuse
  predicate refuses. With a queue, a refused index would have to be popped and pushed back by the producer, which an SPSC
  queue does not allow. Same thread contract: Acquire on one thread, Release on one other. Added `Shutdown()` and the
  `ReusePredicate` typedef; `Init` accepts 1..64 textures.
- `SupportsNv12RenderTarget()` also requires R8_UNORM and R8G8_UNORM render-target support (the per-plane RTV formats).
- `GpuTimer` rotates 4 query sets (not 2), so a span is not skipped when the GPU runs 2-3 frames behind; added `Shutdown()`.
- `InitMfManager()` is guarded by its own critical section (ct_media may call it from a worker thread).
- D3DDevice/TexturePool/GpuTimer tests live in `tests/capture/d3d_test.cpp` (there is no platform test exe). `ct_capture_tests`
  has its own `main()` that sets per-monitor-v2 DPI awareness before running the tests.
Why: the reuse predicate and the SPSC contract cannot both hold with a queue; the rest is robustness.
Impact: P04 uses the specified API unchanged.

## 2026-10-17 · P02 task 2.2 · Display order and region clamp rules
Spec said: `EnumerateDisplays` returns the displays with an `index`; `ClampRegionToDisplay` forces even sizes, min 64x64.
Done instead: the order is primary first, then left to right, top to bottom, so `--monitor 0` is always the primary monitor.
`friendly_name` falls back to the GDI device name when the EDID name is missing. `ClampRegionToDisplay` treats an empty region,
or one that does not intersect the display, as the whole display, and shifts a region grown to 64x64 back inside the display.
Added `RotationName()` for logs.
Why: the spec leaves these open; P05's monitor picker and the bench need a stable "primary = 0".
Impact: none.

## 2026-10-17 · P02 task 2.3 · DesktopDuplicator details
Spec said: the `DesktopDuplicator` declaration and Init/Acquire sequences in P02 section 2.3.
Done instead (specified members unchanged):
- The output is found by GDI device name (`DXGI_OUTPUT_DESC.DeviceName`) across all adapters rather than by
  `adapter_luid/output_index` alone: LUID + index can go stale after a hot-plug, the name is what `DisplayInfo` was built from.
- `DuplicateOutput` (legacy) is also the fallback when `DuplicateOutput1` returns `DXGI_ERROR_UNSUPPORTED`/`E_INVALIDARG`
  (process not per-monitor DPI aware), not only `E_NOTIMPL`/no interface.
- `Reinit()` asks only for the format the session started with, and returns `DXGI_ERROR_UNSUPPORTED` (retry) if the display
  now delivers another one (HDR toggled), so the BGRA/FP16 pool and converter never see a format change. A rotation change
  during a session is not followed (the converter keeps the start rotation); documented limitation.
- Mode change to a smaller size: `ComputeBox` intersects the region with the new desktop; the visible part keeps its place in
  dst and the rest is cleared to black via a cached RTV (dst textures must be bound as render targets). The stream size never
  changes (docs/03 section 10 "letterbox"). The duplicated texture size is re-checked on every frame so the box never leaves it.
- `capture.coalesced` counts the extra frames (`AccumulatedFrames - 1`), not the number of acquires with more than one.
- The cross-adapter slow path is implemented (second device, staging copy, `UpdateSubresource`) but not exercised: the reference
  machine has one GPU.
Why: robustness against hot-plug, DPI-unaware hosts and mode changes.
Impact: P05's pool textures for capture need `D3D11_BIND_RENDER_TARGET` (ScreenCapturer creates them that way).

## 2026-10-17 · P02 task 2.4 · ColorConverter details; ct_compile_shaders fix
Spec said: `bgra_to_nv12.hlsl` with a runtime `tonemap` flag and `float4x4 rot_uv`; `rgba16f_to_nv12.hlsl` as a separate file;
the `ColorConverter` declaration in section 2.4; the video processor enumerator with "Usage NORMAL".
Done instead:
- `rot_uv` is declared `row_major` so the C++ side (`RotationUvMatrix`, exported for tests) writes it in reading order.
- `rgba16f_to_nv12.hlsl` defines `CT_HDR_SOURCE` and includes `bgra_to_nv12.hlsl`: the scRGB scale + sRGB encode is compiled
  only into the FP16 variant; `tonemap` stays a runtime flag inside it. BT.709 constants are unchanged. An FP16 source with
  `hdr_tonemap = false` is still scaled to SDR white and sRGB-encoded (only the roll-off is skipped).
- Note: the specified roll-off `c / (1 + max(c - 1, 0))` equals 1 for every c >= 1, so it behaves as a clip at SDR white. Kept as
  specified; a real shoulder curve is a P11 polish item if HDR captures look flat.
- `Config` gained `force_video_processor` (tests run the fallback on hardware that has the shader path) and `Shutdown()`.
  Exported CPU references `RotationUvMatrix`, `RotateUvRef`, `RgbToNv12Ref` live in `color_convert.h`.
- Views (SRV, per-plane RTVs, VP input/output views) are created on first use per texture and cached (16 slots each); with
  the 3 + 12 pool textures this means no view creation after the first second of a session.
- Video processor fallback: `D3D11_VIDEO_USAGE_PLAYBACK_NORMAL`, auto-processing off, source/dest/target rects set. Rotation maps
  R90 -> `D3D11_VIDEO_PROCESSOR_ROTATION_90` (clockwise, the same mapping as the shader's uv transform); not verified on a
  rotated monitor.
- `cmake/CtModule.cmake` `ct_compile_shaders`: the `$<IF:$<CONFIG:Debug>,/Zi;/Od,/O3>` argument is now quoted. Unquoted, the
  `;` split the generator expression and ninja rejected the build file ("bad $-escape"); P02 is the first user of the helper.
Measured: GPU vs CPU reference, 10,000 samples per rotation, max |dY| = 1 and max |dUV| = 1 for all four rotations; the video
processor fallback gives max |dY| 1 / |dUV| 1 on a smooth gradient (27 on hard sawtooth edges, its chroma filter differs).
Impact: P08 can reuse `fullscreen.hlsl` and the helper.

## 2026-10-17 · P02 task 2.5 · CursorTracker details
Spec said: the `CursorTracker`/`CursorSample`/`CursorShape` declarations and the Poll/CaptureShape algorithm in section 2.5.
Done instead (additions only):
- `CursorTracker()` constructor (reserves the known-id list), `Polls()` and `DroppedSamples()` counters.
- An inverting pixel is detected when *any* channel is brighter over black than over white (the spec checks "black > white").
  Premultiplied colour is clamped to alpha so the bitmap is always valid premultiplied BGRA.
- A cursor whose shape cannot be captured gets `shape_id = 0` and is not retried until the handle changes (no 120 Hz GDI
  retry storm); a full shape queue is retried on the next poll. A full sample queue drops nothing silently: it is counted and
  the change is pushed on the next poll.
- `GetCursorInfo` failing (secure desktop) keeps the last sample.
- Shape ids hash pixels, size and hotspot (FNV-1a over the pixel hash plus a small header); 0 is reserved.
Measured: `CaptureShape` ~0.16 ms per call (2,000 captures in 314 ms, Debug), 0 GDI objects leaked.
Why: robustness; the spec leaves these open.
Impact: P05's EventRecorder writes `shape_id 0` as "default arrow".

## 2026-10-17 · P02 task 2.6 · ScreenCapturer / FrameSink details
Spec said: the `FrameSink`, `CaptureStats` and `ScreenCapturer` declarations and the pacer loop in section 2.6.
Done instead (additions only; specified members unchanged):
- `FrameSink::OnError(HRESULT)` (the loop sketch says "add to FrameSink") is virtual with an empty default, called once on the
  capture thread before it exits on a fatal error. `OnFrame` returning true transfers `pool_index` to the sink, which releases it.
- `CaptureStats` gained `gpu` (GpuTimer around the conversion passes), `first_frame_us` and `last_new_frame_qpc` (the bench and
  P05 use it to see "capture resumed"). `acquire` is timed only for calls that returned S_OK and includes the wait inside
  `AcquireNextFrame` (at most until tick + 2 ms); `repeats` counts ticks that re-sent the previous image (also while access is lost).
- `Config` gained `force_video_processor`; `Init` clamps `region` with `ClampRegionToDisplay` (an empty region = whole display).
  `Region()` and `IsRunning()` getters added.
- The HighResTimer is created inside the capture thread (it holds `timeBeginPeriod(1)` only while capturing).
- Reinit is retried every 250 ms on any failure except device loss (`DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG`,
  `DRIVER_INTERNAL_ERROR`), which is fatal. `reinits` counts successful re-creations. The first retry happens on the next tick.
- Pausing while the pacer waits for a tick discards that tick (no frame, no tick count).
- The BGRA pool rotates through its 3 textures: each acquire writes the next slot, so a failed or timed-out acquire leaves the
  last image intact for repeats.
Why: needed by the bench, tests and P05; the rest are the conservative readings of the sketch.
Impact: P04's Mp4Writer implements `OnFrame` (and may override `OnError`).

## 2026-10-17 · P02 task 2.7 · GPU budget work: zero-copy conversion, compute converter, repeats re-send the texture
Spec said: crop-copy every new desktop image into a BGRA pool texture, convert it with two pixel-shader passes, and on a
tick without a new image convert the previous BGRA texture again into a new NV12 texture. Budget: GPU < 8 % at 1080p60.
Observed with that design (Release, 1080p60, a 60 fps animation on the monitor, Task Manager's per-process "GPU Engine"
counter): 20-27 % GPU. GPU timestamps, paced at 60 Hz: crop copy 0.85-0.9 ms, two-pass conversion 1.4 ms, video processor
1.4-1.8 ms. The iGPU stays at low clocks at this load (the same conversion takes 0.27 ms back to back), so every pass is
expensive. The 8 % budget cannot be met that way.
Done instead (DECISIONS-worthy API additions marked +):
- Zero-copy: `+DesktopDuplicator::AcquireDirect/ReleaseDirect/CanAcquireDirect` hand the duplicated surface and the crop box
  to the capturer, which converts straight from it with `+ColorConverter::ConvertRegion` (crop as uv offset/scale in the
  constant buffer; the old `inv_src_size`/`pad` fields became `src_offset`/`src_scale`, `pad2` became `dst_size`) before
  `ReleaseFrame`. The BGRA crop copy (`AcquireInto`) remains for letterboxing, cross-adapter capture, the video processor
  fallback and `+ScreenCapturer::Config::zero_copy = false`; the BGRA pool (3) is still created.
- Compute converter: one dispatch, one thread per 2x2 block, R8/R8G8 UAVs on the NV12 planes (`D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE`
  on NV12, D3D11.3 `CreateUnorderedAccessView1` with `PlaneSlice`). 0.54-0.8 ms paced vs 1.4-1.8 ms for the two passes,
  bit-exact to the CPU reference (max |dY| 1, |dUV| 0). Used when supported (`+ColorConverter::Config::allow_compute`,
  `+UsingCompute()`, `+DstBindFlags()`); the pixel-shader passes stay as the fallback. Shaders split into `nv12_common.hlsli`,
  `bgra_to_nv12.hlsl` (PS), `bgra_to_nv12_cs.hlsl` (CS) and the two HDR wrappers.
- Repeats: a tick without a new image re-sends the previous NV12 texture (docs/03 section 4 wording) instead of converting
  again: `+TexturePool::AddRef(index)` hands the same texture out again, the pool keeps a hand-out count per texture, and the
  sink releases once per `OnFrame`. Repeats cost no GPU work and no pool texture.
- `CaptureStats::acquire` measures from max(call time, the image's present time) to return, i.e. the cost of acquiring and not
  the wait inside `AcquireNextFrame` for the desktop to change (that wait is a kernel wait of up to 3 ms, no CPU). With the
  spec's definition (whole call), avg was 1.1-1.7 ms on a changing desktop.
- `+platform::ImageIo::WritePngRgba` (WIC) for `--dump-frame`; P06 extends it.
- `capture_bench` extras: `--animate` (child process shows a 60 fps animation over 75 % of the monitor as the "video playing"
  stand-in), `--cursor` (CursorTracker at 120 Hz on the main thread), `--video-processor`, `--copy-path`, `--pixel-shader`,
  `--debug-layer`. GPU % was read with `Get-Counter "\GPU Engine(pid_<pid>_*)\Utilization Percentage"` (busiest engine,
  what Task Manager shows) once per second.
Result (3 x 30 s, animated): GPU 6.4 / 7.0 / 7.8 % avg, 0 drops, 0 late ticks. See PROGRESS.md 2.7.
Not measured: 4K (the reference machine has two 1080p monitors); lock/unlock recovery (needs a person at the machine).
Seen once during development (older build, user working on the machine): 3 late ticks in 30 s; not reproduced in the final runs.
Impact: P04 — the NV12 pool textures carry `D3D11_BIND_UNORDERED_ACCESS` when the compute path is active (check the MF
encoder accepts them; if not, set `ScreenCapturer::Config::allow_compute = false`), and the same `pool_index` can arrive in
consecutive `OnFrame` calls (one Release per call). The reuse predicate is consulted only by `Acquire`, never by `AddRef`.

## 2026-10-17 · P06 task 6.1 · Private MFStartup guard; MediaInfo additions; AAC-padded duration; local test patterns
Spec said: `MFStartup` once per process in `ct::media::MfRuntime` (P04); `MediaInfo` as declared in P06; the pattern
`MediaInfo` test expects duration 5 s +- 1 ms; test media comes from P04's `tools/gen_test_media`.
Done instead:
- P04 and P06 were built in parallel, so `src/media/src/mf_startup_internal.h` holds `detail::EnsureMfStarted()` (a
  function-local static `MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)`, never shut down). **After the merge replace every
  call with `media::MfRuntime::Ensure()` and delete the file.** `src/media/CMakeLists.txt` was created by P06 with
  `DEPS ct_platform ct_audio` (P04 adds its sources/deps).
- `MediaInfo` additions: `IsImagePath()`, `Find(stream_index)`. Images get one `Video` stream (codec "PNG"/"JPEG"/...,
  size, fps 0) so `FirstVideo()` works for them; `seekable = false` for images.
- `MF_PD_DURATION` is the longest track: Media Foundation's AAC encoder pads the audio track to whole 1024-frame packets
  (240000 -> 240640 frames), so a 5.000 s pattern reports 5.0133 s. The test accepts 5 s -1 ms / +22.3 ms.
- Until P04's files exist, the test-only helper `ct_gen_test_media` (tests/media/gen_pattern_main.cpp, not shipped)
  writes the same patterns with a CPU-drawn NV12 image into an `IMFSinkWriter` to `out/testmedia/` (git-ignored).
  Tests look in `tests/data/` first, then `out/testmedia/`, and CT_SKIP when neither has the file.
- The 7-segment counter layout is fixed in `tests/common/frame_index.h` (P04's spec only gives the box (32,32)-(432,96)):
  box filled black (Y 16) and drawn *after* the flash; 12 digits, most significant first, cell x = 34 + 33 k, glyph
  24 x 48 px at (cell + 4, 40), segments 6 px white (Y 235). The header also has `DrawFrameIndexY()`, which P04's
  generator should call so both sides share one layout. Beep: 2 kHz at -12 dBFS *replacing* the 1 kHz tone for the
  first 20 ms of every second (flash frames are 0, 60, 120, ...).
Why: parallel phases; MF behaviour; the generator spec leaves the digit geometry open.
Impact: P04 merge: swap the MFStartup guard, keep the counter layout (or update frame_index.h to match its generator).

## 2026-10-17 · P06 task 6.2 · VideoDecoder: no advanced video processing, low latency on hardware, seek budget missed
Spec said: reader attributes include `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING = TRUE`; seek to an exact frame
< 150 ms (p95 over 20 seeks); the interface in P06 section "Interfaces".
Done instead:
- Hardware path: D3D manager + `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` + `MF_LOW_LATENCY = TRUE`, *without* advanced
  video processing. The H.264/HEVC decoders output NV12 already; the extra video processor stage cost ~3x the CPU per frame
  (156 ms vs 47 ms CPU for a 119-frame seek) and saved no wall time. Low latency makes a seek to a keyframe 2-4 ms instead
  of ~8 ms. Software path (`allow_hardware = false`): no D3D manager, DXVA disabled, `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING`
  (software conversion to NV12), and *no* `MF_LOW_LATENCY`: with it the software decoder restarts output timestamps at 0
  after a seek (Win11 26100).
- Additions: `ReadFrame(out, skip_before_us)` decodes and drops frames ending at/before `skip_before_us` without the copy
  (fast-forward after a seek); `ReadFrame` returns `E_PENDING` (nothing consumed) when every pool texture is held; `YSrv/UvSrv`
  (R8 / R8G8 views per pool slot, created once), `FrameDurationUs`, `AtEnd`, `FramesCopied/Skipped`, `Shutdown`, and the pure
  `ComputeApertureBox` (aperture clamped to the frame, origin rounded *up* and size *down* to even). The software path
  rebuilds the aperture as contiguous NV12 in a scratch buffer before `UpdateSubresource` (the UV plane of an MF buffer
  starts after the padded height, e.g. 1088, which `UpdateSubresource` cannot express).
- Seek budget: on the reference machine (Ryzen 7 5700G iGPU, Win11 26100) Microsoft's DXVA H.264 decoder runs ~1.75 ms per
  1080p frame regardless of bitrate (24 Mbps and 0.45 Mbps files decode equally fast; the video engine is only ~30 % busy;
  an asynchronous Source Reader with 4 requests in flight and two decoders in parallel were tried and gave no or +25 %
  aggregate). A seek to the last frame of a 2 s GOP (119 frames) therefore takes ~210 ms; 20 random seeks: p50 75-82 ms,
  p95 205-220 ms. The decoder test reports the p95 and enforces < 400 ms; MediaCache hides most of it with its prefetch
  window (see 6.4/6.7 for the cache-level numbers).
Why: measured behaviour of the platform decoder; the budget is unreachable by sequential decode from the keyframe here.
Impact: P05/P04 could shorten the recording GOP (1 s halves the worst case) if scrub latency matters more than size;
P09 should keep scrubbing on cached frames (`exact = false`) while the exact frame decodes.

## 2026-10-17 · P06 task 6.3 · AudioDecoder details
Spec said: `AudioDecoder` as declared; total frames of the 1080p pattern 240000 +- 48.
Done instead (additions only): `Shutdown()`, `UsingResampler()`, `Config::force_resampler` (tests run our conversion path on
files the reader converts itself), constants `kRate`/`kChannels`. The reader's 48 kHz stereo float type is verified with
`GetCurrentMediaType` after it is accepted. `Read` returns frames contiguous from the first packet after Init/Seek
(pts = base + n * 1e6 / 48000), not each packet's own timestamp. A seek at or past the duration makes the next `Read`
return `S_FALSE` (MF's `SetCurrentPosition` fails there). The pattern decodes to 240640 frames (MF's AAC encoder pads the
track to whole packets, see 6.1); the test accepts 240000 - 48 .. 240640 + 48. Sample accuracy was checked on the beep:
after `Seek(1.9 s)` the 2 kHz onset lands at frame 4792 (expected 4800, the detector's step is 4 frames).
Why: testability; MF behaviour.
Impact: none for callers.

## 2026-10-17 · P06 task 6.4 · Nv12Blit, MediaCache/FrameCache design choices
Spec said: `platform::Nv12Blit { Init; ToRgba }` (P05 interface); `FrameRef`/`MediaCache`/`CacheBudget` as declared in P06;
prefetch: seek when the decoder is outside `[window.start - 3 s, window.start]`; eviction outside the window first, then LRU;
thumbnails via a software decoder; peaks cached as `<fnv64(path+size+mtime)>.peaks`.
Done instead (additions only, specified members unchanged):
- `Nv12Blit` (src/platform, shaders/nv12_blit.hlsl via ct_compile_shaders): adds `Draw(ctx, y, uv, src_size, rtv, viewport,
  box_filter, alpha)` and static `CreatePlaneViews`; `ToRgba` caches views for the last 8 textures. The box filter is one
  pass of ceil(scale/2)^2 bilinear taps (not the "2-pass box filter"). Measured max |dRGB| 1 against the BT.709 reference.
- `FrameRef::rgba` (images are one BGRA texture; y/uv/nv12 null). `FrameRef`s stay valid until the 4th following
  `GetVideoFrame` (last 4 returned slots are pinned against eviction). `MediaCache` adds `GetStats`, `FrameDurationUs`,
  `FpsNum/Den`, `DurationUs`, `HasVideo`, `WaitForFrame` (tools/tests). `FrameCache` is a GPU-free bookkeeping class
  (slot indices into MediaCache's texture store) so its logic is unit-tested; a frame lasts until the next cached one when
  that starts within 1 ms (rounded MF durations leave 1 us holes).
- Prefetch order: the part of the window ahead of the playhead first, then the part behind it. Eviction: outside the window
  (LRU), then inside the window but behind the playhead; frames ahead of the playhead are never evicted — when the budget
  is full of them the thread stalls until the next hint (a 2.5 s playing window of 1080p is 466 MB, more than the 256 MB
  budget, so playback runs as a ring of budget-sized read-ahead). Forward-vs-seek: instead of the fixed 3 s rule, decode
  forward unless a keyframe is known/predicted in between (keyframes are learned from seek landings; decoders flag every
  output sample as CleanPoint, so the flag is useless); unknown GOP -> forward up to 1 s; never more than 3 s.
  Cold exact frame at 2.5 s: 64-89 ms (was 300-350 ms with the 3 s rule).
- Thumbnails: the software decoder is created on the IO pool right after `Open` (its creation takes ~0.5 s under load);
  newest request first; consecutive buckets decode forward without a seek. 650 ms for the first, 150-230 ms after.
- Peaks key also includes the stream index; cache dir = `<Paths logs dir>\..\cache` when `Paths::Init` ran, else
  `%LOCALAPPDATA%\Camtesia\cache`. The 1080p pattern gives 502 windows (AAC padding, see 6.1); the test accepts 500-502.
- Draw FrameRefs/thumbnails with `D3DDevice::Lock` held: worker threads (thumbnails, decoder copies) use the immediate
  context under that lock, so the UI thread must too (P08/P09).
Why: budget/latency measurements above; testability.
Impact: P08 samples `FrameRef::y/uv` (or `rgba` for images) and reuses `Nv12Blit`; P09 takes the device lock while rendering.

## 2026-10-17 · P06 task 6.5 · AudioEngine clock and control details
Spec said: `PositionUs = play_from + (frames - frames_at_play) * rate / 48000 - latency`, interpolated with QPC.
Done instead: no latency is subtracted: `IAudioClock::GetPosition` already reports the stream position of the sample
being played (docs), and `frames_at_play` is the device frame index of the first sample of the new playback (the
`device_position_frames` of the buffer in which the render thread applied Play). Measured: 200 ms after `Play` the
position is 171-184 ms in (the 20-30 ms difference is the output latency), then advances 502-503 ms over 502-503 ms
of wall time; rate 2: 1001-1006 ms. QPC extrapolation past the last `GetClock` reading is capped at 50 ms. Until the render
thread applied the latest command, `PositionUs` returns the requested start. Play/Pause go to the render thread through
an SRW lock the render thread only try-locks. Varispeed keeps one source frame of history between buffers. Negative
rates are silent (the PlaybackClock runs them on QPC). Additions: `Muted`, `PeakLevel`, `DeviceOk`, `LatencyMs`,
`DeviceName`. Default-device hot-swap is not followed (the editor reopens the engine; P09/P11).
Why: correctness of the clock; the render thread must not block.
Impact: P09's timeline mixer implements `AudioEngine::Source` fed from decoder threads (Pull must not block).

## 2026-10-17 · P06 task 6.6 · PlaybackClock / Player details; thumbnails on a second DXVA decoder
Spec said: `PlaybackClock` and `Player` as declared; thumbnails from a software `VideoDecoder` (`allow_hardware = false`).
Done instead (additions only):
- `PlaybackClock`: `UsingAudio()`, `ClampRate()` (|rate| 0.25..4, negative = reverse on QPC, 0/NaN -> 1),
  `SetQpcSourceForTest()`. The audio engine is the clock only while playing forward with a working device
  (`AudioEngine::DeviceOk`); every Play/Seek/SetRate re-anchors both clocks at the current position.
- `Player`: audio goes through a "ct-audio-feed" thread that decodes the first audio stream into a 1 s `AudioRing`;
  `Feed::Pull` (render thread) try-locks, drops stale frames to catch up with `from_us` and plays silence until data for
  the requested generation exists, so a seek never blocks the audio thread and audio rejoins in sync. Additions:
  `Close`, `Playing`, `Rate`, `Duration`, `HasAudio`, `ClockUsesAudio`, `Cache()`, `Engine()`, `ResetStats`. Tick stops at
  the end (last frame) and at 0 for reverse; `seek_to_frame_ms` = `Seek()` -> first Tick with the exact frame;
  `frames_late` counts newly shown frames already > 50 ms stale.
- Thumbnails use a second hardware (DXVA) decoder, falling back to software: with the machine at 70-100 % CPU (parallel
  agent builds) the software decoder needed 1.1-3 s for the first thumbnail; DXVA: 450 ms first (decoder creation),
  95-140 ms after.
- Real-time assertions in the playback tests (late frames, cache misses during 60 fps playback, the 1 s thumbnail) are
  enforced in Release only; Debug + debug layer under load only reports them. On a machine at 100 % CPU from other
  processes even Release misses them; the numbers in PROGRESS.md come from runs at <= 70 % background load.
Why: measured behaviour; non-blocking audio thread.
Impact: P09 drives `Player`-like logic per timeline (MediaCache per clip media, AudioEngine::Source = timeline mixer).
