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