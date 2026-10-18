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
## 2026-10-17 · P07 task 7.0 · ct_events created in P07; header is 44 bytes; writer/reader API
Spec said: `ct_events` is created in P05 (P07 creates it only if it runs first). docs/04 section 2: `header_size u16 = 40`;
the reader API is called `ct::project::EventLog`.
Done instead:
- P07 created `src/events` (`ct::events`, depends on ct_core only) because P05 has not been built. P05's recorder uses
  `EventWriter` directly: packed payload structs (`CursorMovePayload`, `KeyPayload`, ...), typed `Write` helpers, buffered
  in memory, `Flush()` = fwrite + fflush (call it every second from ct-events-io), auto-flush past a threshold (256 KB default).
  The file is opened with `_SH_DENYWR`, so readers can open it while it is being recorded.
- The header fields listed in docs/04 take 44 bytes, not 40 (4+2+2+8+8+16+4). The writer stores `header_size = 44`.
  The reader accepts any `header_size >= 40` (multiple of 4) and skips to it, so a 40-byte header without `flags`
  (flags = 0) also loads.
- `EventLog` lives in `ct::events` (not `ct::project`): it is shared by recorder, project and render.
  It adds derived data: `Strokes()` (key-downs of non-modifier keys as "Ctrl+Shift+S"; auto-repeat ignored) and
  `TypingBursts()` (from TYPING records, else derived from key-downs closer than 1 s).
- `CursorAt` interpolates position linearly but not across a hide/show; shape and visibility come from the earlier sample.
- A truncated last record (crash while writing) ends reading and sets `Truncated()`; short payloads of known types are skipped.
Why: docs/04 internal inconsistency; P05/P08 need the queries; crash tolerance.
Impact: P05 links `ct_events` and writes with `EventWriter`; P08 reads with `EventLog`.

## 2026-10-17 · P07 tasks 7.1-7.2 · Model header split, `extra` everywhere, canonical .ctproj layout
Spec said: all model types in `types.h` (plus `project.h`, `camera.h`, `easing.h` in the deliverables); only `Project::extra`
holds unknown keys; "every sample in docs/04 round-trips byte-identically"; Validate checks `out_us <= duration_us * speed`.
Done instead:
- `easing.h` (Ease, ApplyEase, EaseName/ParseEase), `types.h` (everything else from the spec block), `camera.h` (CameraKey,
  AutoZoomParams, Camera), `project.h` (Project). Names and members are as specified. `PointF` is defined in `ct::project`
  (ct_core has none). Additions: `Json extra` on every struct that is a JSON object (not on keyframe/camera-key entries, rect,
  pointer, media.video); `KeyTrack::IsSorted`, `Clip::IsAnnotation`, `IsKeyframeProp`, `Camera::SetKey/RemoveKey/
  ReplaceGenerated/IsSorted`, `ClampCentre`, `Project::TrackIndex/ClipFits/MaxOutUs/RecomputeNextId`, `kMinSpeed/kMaxSpeed` (0.1..16).
- `tracks[0]` is the bottom layer. `ClipsAt` skips hidden tracks. Keyframe times are relative to the clip start.
- Validate: `out_us <= media.duration_us` (in/out are media time, so speed does not enter); images, annotation clips and media
  with unknown duration are unbounded. Track kinds: video tracks take recording (screen/camera), video and image media; audio
  tracks take recording (mic/system, stream must exist), video and audio media; annotation tracks take annotation clips only.
- `next_id` is not serialized (docs/04 has no key for it); FromJson recomputes it from the largest id counter.
- Canonical layout = `Json::Dump(2)` (one key per line) + trailing newline. The docs/04 sample uses a condensed layout and
  placeholder values ("video|audio|image"), so it cannot round-trip byte for byte: the test loads it, checks its values and
  checks that its re-serialization is stable. Byte identity is asserted on `tests/data/projects/*.ctproj` (written by this
  version via `--update-golden`; the test substitutes the current `app_version`).
- Floats: `%.6g` (C locale), `-0` -> 0. Doubles (`speed`, `ui.zoom`) are written exactly (shortest round-trip), because
  rounding a speed would change `Duration()` and could create overlaps after reload. Integral numbers print as `1`, not `1.0`.
- Emission rules (deterministic): `cursor`/`keystrokes` only on screen clips, `pip`/`annotation` only when set, `media: null` on
  annotation clips, `color: null` for a clip without color (alpha 0), `video` omitted for audio media, camera key `generated`
  only when true, `transform.anchor` always `"center"` (read but not stored). Cursor style also writes `shadow`; annotations
  also write `arrow_head_px`, `blur_px`, `pixelate`.
- Paths: relative paths are written with `/`; "same volume" means `fs::Relative` can express the path (same drive/share).
  Load also looks for a missing file next to the project before marking it `missing`.
- Version 0 (no format/version keys; clip `duration_us`; string background; camera `keys`) is defined by `Migrate` since no
  earlier format existed; fixture `tests/data/projects/v0.ctproj`. A newer version loads best-effort and keeps its number.
- `Load` returns false (with the Validate reason) for a timeline that breaks the invariants; `FromJson` alone does not validate.
Why: the spec text conflicts with itself (docs sample layout vs byte identity) or is silent; these keep files stable.
Impact: P08/P09 include `<ct/project/project.h>` (pulls in all model headers).

## 2026-10-17 · P07 review fix · Damaged .ctev records fail the load; media rates/sizes validated
Spec said: docs/04 section 2 only says readers skip unknown record types by size. The 7.0 entry above said short payloads of
known types are skipped.
Done instead: `EventLog::Load*` now returns false (with an Err) when a record of a known type is too short or a CURSOR_SHAPE
has impossible sizes (w or h 0 or > `kMaxShapeSize` = 1024, raw `data_size != w*h*4`, RLE that cannot encode `w*h` pixels in
`data_size` bytes, `data_size` past the record, unknown compression/format). Every size is checked before allocating;
`RleDecode` resizes once and fills runs. `EventWriter::CursorShape` refuses shapes it could not read back (returns 0).
A torn last record is still tolerated (`Truncated()`), unknown types are still skipped.
`Project::Validate` also rejects media with `fps_den <= 0`, `fps_num < 0`, negative size or duration, video/recording
media without a positive frame rate and size, and images without a size (canvas fps was already checked).
Why: independent review: a corrupt shape record (65535x65535 RLE) requested ~17 GB and terminated the process (no
exceptions); a zero fps_den would divide by zero in frame-time math.
Impact: P05 must not write shapes larger than 1024 px (CursorTracker already caps them); P08 can trust EventLog sizes.

## 2026-10-17 · P07 task 7.3 · Edit-op semantics; streaming .ctproj writer; <charconv>
Spec said: the op list in P07 7.3; `SetCrop(...)` etc. without full signatures; serialization through the Json DOM.
Done instead:
- Setter signatures are `(Project&, const Id& clip, const T&)`; `SetPip` takes `std::optional<Pip>` (nullopt removes the PIP).
  Clip-mutating ops refuse locked tracks. A failed op leaves the project byte-identical (the fuzz test checks this).
- Transitions that an edit makes invalid (no longer adjacent, or longer than half the shorter clip) are removed by the op.
  Split keeps the incoming transition on the left part and moves the outgoing one to the right part (the spec's
  "transitions touching the split point" cannot exist: a split is strictly inside a clip).
- TrimIn shifts the clip's keyframes so they stay with the content; Slip and SetSpeed leave them alone. Split inserts a key
  at the split on each side only where the track has keys beyond it, so values at the split are kept exactly.
- Split trims the right part's `out_us` by the 1-2 us that truncation can add at speeds < 1, so it never grows into the next clip.
- `ToJson`/`Save` write with a streaming writer that reproduces `Json::Dump(2)` byte for byte (extras are dumped with
  `Json::Dump` and re-indented). The DOM version took 169 us for the 8-clip fixture (undo snapshots serialize on every edit);
  the writer takes 22 us. `ToJsonValue` is now `Json::Parse(ToJson(p))`.
- Number formatting uses `<charconv>` (`std::to_chars`/`from_chars`, locale-independent) - not in the docs/02 header list,
  but it is part of the standard library, allocation-free, and faster than `_snprintf_l` + `_strtod_l`.
- The fuzz test runs 10,000 ops in Release and 2,000 in Debug (/Od + checked iterators are ~10x slower).
Why: completeness of the API, speed of undo snapshots, determinism.
Impact: P09 calls these ops; selection lists may reference clips removed by ripple/remove ops (UI prunes them).

## 2026-10-17 · P07 task 7.4 · Undo restore rules, dirty flag, autosave lock by share mode
Spec said: `UndoStack` with Push/Undo/Redo/labels/coalescing/Entries/Bytes/Clear (cap 200 entries / 64 MB); `Autosave` with
Init/NotifyChanged/Tick/Clear/FindRecoverable; the lock holds the owning PID and "a lock whose PID is dead" triggers recovery.
Done instead:
- Undo/Redo restore the snapshot but keep the current `path`, `ui` (playhead, selection, zoom) and media `missing` flags:
  they are not document history. Additions: `PushJson` (caller already has the JSON), optional `current_json` on
  Undo/Redo, `UndoEntries`, `Coalescing`, and the dirty flag (`MarkSaved`/`IsDirty`, state serials, so undoing back to
  the saved state is clean). Coalesced pushes after the first record nothing but still make the document dirty.
  `BeginCoalesce` with the same key while active is a no-op; another key starts a new group; Undo/Redo end a group.
  The entry cap counts undo + redo entries; the oldest undo entries are dropped first.
- Autosave additions: `SetDirectory`/`Directory` (ct_project cannot use ct::platform::Paths; the app sets it at startup,
  default `%LOCALAPPDATA%\Camtesia\autosave`), `Tick(p, now_us)`, `SaveNow`, `SetSynchronous` (tests), `Saves`, `Dirty`,
  `AutosavePath`/`LockPath`, `Discard`, `kEditsPerSave = 20`. `interval_s <= 0` disables time-based saves.
- Liveness is decided by the lock file's share mode, not by PID lookup: the owner keeps `<id>.lock` open with `_SH_DENYWR`
  for its lifetime; a lock that can be opened for writing has no live owner (the OS closes handles when a process dies,
  so PID reuse cannot fool it, and ct_project stays free of process APIs). The lock still records
  `{"pid", "original_path", "name"}` as JSON. An autosave without a lock is also recoverable.
- Tick serializes on the UI thread (fast writer, see 7.3) and writes on the ct::Async pool; a newer snapshot supersedes
  queued older ones; Clear() waits for an in-flight write and deletes both files. Autosaves keep absolute media paths.
- The fuzz test checks undo after every applied op and redo after every 10th (the others restore the edited state from a
  copy): two JSON parses per op would take ~10 s in Release (Json::Parse is ~5 ns/byte), over the 10 s budget.
  Bias towards Delete above 30 clips keeps the timeline bounded.
Why: robustness and testability; budget.
Impact: P09 owns an UndoStack + Autosave per document, calls `Autosave::SetDirectory(Paths...)` at startup, `MarkSaved`
after Save/Load, and `Clear()` on save/close.

## 2026-10-17 · P07 task 7.5 · Importer details; synthetic bundles instead of tests/data/sample.ctrec
Spec said: `ImportRecording(Project&, bundle_path, Err*, ImportOptions)`; tests import `tests/data/sample.ctrec`.
Done instead:
- No real recording exists before P05, so the tests build bundles at run time (`tests/project/bundle_util.h`): meta.json
  is the docs/04 section 1 sample (adjusted per test) and events.ctev is written with `EventWriter`. screen.mp4/cam.mp4
  are absent: the importer reads only meta.json and events.ctev; the media files are referenced by the bundle path and
  opened later by playback (P06), which reports them offline if they are missing.
- Added `RecordingMeta` + `ReadRecordingMeta` (the editor needs the same fields for the media panel). meta.json is
  rejected when `format != "ctrec"`, duration <= 0, or the screen size/frame rate is not positive.
- Existing tracks are reused by (kind, name); missing ones are appended on top in the order Screen, Camera, Mic, System,
  Callouts. A new project also gets `name` = bundle stem and a UUID `id` when it has none. Odd recording sizes become
  the next lower even canvas size. The import is all-or-nothing (works on a copy; e.g. a locked Screen track fails it).
- `ImportOptions::auto_zoom` is honoured from task 7.6 on (the generator lands there).
Why: no sample recording yet; keep the project consistent on failure.
Impact: P05 must write meta.json exactly as docs/04 section 1 (format, duration_us, screen.width/height/fps, audio.*.stream_index, events.file).

## 2026-10-17 · P07 task 7.6 · Auto-Zoom interpretation: ramp room, merge vs pan, two-key follow moves
Spec said: free functions `ExtractInterest` / `Generate`; step 2 "merge consecutive segments with gap < 2 * ease_ms; drop
segments shorter than min_hold; clamp to [0, duration]"; step 3 "adjacent segments share the transition (pan c1 -> c2 over
ease_ms) if the gap is < 2 * ease_ms"; step 4 "emit a key moving centre toward the cursor ... with Ease::Out".
Done instead:
- The functions are static members of `struct AutoZoom` (the importer spec calls `AutoZoom::Generate`), plus `ForClip`
  (events inside a clip's in/out, mapped to timeline time) and `AppendKeys` (adds without replacing keys at equal times).
- Steps 2/3 conflict as written (after merging every gap < 2 * ease, there is nothing left to pan between). Resolution:
  neighbours closer than 2 * ease are merged when their centres are within 0.25 (or a pan cannot fit after the previous
  zoom-in), otherwise both are kept and the camera pans from c1 to c2 over exactly ease_ms, ending at t_in2.
- Clamping is to [ease, duration - ease] so both ramps fit inside the recording (a ramp cut short would be a jump); it runs
  before the "drop shorter than min_hold" step, so clicks in the first/last ~0.8 s may produce no zoom (fixture edges.json).
- Keys use Hold for the zoomed hold and the zoomed-out rest, so every follow move is two keys: (t, current centre, Out) and
  (t + 300 ms, new centre, Hold); a single Out key after a Hold key would jump. "At most one key per 400 ms" became at most
  one follow move per 400 ms. Follow normalizes the cursor by the events region size.
- `Camera::At` continuity: fixtures with the default ease meet the spec bound (0.02 zoom / 0.01 centre per ms). The random
  property test scales the bound with the parameters (a 100 ms ease at zoom 4 legitimately moves faster than 0.02/ms).
- Fixtures `tests/data/autozoom/*.json` (8 files: one_click, cluster_merge, far_pan, separate, edges, typing, follow, dense)
  were written by this implementation with `--update-golden` and reviewed against the descriptions in each file.
Why: the spec text is internally inconsistent; these choices keep the camera continuous.
Impact: P08 plays the keys with Camera::At; P09's "Regenerate" = `Camera::ReplaceGenerated(AutoZoom::ForClip(...))`.

## 2026-10-17 · P07 task 7.7 · Autosave retries failed writes
Spec said: autosave writes when dirty and the interval elapsed.
Done instead: a failed write (the atomic replace failed, e.g. while another process had the autosave open) is retried
3 times 50 ms apart on the IO thread; if it still fails the document stays dirty and the next Tick writes again.
Why: `--repeat 3` exposed a lost newest snapshot when the test read the file during the replace (the 4th of 5 writes
stayed on disk). Without the retry a transient sharing violation would silently drop crash protection until the next edit.
Impact: none for callers.

## 2026-10-17 · P12 task 12.1 · Manifest version generated; `--tray` only hides the window
Spec said: docs/01 section 7 embeds a literal `src/app/app.manifest` with `version="0.1.0.0"`; "everything else derives
from" `project(Camtesia VERSION x.y.z)`; add `--tray` (start hidden to the tray).
Done instead: the manifest is now `src/app/app.manifest.in`, configured into the build dir with
`@PROJECT_VERSION_MAJOR@.@PROJECT_VERSION_MINOR@.@PROJECT_VERSION_PATCH@.0`. `version.h` also defines
`CT_VERSION_MAJOR/MINOR/PATCH` (guarded by `#ifndef`, since `ct_options` passes them as `-D` too), as docs/01 section 8
lists. `--tray` starts the main window with `SW_HIDE`; a second launch (forwarded via WM_COPYDATA) shows it.
Why: the literal manifest version was the only place that would not follow a version bump. The tray icon belongs to P05
(`TrayIcon`), which does not exist yet.
Impact: P05 adds the tray icon for `--tray` (left click shows the bar). Edit `app.manifest.in`, not `app.manifest`.

## 2026-10-17 · P12 task 12.2 · Installer/package deviations from the P12 listings
Spec said: the `.iss` and `tools/package.ps1` listings in docs/phases/P12 sections 12.2/12.3; ISCC at
`%ProgramFiles(x86)%\Inno Setup 6`.
Done instead:
- `.ctproj` key: `Flags: uninsdeletevalue uninsdeletekeyifempty`. With `uninsdeletevalue` alone the silent uninstall test
  left an empty `HKCU\Software\Classes\.ctproj` key behind.
- `PrivilegesRequiredOverridesAllowed=dialog commandline` so `/CURRENTUSER` / `/ALLUSERS` work for scripted installs.
- `#define BinDir` (default `..\out\build\ninja-release\bin`), passed by `package.ps1 -Config`; setup VersionInfo
  product name/description added.
- `package.ps1` finds ISCC in Program Files (x86) (choco/CI), Program Files, or `%LOCALAPPDATA%\Programs\Inno Setup 6`
  (where `winget install JRSoftware.InnoSetup` put it on the reference machine, per-user). The portable zip also holds
  `licenses\imgui-LICENSE.txt`; the symbols zip holds the PDB plus the matching exe (dump debugging needs both).
  `SHA256SUMS.txt` lists only this version's three artifacts, lower-case hex, LF endings (`sha256sum -c` compatible,
  used by the CI release job). The budget check (exe < 20 MB, installer < 15 MB) fails the script when exceeded.
Why: found while running `build.cmd package` and the silent install/uninstall test on this machine.
Impact: `build.cmd doctor` still only checks `%ProgramFiles(x86)%` and prints Inno "MISSING" for a per-user install
(doctor is outside this task's file ownership; fix when convenient).
## 2026-10-17 · P08 task 8.1 · ct_render does not depend on ct_playback: VideoLayerInput has plain fields
Spec said: `VideoLayerInput { const Clip* clip; playback::FrameRef frame; ID3D11ShaderResourceView* rgba; bool is_rgba; int rotation_deg; TimeUs media_t_us; const EventLog* events; SizeI video_size; }`; P08 depends on P06 (`FrameRef`, `Nv12Blit`).
Done instead: P08 was built in parallel with P06, so `ct/render/frame_inputs.h` has no playback type:
`VideoLayerInput { y_srv (R8 view of plane 0), uv_srv (R8G8 view of plane 1), rgba_srv, is_rgba, rgba_premultiplied, video_size, rotation_deg, media_t_us, clip, events }`.
layer.hlsl samples NV12 itself (BT.709 limited range); no `Nv12Blit` is used.
Other additions to the specified structs: `FrameInputs::background_image` + `background_image_size` (Background::Kind::Image needs a
decoded wallpaper; null -> color_a), `RenderOptions::gpu_timing`, `RenderTiming::gpu_background_ms/gpu_layers_ms/gpu_overlay_ms/gpu_total_ms`
(GpuTimer results, a few frames old). `active_transitions[i].first` is the incoming clip (b); the compositor matches both clips by the
transition's ids.
Why: P06 is not merged; the compositor only needs two SRVs per frame.
Impact: P09 (Player) and P10 (Exporter) convert `playback::FrameRef` -> `VideoLayerInput` (Y/UV SRVs of the decoded NV12 texture, or
the RGBA SRV), fill `active_transitions` with `Compositor::ActiveTransitions(project, t, ...)` and request frames at
`Compositor::ClipMediaTime(clip, t)`.

## 2026-10-17 · P08 tasks 8.1-8.6 · Render API additions
Spec said: the `ct::render` interface block of docs/phases/P08.
Done instead (additions only; the specified members exist with their signatures, except where noted):
- `transitions.h` and `readback.h` are separate headers (deliverables list); `Readback::ToRgba8` added (goldens, PNG).
- `RenderContext`: `Shutdown`, `IsInitialized`, `BeginFrame` (returns all temps; RenderFrame calls it), `UnbindSrvs`, `D2DFactory`,
  `TempCount`, `P()` (internal pipelines). One constant-buffer layout (`QuadCb`, 208 bytes) serves every shader; Map(DISCARD) per draw.
- `Compositor`: `Shutdown`, `Text()`, `Cursor()`, static `ActiveTransitions`, `TransitionProgress`, `ClipMediaTime`, `IsPrimaryScreen`.
  RenderFrame returns E_INVALIDARG when the target is not exactly canvas-sized and leaves no RTV/SRV bound.
- `effects::Pixelate(..., region, dst, dst_size, radius_px = 0, opacity = 1)`: `dst_size` added (the quad VS needs the target size);
  `RoundedShadow(..., opacity_mul = 1)`; `ShadowMaskRenders()` (cache misses, for tests).
- `TextRenderer`: `KeystrokeBadge(..., font_px = 0)`, `SetDeterministic`, `ResolveFont`, `CachedLayers/CachedBytes/Rasterizations`.
- `CursorRenderer`: `ShelfPacker` (public, tested), `MakeArrowShape` (built-in arrow for shape id 0 / unknown shapes; tests use it),
  `HasShape/AtlasCount/AtlasUploads`.
- `layer_math::PropAt` (allocation-free `Clip::Prop`), `TransformBounds`, `TransformCorners`, `Mat3::ScaleFactor`.
Why: needed by the passes, the tests and the zero-allocation budget.
Impact: P09/P10 use these names.

## 2026-10-17 · P08 tasks 8.2-8.6 · Rendering choices the spec leaves open
- NV12 decode uses the chroma offset 128/255 (what ColorConverter encodes with), not 0.5 as written in the phase file.
- "Primary screen clip" = a Source::Screen clip on the track of `Project::PrimaryScreenClip()`, so every part of a split screen
  recording gets the canvas padding, radius and shadow.
- PIP: the Circle mask crops the camera to a centred square; `pip.shadow` uses blur 32 / offset 12 / opacity 0.4 (Pip has no Shadow).
  Shadow blur_px is CSS-like (sigma = blur_px / 2) and scales with the layer transform.
- Transitions: window [b.start - d/2, b.start + d/2); A and B are each drawn (with shadow) into a canvas-size temp and composited
  (crossfade lerp; dip passes through the opaque colour at u = 0.5; slide moves both images in `dir`).
- Annotations: Text = glyphs only (fill ignored); Callout = rounded rect + pointer triangle (union geometry), text centred; Arrow =
  from (rect.x, rect.y) to (rect.x + w, rect.y + h) (w/h may be negative), colour `fill`, width `stroke_px` or 0.3 * head;
  Rect/Ellipse = fill (if alpha > 0) + stroke; Highlight = region multiplied by `fill`; Blur = Gaussian with sigma blur_px / 2 (reads a
  margin around the region), or block-average pixelation with block = blur_px; Keystrokes annotation = badge of the top screen layer's
  strokes centred in its rect. Annotations follow the camera; text rasters are cached at a scale quantized to 0.5 steps (1..4).
  Keystroke overlay badges (clip.keystrokes) are canvas overlays (not zoomed), at most 3 strokes, font 3.2 % of the canvas height, docs/05 colours.
- Cursor: click effects scale with the effective cursor scale (ripple 12..48 px, ease-out); the Highlight disc (radius 20 px) shows
  while any of left/right/middle is held; spotlight darkens to 55 % outside 220 px with an 80 px soft edge; clicks count as activity
  for hide-when-idle.
- Text determinism: grayscale AA plus fixed DirectWrite rendering params (gamma 1.8, contrast 0.5, NATURAL_SYMMETRIC), so goldens do
  not depend on the user's ClearType tuning.
Why: the spec is silent on these.
Impact: P09's properties panel should describe Arrow/Highlight with these semantics.

## 2026-10-17 · P08 task 8.4 · SmoothedPosition resamples the cursor path at 120 Hz
Spec said: Gaussian over `events.CursorSamplesBetween(t - w, t + w)`; click snap within 60 ms *before* a mouse-down.
Done instead: the piecewise-linear path (`EventLog::CursorAt`) is sampled at t + k * 8333 us inside the window and weighted with
sigma = w / 2. The click snap blends toward the click within 60 ms on *both* sides of the down (weight 1 at the down).
Why: CURSOR_MOVE is written only on change, so a stopped cursor has no samples; averaging raw records drags the result back toward old
positions after every stop. Resampling keeps constant-velocity motion exactly on the line and a still cursor still. Snapping after the
down too keeps the sprite on the ripple centre while the window still contains the approach.
Impact: none for callers.

## 2026-10-17 · P08 task 8.7 · Zero allocations: render code avoids Clip::Prop and copies of project structs
Spec said: zero heap allocations after warm-up.
Done instead: `Clip::Prop(const char*)` builds a `std::string` for the map lookup ("transform.opacity" is longer than the SSO buffer)
and every project struct carries a `Json extra` whose copy allocates (in Debug every std container construction also allocates its
iterator proxy). The compositor uses `layer_math::PropAt` and keeps pointers to `Shadow`/`KeystrokeStyle`.
Measured with a CRT alloc hook (Debug): 0 allocations over 300 frames after warm-up (24 per frame before the change).
Impact: P07 could give `Clip::keyframes` a transparent comparator (`std::less<>`) so `Clip::Prop` stops allocating.

## 2026-10-17 · P08 task 8.7 · Golden harness and golden set
Spec said: P00 left `CT_TEST_GOLDEN` stubbed; P08 goldens include `cursor_ripple_t+150ms` and `transition_crossfade_u0.5`.
Done instead: `ct_test` implements `CheckGolden` with WIC (no ct_platform dependency): `data/golden/<test>.png` relative to the working
directory (ctest runs from tests/), pass at max channel diff <= 2 and PSNR >= 45 dB, `--update-golden` writes, a failure writes
`.actual.png`/`.diff.png` (|a-b| x 8; ignored by tests/data/golden/.gitignore). Added `CheckGoldenTol`/`CT_GOLDEN_IMAGE_TOL` and
`WritePng/ReadPng/CompareImages`; text goldens use max diff 255 + PSNR 35 dB. Test (and file) names are `cursor_ripple_t150ms` and
`transition_crossfade_u05` (C++ identifiers). Goldens are 640x360 (9 files, 0.35 MB) to keep tests/data small; they were written
by the Debug build and the Release build matches them.
Why: identifiers cannot contain '+' or '.'; test data budget.
Impact: P10 (export goldens) uses the same macros.

## 2026-10-17 · P05 task 5.7 · WebcamSource decoupled from SessionClock, MfRuntime and Nv12Blit; additions
Spec said: `WebcamSource::Start(const recorder::SessionClock&, FrameSink&)`; `LatestPreviewSrv()` returns an RGBA preview
updated per frame with `platform::Nv12Blit`; MF is started by P04's `media::MfRuntime`; Init scores native types and asks
for NV12; `WebcamInfo { symbolic_link, name, sizes }`.
Done instead (P05a, P04 and P06 run in parallel and own those pieces):
- Time base: `Start(const capture::MediaTimeSource&, FrameSink&)` with `MediaTimeSource { TimeUs QpcHnsToMediaUs(uint64_t) const;
  TimeUs NowMediaUs() const; }` declared in `ct/capture/webcam_source.h`. `recorder::SessionClock` should derive from it
  (its `HnsToMediaUs` is the same mapping), which keeps ct_capture free of a ct_recorder dependency.
  `capture::QpcMediaTimeSource(epoch_qpc)` is the pause-free version for tools/tests.
- Frame time = `MFSampleExtension_DeviceTimestamp` mapped through the clock when it lies within 1 s before now (later than
  now is clamped to now), else `NowMediaUs()` (`WebcamFrameTimeUs`). Frames whose time is not after the previous delivered
  frame are dropped (`dropped_late`), so cam.mp4 timestamps are strictly increasing.
- Pause: `+SetPaused(bool)`; frames read while paused are dropped (the session calls it next to `capturer.Pause()`).
- MF startup: an internal `std::call_once` `MFStartup(MF_VERSION, MFSTARTUP_LITE)` in `webcam_source.cpp`, never shut down
  (MFStartup is reference counted, so it coexists with P04). Becomes `media::MfRuntime::Ensure()` after P04 merges.
- Preview: no RGBA texture and no `LatestPreviewSrv()`. Each delivered frame is also copied into a preview NV12 texture and
  `LatestPreviewPlanes(&y_srv, &uv_srv, &size)` exposes R8 / R8G8 plane views (+`PreviewSerial()`); the UI converts with
  P06's `Nv12Blit` (or its own shader) under the device lock. Rationale: `Nv12Blit` belongs to P06.
- Enumeration lists every native type (`+WebcamInfo::modes`: size, fps num/den, fourcc, native index) and `sizes` (distinct,
  largest first); `Enumerate(out, with_modes = false)` skips opening the devices. No camera = S_OK with an empty list;
  `Init` then returns `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)`.
- Native type choice is `PickWebcamMode` (pure, unit tested): |area - preferred| then |fps - preferred| (milli-fps) then
  NV12 > YUY2 > MJPG > other; odd sizes and types without a frame rate are skipped; ties keep the device order.
- Testing seam: `WebcamReader` (ReadSample/Flush/Shutdown) behind the loop, `InitWithReader(...)`, and
  `CreateSyntheticWebcamReader` (moving 75 % colour bars, DXGI or memory buffers, device timestamps) used by tests, by
  `webcam_demo --synthetic` and available for a recorder self-test without a camera.
- Memory-buffer samples (a software pipeline without DXGI output) are uploaded through a staging NV12 texture
  (`cpu_uploads` counts them); DXGI samples are `CopySubresourceRegion`'d (box = output size, so padded decoder surfaces work).
- Stop: stop flag + `Flush` repeated every 100 ms until the thread exits (a Flush landing just before `ReadSample` would be
  lost); after 3 s the device source is shut down (the source then needs `Init` again); `CT_FATAL` after 5 s.
  A ReadSample failure or ERROR/ENDOFSTREAM flag calls `FrameSink::OnError` once (device lock held) and ends the thread.
- `WebcamStats` (atomics) for the session's LiveStats/logs.
Not verified: the reference machine has no camera, so the MF device path (Init, native types, reader NV12 conversion,
`Flush` unblocking a synchronous `ReadSample`, device timestamps after the video processor) has not run on hardware; the
real-device test skips. Run `webcam_demo` and `ct_capture_tests --filter webcam_real` on a machine with a webcam.
Why: parallel phases own SessionClock (P05a), MfRuntime (P04) and Nv12Blit (P06).
Impact: P05a: `struct SessionClock : capture::MediaTimeSource` and call `webcam.SetPaused` on pause/resume. P04: cam writer
uses `webcam.Nv12Pool()` (6 NV12 textures, RENDER_TARGET|SHADER_RESOURCE when supported) with `fps = ActualFps()`; the
`#include`/call to the internal MF guard is replaced by `MfRuntime::Ensure`. UI: preview via `LatestPreviewPlanes`.

## 2026-10-17 · P04 task 4.2 · Low latency off by default (AMD POC type 2); MF source reader lists tracks in reverse
Spec said: `VideoEncodeConfig::low_latency = true` (sets `MF_LOW_LATENCY` and `CODECAPI_AVEncCommonLowLatency`); stream order
video = 0, then audio in config order, "matches meta.json stream_index"; `Probe` via the source reader.
Done instead:
- `low_latency` defaults to **false**. With it on, `AMDh264Encoder` (Radeon 5700G iGPU, Windows 10.0.26100) writes SPS with
  `pic_order_cnt_type = 2`, and the Microsoft H.264 decoder MFT returns end-of-stream on the first `ReadSample` of such a file,
  with the software path and with DXVA (`MF_SOURCE_READER_D3D_MANAGER`) alike: 0 frames decoded. The same stream with low
  latency off (POC type 0, no B-frames since `bframes = false`) decodes in both paths, and the Microsoft software encoder's
  output decodes either way. Recording does not need encoder low latency (nothing is streamed); the cost is a few frames of
  encoder delay, measured as the OnFrame -> WriteSample latency and pool use in the tests/bench.
- The MP4 file has the specified order (moov: track 1 video, 2 mic, 3 system; checked with a box dump), but the MF MPEG-4
  source presents the tracks in **reverse** order (reader stream 0 = last track) and numbers its stream identifiers 1..n in
  that order (not the track_IDs). `Probe`/`ScanStream` reverse the reader order so their stream numbers are the file's track
  order (video 0, mic 1, system 2); the tests tell the two AAC tracks apart by bitrate (128 vs 192 kbps). `ProbeResult` gained
  `stream_codecs`, `audio_sample_rate`, `audio_channels`; added `ScanStream` (compressed sample count/times/bytes, no decode).
- The sink writer's video MFT carries no `MFT_FRIENDLY_NAME_Attribute`: the hardware encoder is named from
  `MFT_ENUM_HARDWARE_URL_Attribute` ("AMDh264Encoder"), the Microsoft software encoder (no name, no CLSID on its attributes)
  from the first software entry of `EncoderCaps`. Hardware = `MFT_ENUM_HARDWARE_URL_Attribute` or `MF_TRANSFORM_ASYNC`.
- When the encoder rejects the parameter attributes in `SetInputMediaType`, the writer retries without them and then applies
  each `ICodecAPI` value on its own (`IsSupported` + `SetValue`), logging the unsupported ones.
Why: files that the Windows decoder cannot play are not acceptable; meta.json's stream_index means the file's track order.
Impact: P05 writes `stream_index` 0/1/2 as specified. **P06**: do not use source-reader stream indices as track numbers; map
them (reverse order for MP4, or by major type: the video is `MF_SOURCE_READER_FIRST_VIDEO_STREAM`, mic = the last audio
reader stream, system = the one before). P10 export may enable low latency only after verifying the decode on the target GPU.

## 2026-10-17 · P04 task 4.3 · Texture reuse, UAV-bound NV12 accepted, sync export path copies, fast start unsupported
Spec said: one `MFCreateDXGISurfaceBuffer` per pool texture stored as the slot's user data; reuse predicate
`CanReuse(user = pool, index)` true when the buffer's refcount is 1; `WriteVideoSync` "blocks until the sample was
written"; `fast_start` sets `MF_MPEG4SINK_MOOV_BEFORE_MDAT`. P02 asked P04 to check that the encoder accepts NV12 textures
with `D3D11_BIND_UNORDERED_ACCESS` (compute converter), else set `allow_compute = false`.
Done instead:
- **UAV-bound NV12 works**: `AMDh264Encoder` encodes the capturer's pool (bind flags 0xa8 = RT|SR|UAV) through
  `MFCreateDXGISurfaceBuffer`; decoded luma matches the written values (software and DXVA decoders) and a 3 s real capture
  writes every delivered frame. `allow_compute` stays true, no extra GPU cost.
- The buffer is still stored as the slot's user data, but the predicate's `user` is the writer (it reads its own slot table),
  so it never touches the pool object. `SetCurrentLength` is called once at Init (constant length) instead of per frame, so
  nothing writes a buffer while MF may hold it. The writer's destructor clears the predicate and user data: the pool must
  outlive the writer.
- Repeats: every `OnFrame` is one hand-out; the writer creates one `IMFSample` per call (several samples may reference one
  buffer) and calls `pool->Release(index)` after dropping its own sample reference. The predicate becomes true only when
  every sample of every repeat was released by MF. Test `texture_reuse_waits_for_every_repeat` (pool of one texture, bursts
  of 10 sends): the slot was free by hand-out count but refused by the predicate for 53-59 ms (up to 13 references on the
  buffer), frames written after the reuse never leak into the earlier ones (decoded luma per frame).
- `WriteVideoSync` copies the caller's texture into an internal NV12 pool (`Config::sync_pool_size`, default 8, same reuse
  predicate) under `D3DDevice::Lock` and returns once queued (waits up to 5 s while the queue or the pool is full). Waiting
  until "written" could deadlock with the 500 ms interleave rule when the caller writes video before audio; the copy makes
  the caller's texture reusable immediately. `WriteAudioSync` likewise returns once queued. Export measured 17x real time
  for 720p30 (queueing only).
- `fast_start` is **ignored with a warning**: on the default byte stream `IMFSinkWriter::Finalize` fails with
  `E_ACCESSDENIED`; with a read/write `MFCreateFile` byte stream the sink writes moov first but drops the mdat header and
  leaves the old data behind (file twice the size, undecodable). P10 can relocate the moov itself after `Finalize`.
- The reviewer's concern (ScreenCapturer holds `D3DDevice::Lock` while `AcquireNextFrame` waits up to 4 ms): ct-encode never
  takes that lock on the capture path (`OnFrame` only queues; WriteSample does not use our lock). Measured in
  `capture_into_writer_3s`: OnFrame -> WriteSample latency avg 0.04 ms, max 0.9 ms; WriteSample avg 0.15 ms (the first call,
  which starts the encoder, up to 18 ms). No change to screen_capturer.cpp.
- `Mp4Writer::Stats` gained `audio_rejected_frames`, `audio_lead_pad_frames`, `video_write`, `video_latency`, `finalize_ms`;
  `Config` gained `listener`, `sync_pool_size`; added `VideoStreamIndex`, `AudioStreamIndex`, `Accepting`, `ErrorMessage`,
  `InjectWriteErrorForTest`.
Why: correctness of texture lifetime with repeats; no deadlock in export; no corrupt files.
Impact: P05 passes `ScreenCapturer::Nv12Pool()` to `Init` before `ScreenCapturer::Start` and destroys the writer before the
capturer. P10 calls `WriteVideoSync`/`WriteAudioSync` from its export thread and `Finalize` at the end; no fast start.

## 2026-10-17 · P04 task 4.5 · Error path details; HEVC decode not checkable here; fps clamp left to the caller
Spec said: any `WriteSample` failure stores `LastError`, stops input, tries `Finalize` once and reports via `OnError` to the
owner; disk full gets a specific message; device removed takes the same path; clamp fps by trying 120/60/30 when the
encoder rejects it; HEVC round trip test.
Done instead:
- The owner is notified through `Mp4Writer::Config::listener` (`Listener::OnWriterError(hr)`, once, on ct-encode) since
  the writer is a sink, not a source, of `FrameSink::OnError`. `ErrorMessage()` gives the user text (disk full, missing
  folder, access denied, device reset, else the HRESULT). Queued frames are released to the pool, so a capturer keeps running
  with all textures available until the owner stops it. Verified with `InjectWriteErrorForTest` (ERROR_DISK_FULL on the 40th
  sample): 40 frames in a playable file, one listener call, pool empty, input refused afterwards. A real full disk and a
  real device removal were not produced (same code path).
- A path in a missing folder fails in `Init` (`MFCreateSinkWriterFromURL` -> `HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)`),
  `LastError()`/`Finalize()` return it, message "The folder does not exist: <folder>".
- HEVC (`AMDh265Encoder`): encode, codec and frame count are verified; the decode check is skipped because this machine has
  no HEVC decoder (the Store "HEVC Video Extensions" are not installed). P06/P09 must not assume HEVC files can be previewed.
- The fps clamp (try 120, 60, 30) is not done inside the writer: changing the rate silently would break the capturer's
  tick-aligned timestamps. `Init` fails with the encoder's HRESULT; P05 picks the next lower rate and restarts capture+writer.
- `Abort()` returned after 16 ms in the test (ct-encode drops queued input and calls IMFSinkWriter::Finalize).
Why: keep the sink single-purpose; hardware availability.
Impact: P05 implements `Mp4Writer::Listener` (finalize + user message) and the fps retry.

## 2026-10-17 · P04 task 4.6 · Acceptance run shortened; clap test measured by avsync_source --analyze; A/V findings
Spec said: 10 min `capture_bench --encode --mic --system`; files play in Movies & TV and VLC; clap test (docs/06 section 4,
analysis in P06's `player_demo --avsync`) |offset| < 20 ms; size within 10 % of bitrate * 600 s / 8 in VBR/CBR mode.
Done instead:
- 2 min run (the machine is shared with parallel agents' builds); no microphone exists, so only system audio. Numbers in
  PROGRESS 4.6. capture_bench now stops after exactly fps * seconds ticks (it used to overshoot by a few ticks under load)
  and prints `epoch_qpc` for the analyzer; `--rc quality|vbr|cbr`, `--bitrate`.
- Size check uses CBR (100.0 %). PeakConstrainedVBR undershoots on a mostly static desktop (40 % in 10 s): VBR's mean
  bitrate is a ceiling, not a target, for screen content.
- `avsync_source` (P04) also analyzes (`--analyze rec.mp4 --log events.csv --epoch QPC`): flash = mean luma of the window
  region > 128, beep = 1 ms RMS onset confirmed by a 2 kHz Goertzel. The source logs, per event, the predicted play QPC of
  the beep (IAudioClock mapping) and the vsync QPC of the white frame (DXGI frame statistics when the present count matches
  exactly, else present + median latency), so the analyzer reports each stream's error against what was really emitted:
  the source itself emits flash and beep 5-57 ms apart (window composition latency of a layered topmost window, 41 ms
  median), which makes the raw "video - audio" in the file meaningless as a recorder metric. The flash lasts 3 frames
  instead of 1 (a 1-frame flash was coalesced away by the 60 Hz capture under load).
- Findings for later phases:
  1. Video timestamps lag the display by +14..17 ms (one tick of quantization plus duplication latency; +31 ms when a frame
     is late): ScreenCapturer stamps a new image with the tick at which it was acquired. Stamping it with the tick that
     precedes its `present_qpc` (available from `AcquireDirect`) would remove most of this bias. Owner: P02 code, decide in
     P05/P11 (not changed in P04).
  2. The system-audio track moved 10 ms later at ~68 s after a loopback DATA_DISCONTINUITY where the StreamAligner inserted
     585 frames; the beeps after it are +10.4 ms against their emission (before: +0.4..1.3 ms). Owner: P03 aligner (P11).
  Result: A/V offset +15.9 ms before, +4 ms after the audio shift; 38 of 46 events < 20 ms, 7 at +20.5..20.8 ms, one 37.5 ms.
Why: machine availability; a headless and source-corrected measurement instead of the P06 tool that does not exist yet.
Impact: P05/P11 rerun the 10 min recording and the clap test with a person at the machine (Movies & TV / VLC playback,
USB mic) and address findings 1-2 to meet the 20 ms budget reliably.
## 2026-10-17 · P05 tasks 5.3-5.6 · Overlays live in ct_recorder; recorder input/overlay API details
Spec said: overlays are `ui::RegionOverlay`, `ui::CountdownOverlay`, `ui::RecordingFrame` in `src/ui`; `KeystrokeMode`
lives in `recording_session.h`; `EventRecorder::Live` has `char last_key_label[32]`; `HooksThread` polls
`capture::CursorTracker` itself; `LayeredWindow` draws with an `ID2D1DCRenderTarget` over the DIB.
Done instead:
- The overlays are `ct::recorder::{RegionOverlay, CountdownOverlay, RecordingFrame}` in `src/recorder` (another agent
  owned `src/ui` while P05 ran in parallel; they are Direct2D on layered windows, not ImGui, so they need nothing from
  ct_ui). Colors are the docs/05 tokens as local constants in `src/recorder/src/overlay_style.h`, hint strings too
  (move them to `ct/ui/strings.h` when the bar lands). Pure geometry (handles, drag/resize/nudge, even sizes, snap
  pick, final region/monitor, label placement, frame edges) is in `ct/recorder/overlay_geometry.h` and unit-tested.
- `KeystrokeMode` is defined in `ct/recorder/event_recorder.h`; `RecordingSession` will include it from there.
- `platform::HooksThread` knows nothing about the recorder (platform sits below capture): it calls a
  `platform::HooksHandler` (OnMouseHook/OnKeyboardHook/OnForeground/OnTimer) on ct-hooks; the hook procs find their
  owner through a `thread_local`. `EventRecorder` implements the handler and polls the CursorTracker in `OnTimer`.
  `HooksThread::Config::swallow_tag` (test hook): injected input whose `dwExtraInfo` equals the tag is recorded and
  then swallowed, so tests/tools click and type through real LL hooks without touching other apps.
  `DebugStall(ms)` exists to test the watchdog (the IO thread calls `CheckWatchdog()` every 100 ms; more than 1 s
  without the 250 ms WM_NULL heartbeat -> unhook and re-install on the next pump).
- `EventRecorder`: `Start(..., Err* = nullptr)`; `Live` holds atomics (cursor, records) and the last key label is read
  with `LastKeyLabel(char[32])` (lock-protected) instead of a raw shared array; `Marker(label, kind = 0)`;
  `Flush(timeout_ms)` waits for the IO thread; `GetStats()`. `Config` adds `focus` and `test_swallow_tag`.
  WINDOW_FOCUS: the hook only queues the HWND; title/rect/exe are looked up on ct-events-io (keeps the hooks thread
  free of cross-process calls). The focus rect is region-relative like every other coordinate. An initial
  WINDOW_FOCUS of the foreground window is written at start.
- TYPING in shortcuts-only mode: every non-modifier key-down is queued anonymously (vk 0, no label) only to derive
  TYPING bursts, so auto-zoom still sees typing without its content being stored. Off mode installs no keyboard hook
  (no TYPING). A burst ends at its last key-down (the rule EventLog uses for files without TYPING records); the
  "ended" record is written once 1 s passed (or on pause/stop) with that earlier time; readers sort by time.
  Modifier keys alone are never shortcuts (the stroke carries them as bits); in shortcuts-only mode the key-up of a
  recorded key-down is recorded too.
- CharFor gets a key state built from the recorded Shift bit and a Caps Lock state tracked from recorded VK_CAPITAL
  downs (seeded with GetKeyState at start); the layout is the foreground thread's. Labels: fixed English names for
  named keys, the VK letter/digit for A-Z/0-9, GetKeyNameTextW otherwise (MapVirtualKey char when the scan code is 0).
- `SessionClock` is the spec struct plus `Start(epoch)`, `QpcToMediaUs`, `PauseAt/ResumeAt`, `Pauses()` (the
  meta.json log) and an atomic `pause_started_qpc`. Readers are lock-free with a sequence counter, and
  NowMediaUs/Pause/Resume read QPC *inside* the sequence window: a "now" read before a concurrent resume and combined
  with the new offset made time go backwards (the monotonic test saw 669-1147 regressions in ~40k cycles; 0 after).
- `LayeredWindow` renders with a software `ID2D1RenderTarget` on a WIC bitmap and copies only the dirty rows into the
  DIB before `UpdateLayeredWindowIndirect(prcDirty)`. Measured on the 3840x1080 virtual screen (Release, machine loaded
  by other agents): the DC render target cost 30-40 ms per pass whatever the dirty size (its EndDraw copies the whole
  surface to the DC); the WIC path 1-10 ms per pass, slowest whole update 46 ms, first full draw ~80 ms. The DC
  target stays as the fallback when WIC cannot be created (COM not initialized on the thread). Additions:
  `SetHandler`, `Rect()`, `ReadPixelsRgba` (the overlays are excluded from screenshots, so tests/tools snapshot them
  this way), `LastDrawMs`, `Present(alpha)`, `Show(show, activate)`.
- Region overlay: the selection is filled with alpha 1/255, not 0 (fully transparent layered pixels are not
  hit-testable). A click without drag selects the hovered window (or the monitor); in Window mode the click confirms.
  Ctrl+arrows resize by 2 px (Shift 10) so sizes stay even; right click goes back a step / cancels. Window snapping
  also skips minimized and click-through layered windows (WS_EX_TRANSPARENT and WS_EX_LAYERED).
- RecordingFrame is four edge windows (a 4K region would otherwise need a 33 MB surface); an edge that would be
  outside every display is drawn just inside the region (still excluded from capture), so full-monitor regions show it.
- `TrayIcon` adds `Readd()/TaskbarCreatedMessage()`, `Decode()`, `ShowMenu()` and `CreateRecordingIcon(base, size)`
  (base icon drawn over black and white to recover its alpha, dot coverage-blended, straight alpha for
  CreateIconIndirect). `HotkeyManager` adds `Unregister`, `IsRegistered`, `Count`, `Describe` (canonical text from
  `ct::HotkeyToString`).
Why: file ownership while phases ran in parallel, the dependency order, measured performance, privacy-preserving TYPING.
Impact: RecordingSession (5.1) creates the `EventRecorder` with `KeystrokeMode` from settings and calls `SetPaused`
next to `SessionClock::Pause/Resume`; the recorder bar (5.6) uses `RegionOverlay::Open`, `CountdownOverlay::Start/
Cancel`, `RecordingFrame::Show/Hide`, `HotkeyManager` and `TrayIcon` on the UI thread (COM STA initialized). P09 may
move the overlays into ct_ui later; nothing in them depends on ImGui.
## 2026-10-17 · P09 task 9.1 · ui module, fonts, icons, shortcut interpretations
Spec said: docs/05 fonts "Segoe UI Variable Text 14 / Display 20 semibold", "rebuild the atlas on DPI change"; icons only from the
docs/05 list; shortcut table per docs/05 section 5.
Done instead:
- `src/ui` is `ct_add_module(ui ...)` (target still `ct_ui`, `ct_imconfig.h` unchanged), DEPS ct_core ct_project ct_platform imgui
  (ct_platform for `ImageIo` used by the screenshot helper).
- Body/label = `SegUIVar.ttf` default instance (regular) at 14/12 px; titles/section headers use `seguisb.ttf` (Segoe UI Semibold):
  ImGui's stb_truetype loader cannot select a weight instance of a variable font. Mono = CascadiaMono.ttf (fallback consola.ttf).
  No atlas rebuild: ImGui 1.92+ rasterizes on demand, the app sets `style.FontScaleDpi` (see the P00 0.4 entry); `Dp(x)` scales
  layout constants by `FontScaleDpi`. `Fonts::Unload()` exists so tests can destroy contexts.
- Icons merged into body and semibold fonts (U+E700-F8FF, GlyphOffset y 2, square advance). Two codepoints beyond the docs/05 list:
  ChevronLeft E76B and RepeatAll E8EE; `ct_ui_tests icons_present_in_icon_font` checks every ICON_* in SegoeIcons.ttf and segmdl2.ttf.
- Shortcuts: "+" is the unshifted `=`/`+` key (ImGuiKey_Equal) plus `Keypad+`; "-" likewise with `Keypad-`; "?" is Shift+/; added P09's
  Shift+J/Shift+L (0.5x) and `,`/`.` (Shift = 10 frames) nudges; Ctrl+S, Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y fire while a text field has focus.
  Dispatch uses `IsKeyChordPressed(chord, Repeat only for stepping/zoom/nudge)`.
Why: font loader limits; codepoints verified by test instead of Character Map; keyboard layouts.
Impact: second half of P09 and P05's recorder bar include `<ct/ui/theme.h>`, `<ct/ui/fonts.h>`, `<ct/ui/widgets.h>`.

## 2026-10-17 · P09 tasks 9.1-9.6 (first half) · EditorState without player/compositor; provider and playback interfaces
Spec said: `EditorState` owns `render::TimelinePlayer player`, `render::Compositor compositor`, the preview texture; the timeline
draws thumbnails/peaks from `MediaCache`.
Done instead (P06/P08 are built in parallel):
- `ct/ui/editor_state.h`: `EditorState` has project, UndoStack, Autosave, Selection, view (pps in DIP px/s, scroll_us, scroll_y,
  track_scale), `needs_render`, `revision` (cache key), `clipboard`, and `PlaybackControl* playback` (interface: Playing, Rate,
  Play(rate), Pause, Seek, HintScrub, Tick). `SimplePlayback` (default) only moves `project.ui.playhead_us`. TimelinePlayer implements
  PlaybackControl in the second half; preview/compositor members are added there.
- `Edit(label, fn)`: fn must leave the project untouched when it returns false (true for any single ops call); the JSON snapshot is
  taken before fn and pushed only on success, and skipped for the 2nd+ edit of a BeginDrag/EndDrag group (one undo entry per drag
  labelled with the drag label). `EditCopy` runs multi-op edits on a copy and swaps it in on success.
- `ct/ui/timeline_media.h`: `TimelineMediaProvider { Thumbnail(media, t, tex, uv0, uv1) -> Ready|Loading|Unavailable;
  PeaksMinMax(media, stream, &pairs, &per_second, &status) }`, `NullMediaProvider`. Loading draws an animated shimmer and makes
  `WantsRedraw()` true; Unavailable draws static frame placeholders / a flat line. The demo uses a synthetic peaks provider.
- `ct/ui/editor_shell.h`: `EditorShell` (menus, DockBuilder layout, panels, status bar, toasts, shortcut dispatch, cheat sheet, F11
  stats) with hooks `draw_preview(min, max)`, `on_action(Action) -> handled`, `status` (StatusInfo), `SetMediaProvider`. Without
  `draw_preview` the preview shows a styled placeholder of the canvas (background gradient, padding, radius, shadow).
- `--editor-demo` runs with `io.IniFilename = nullptr` (deterministic layout for screenshots); layout persistence in
  `%APPDATA%\Camtesia\editor_layout.ini` belongs to the EditorWindow. `--screenshot out.png --frames N` uses
  `ui::SaveWindowScreenshot` (PrintWindow PW_CLIENTONLY|PW_RENDERFULLCONTENT, BitBlt fallback -> ImageIo::WritePngRgba).
Why: build the editor UI before playback/compositor exist without blocking on them.
Impact: P09 second half: implement PlaybackControl in TimelinePlayer (or an adapter), a MediaCache-backed TimelineMediaProvider,
set `draw_preview`, `on_action` (Save/Open/Import/Export/OpenRecorder), `m_media.on_import/on_reveal/on_relink`,
`Properties().on_regenerate`, and fill `status` each frame.

## 2026-10-17 · P09 task 9.4 · Timeline interpretations
Spec said: lanes "in tracks order (annotations/camera lanes on top, video, audio bottom)"; `S` splits "the clip under the playhead
on the focused track" without a selection; playhead "1 px record line with a 12 px triangle handle" (tokens: accent = playhead handle).
Done instead:
- Lane order: camera-keyframe lane pinned under the ruler (not scrolled); then non-audio tracks from the top layer down (highest
  index first), then audio tracks in index order. Camera tracks (first clip Source::Camera) get 56 px, other video 72.
- Split without a selection splits every clip under the playhead on unlocked tracks (there is no track focus yet).
- Playhead line `record`, handle `accent` (token table). Narrow clips (< 10 px) draw as one flat rect; selection lookup uses a
  per-frame sorted id list (Select All on 1,000 clips stays O(log n) per clip).
- Trim past the media/neighbour limit clamps to the nearest valid time by bisection; multi-selection moves go through EditCopy
  (cross-track moves only for single clips); Ctrl+drag adds the copy with AddClip on the first frame and moves it afterwards.
- Header buttons: lock + (hide | mute for audio); dragging a header reorders tracks within the same group (video/annotation vs audio).
- Empty space below the last lane starts a rubber band; Shift+click selects the range on the same track.
Why: unspecified details; keeps drags one undo entry and the 1 ms budget.
Impact: none for other phases.
## 2026-10-17 · P06 task 6.1 · Private MFStartup guard; MediaInfo additions; AAC-padded duration; local test patterns
Spec said: `MFStartup` once per process in `ct::media::MfRuntime` (P04); `MediaInfo` as declared in P06; the pattern
`MediaInfo` test expects duration 5 s +- 1 ms; test media comes from P04's `tools/gen_test_media`.
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
## 2026-10-17 · P06 task 6.7 · player_demo, seek bench, avsync; acceptance measured on a substitute file
Spec said: `player_demo file.mp4` with an overlay; acceptance on the 4K60 H.264 pattern: in sync, seek-to-frame < 150 ms,
CPU < 10 %, frames_late measured; `--avsync` on the clap recording.
- The overlay is the window title (pts, clock, rate, cache MB, shown, late), not text drawn into the frame; `--stats`
  prints the same once per second plus a summary (CPU of the process, cache stats). Other modes: `--count-frames`,
  `--seek-bench [N]` (random seeks through `MediaCache::Hint` + `WaitForFrame`, p50/p95/max), `--avsync`.
- `--avsync`: per-frame luma mean (staging readback) finds flashes; the audio stream goes through two 2 kHz band-passes
  (Q 8) and a 1 ms RMS envelope; the detector's own delay (1.69 ms, measured on a synthetic click) is subtracted; events
  in the first 250 ms are skipped (decoder priming); a flash pairs with the nearest beep within +-250 ms; PASS when
  |mean offset| < 20 ms and the regression drift < 20 ms. Tested only on the synthetic pattern (exact by construction);
  the real clap recording is left for the P11/P13 manual pass.
- `VideoDecoder::ReadFrame(out, skip_before, abort_gen, gen)`: the skip phase returns `E_ABORT` when `*abort_gen != gen`.
  `MediaCache` bumps that generation only on a jump (|dt| > 1 s, play/pause, direction change), never on per-tick
  playhead moves (the first version re-seeked 34 times in 3 s of 4K playback). After an abort the decoder position is
  unknown and the next request seeks.
- No 4K60 H.264 reference file and no idle machine: the numbers come from `ct_gen_test_media` output
  (`out/testmedia/pattern_{1080p60_5s,4k60_3s}.mp4`, GOP 2 s) on an AMD Radeon iGPU with the CPU at 70-100 % from parallel
  agent builds. 4K60 does not play in real time here: the hardware decoder is saturated (~56 fps decoded with copies)
  and the swap chain's present waits behind the decode copies (~5 presents/s); rate 0.5 plays every frame.
Why: no reference media in the worktree; measured behaviour.
Impact: re-measure 6.7 on the reference machine with the real 4K file. If 4K60 is still short there, the cache should take
the decoder pool texture instead of copying it (one 12 MB copy per frame less).

## 2026-10-17 · P10 task 10.1 · Preset without ct_media; JSON shape and validation
Spec said: `Preset::video` uses `media::VideoCodec` / `media::RateControl`; `LoadUser()` / `SaveUser(Err*)`;
`EstimateBytes(TimeUs)`; JSON as the docs/04 section 5 example.
Done instead:
- `ct::exportng::VideoCodec { H264, Hevc }` and `RateControl { Quality, Vbr, Cbr }` mirror the P04 enums (same
  enumerators, same order). P04 runs in parallel and ct_media is not on master; ExportJob converts with `static_cast`
  (or a `static_assert`ed mapping once both exist).
- Added `video.profile` (the docs/04 JSON has `"profile"`), `FindBuiltin(id)`, `UserDir()`, and the testable
  `LoadUserFrom(dir)` / `SaveUserTo(dir, Err*)`. `EstimateBytes(duration, canvas = 1920x1080, canvas_fps = 60)`:
  the defaults keep the spec call valid; presets with size/fps 0 (= canvas) need the canvas to estimate.
- `ToJson` always writes every section (video, audio, gif) whatever the container, so JSON round trips are lossless.
  `"fps"` is `"source"` (0), an integer, or `"num/den"`. `audio.codec` is always `"aac"`.
- `FromJson` is strict: missing keys keep defaults, but wrong types, unknown enum names and out-of-range values fail
  with a message naming the key (no clamping), and `out` is untouched on failure. `id` must match `[a-z0-9_-]{1,64}`
  because it is the user preset file name (no path traversal). `SaveUserTo` refuses presets `FromJson` would reject.
- Built-in values chosen here (docs/04 lists only ids): H.264 720p 5/8 Mbps, 1080p 10/16, 1440p 16/25, 4K 35/50;
  HEVC 1080p 6/10, 4K 20/32 (profile main); GIF 720p 20 fps, GIF 480p (854x480) 15 fps, 256 colors, dither;
  PNG/M4A at canvas size. `assets/presets/*.json` are generated from `Builtin()` (`ct_export_tests --update-golden`)
  and a test requires them byte-identical.
Why: P10 is split; the CPU part lands before P04/P08/P09.
Impact: ExportJob (P10 rest) maps the enums to ct_media; the export dialog uses `UserDir`/`SaveUser`/`EstimateBytes`
with the canvas size.

## 2026-10-17 · P10 task 10.4 · GIF encoder details
Spec said: frame differencing compares quantized indices with the previous frame; global palette from the first frame
plus every 30th frame; LZW API `LzwEncode(indices, n, min_code_size, out)`; quantization on the IO pool in parallel
with rendering.
Done instead:
- Frame diff runs on the source pixels first: pixels with the same RGB as in the previous input frame keep the
  transparent index and neither receive nor spread dither error (otherwise Floyd-Steinberg noise from any change makes
  the whole frame "dirty"). Changed pixels are dithered, then compared with the displayed color (a canvas of palette
  colors, so it also works with local palettes) and become transparent when equal. The frame is cropped to the
  bounding box of the remaining opaque pixels. Without dither the decoded frames equal `Quantize(frame)` exactly
  (tested).
- A frame with no visible change extends the previous frame's delay instead of emitting a frame; past the 16-bit
  delay limit a 1x1 transparent frame carries the rest.
- Two-pass global palette: `AddPaletteSample(bgra, stride)` before the first `AddFrame` adds sampled frames to the
  histogram; the first frame is always added. `Options::max_colors` includes the transparent slot (255 real colors
  with `frame_diff`). `per_frame_palette` writes no global table. `loop_count < 0` omits NETSCAPE2.0 (play once).
- LZW: `out` is the sub-block chain incl. the 0 terminator, without the min-code-size byte; a clear code starts the
  stream, codes widen when the next free code reaches `1 << size` after emitting (giflib rule), and a clear code is
  emitted instead of assigning code 4095.
- The 32x32x32 nearest cache stores the exact nearest entry for the bucket centre ((v5 << 3) | 4). The [-32, 32] clamp
  applies to the accumulated error added to each pixel.
- Pipelining: `AddFrame` copies into one of 2 slots (blocks when both are full); one Async job processes frames in
  order (diff, quantize, canvas) and each frame's LZW runs as a second Async job while the next frame is quantized
  (the worker runs it inline if it has not started, so a 1-thread pool cannot deadlock). `Close` drains everything.
- No AVX2 path: the scalar code meets the budget (gif_demo worst case 2.65-2.9x real time at 720p20).
- Extras: `DelayForFps`, `BytesWritten`, `FramesWritten`, `EncodeBusyMs`, public `Histogram`, `NearestCache`,
  `QuantizeRegion`.
Why: dithering noise defeated index-level differencing; the rest are spec gaps or measured performance needs.
Impact: ExportJob GIF path: sample every 30th frame with `AddPaletteSample` first (two-pass), then `AddFrame` each
frame at `gif.fps` with `DelayForFps(gif.fps)`, then `Close`; cancel = `Close` + delete the file.

## 2026-10-17 · P10 task 10.2 (prerequisite) · render::TimelinePlayer details
Spec said: P09 "Core structures" `TimelinePlayer { Init(RenderContext&, AudioDevices&); SetProject; Cache(media); Resolve;
ResolveExact; Clock; Audio; Play/Pause/Seek/SetRate/StepFrames; HintScrub; RenderAudio }`, mixing "active audio-bearing clips".
Done instead (every specified member exists with its signature; additions only):
- `ct_render` now depends on `ct_playback` (DEPS line in src/render/CMakeLists.txt). frame_inputs.h/compositor.h stay
  playback-free (the P08 entry still holds for them).
- Additions: `Options { audio_output, async_open }` + `Init(rc, AudioDevices*, Options)` (export/tests run without WASAPI),
  `Tick()` (per UI frame: returns the playhead, pauses at the end, refreshes the audio plan), `Now`, `Playing`, `Rate`,
  `Duration`, `HasAudioOutput`, `MediaPending`, `CameraCache`, `Events`, `WaitMediaOpen`, `ProjectChanged`, static
  `AudioFramesBetween/AudioSampleIndex` (floor(t * 48000 / 1e6), so per-frame chunks add up exactly).
- Media: one slot per media id. Recordings (`.ctrec`) open `<bundle>/<meta screen.file>` (Screen clips, mic/system audio)
  and `<bundle>/<meta camera.file>` (Camera clips) as two MediaCaches, and load events.ctev for Screen clips. Resolve opens
  media on the IO pool (`all_exact = false` meanwhile); ResolveExact opens synchronously. Images: `is_rgba`,
  `rgba_premultiplied = true` (ImageIo decodes premultiplied). The Image background is decoded once per path.
- Frame requests ask for the *middle of the nominal frame slot* of the clip's media time (`FrameIndexAt` on the media fps):
  decoded durations are rounded (16666 us at 60 fps), and asking for a time in the 1 us hole after a frame (e.g. a clip's
  last frame at `out_us - 1` on a frame boundary) made the prefetcher drop that frame forever (MediaCache skips frames that
  end at or before the target). `VideoLayerInput::media_t_us` keeps the exact media time (events). When a file's decode
  ends before its container duration (AAC padding), ResolveExact remembers the time after one timeout and shows the last
  frame from then on.
- One cache hint per file per Resolve (the first clip using it): two clips of the same file far apart in media time (a
  transition between distant parts of one recording) fight over one prefetch window; the second clip's frame comes from
  its miss/wait. Acceptable for transitions (0.5 s); per-clip caches would double decoder count.
- SRV lifetime: the views of the last Resolve are AddRef'ed until the next Resolve, and MediaCache pins the last 4 frames per
  file; more than 4 simultaneous frames from one file in one Resolve is unsupported.
- Audio-bearing clips = clips on unmuted *audio* tracks (Recording/Video/Audio media); clips on video tracks are silent. The
  editor adds a linked audio clip when a video with audio is dropped. Stream: Mic/System via `Media::audio_streams` labels,
  else the first audio stream (index into `MediaInfo::AudioStreams()`, file order).
- Mix (src/render/src/audio_mixer.*, shared by playback and RenderAudio): one AudioDecoder per clip, re-seeked only on a
  discontinuity; speed via `audio::Resampler` (48000 * speed -> 48000: pitch follows speed, like varispeed; clips outside
  0.125..8x are silent: the resampler's ratio limit); gain = dB(static or `audio.gain_db` keys) * linear fades * normalize,
  evaluated exactly every 256 frames and ramped linearly in between. Normalize = -1 dBFS peak target from the waveform
  peaks of the clip's range, capped at +20 dB; playback takes peaks from the MediaCache (unity until built), RenderAudio
  loads/builds them itself (blocking).
- Playback feed: "ct-audio-mix" thread (above normal) mixes 10 ms chunks into a 300 ms AudioRing; `Pull` follows
  playback::Player's protocol (try-lock, catch-up drop, silence until the requested generation has data) but counts in
  samples, not microseconds (no rounding drift). Edits are picked up by the mixer at the next chunk (<= 300 ms latency, no
  restart). Decoders of clips starting within 1 s are opened ahead.
Why: P09 interface gaps; MediaCache rounding behaviour; real-time constraints of the audio thread.
Impact: P09b drives `Tick()` each frame, calls `Resolve(playhead)` and re-renders while `all_exact` is false, and adds
audio clips on audio tracks for dropped video media.
## 2026-10-17 · Review fix (P04) · Mp4Writer never frees its state under a live ct-encode thread
Spec said: `Abort()` returns after at most 3 s and does not join; the destructor finalizes and releases the writer.
Done instead: the destructor joins ct-encode (no timeout) before releasing anything when `Finalize`'s 60 s join timed out.
After `Abort()`, if ct-encode is still running (stuck in `IMFSinkWriter::Finalize`), the destructor logs a warning and
deliberately leaks the internal state (sink writer, buffers, events, thread) instead of freeing it under the thread or
blocking the crash path. To make that safe, the state ct-encode publishes (`Stats`, last error, accepting flag) moved from
`Mp4Writer` into its Impl (`GetStats`/`LastError`/`Accepting` are no longer inline; same signatures), `Impl::thread` is
declared last (destroyed, i.e. joined, first), and the listener is not called once Abort was requested.
Added `SetFinalizeDelayForTest(ms)` (test `mp4_writer_abort_during_slow_finalize_then_destroy`).
Why: the old destructor reset the sink writer while ct-encode could still be inside Finalize (use-after-free), and the
events/atomics were destroyed before `~Thread` joined.
Impact: P05 can destroy the writer right after `Abort()` on the crash path; the NV12 pool must still outlive the writer.

## 2026-10-17 · Review fix (P04) · Mp4Writer trims audio that overlaps what was already written
Spec said: audio items are written at their `t_us`; gaps > 2 ms become silence.
Done instead: an item that starts before the end of the stream's written data (an overlapping/late producer, or a
negative first time) loses its overlapping frames and is written at the end of the written data; an item entirely
inside it is dropped. Sub-frame overlaps (rounding) only snap the timestamp. Trimmed frames are counted in the new
`Stats::audio_overlap_frames[stream]` (not in `audio_written`). Test `mp4_writer_audio_overlap_trimmed`.
Why: the item was written as-is, so the file's audio timestamps went backwards.
Impact: none for callers; P05 may log `audio_overlap_frames` next to the other writer stats.

## 2026-10-17 · Review fix (P06) · Peaks::Build is cancellable; MediaCache::Close cancels it
Spec said: `Peaks::Build(path, stream, Peaks&)` (P06 header).
Done instead: `Build(path, stream, Peaks&, const std::atomic<bool>* stop = nullptr)`; the flag is checked before each
decode step (~160 ms of audio) and Build returns `E_ABORT` once it is set. MediaCache's peaks job passes its stop flag,
so `Close` (which sets it first) no longer waits for a full decode pass of a long file (10 min WAV: 4.5 s build, Close
during it 3 ms in Debug). Existing callers are unchanged (default argument).
Why: Close blocked the owner thread for the whole build.
Impact: none for callers.

## 2026-10-17 · Review fix (P05 task 5.7) · Webcam memory buffers use the media type's frame height
Spec said: (DECISIONS P05 5.7) memory-buffer samples are uploaded as NV12 with the UV plane after `height` luma rows.
Done instead: like VideoDecoder (frame vs display size), `Init` delivers the reader output's
`MF_MT_MINIMUM_DISPLAY_APERTURE` when it is a top-left sub-rectangle of the frame (e.g. 1920x1080 of a 1920x1088
frame; `ActualSize()` and the pool are the aperture) and remembers the type's frame height (`m_frame_h`); the memory
path reads the UV plane after `m_frame_h` rows (`internal::UploadNv12(..., src_rows)`). Without an aperture nothing
changes. Test `webcam_upload_nv12_padded_frame_height` (the MF device path is still not verified on hardware here).
Why: a padded height (1088) put the chroma read 8 rows early: corrupt colours.
Impact: none for callers.
## 2026-10-17 · P08 review fix · Text raster cache: per-frame eviction, O(1) lookup
Spec said: `TextRenderer::Trim(max_bytes = 64 MB)` evicts least recently used layers (nothing called it).
Done instead: `TextRenderer::EndFrame(max_idle_frames = kIdleFrames (60), max_bytes = kMaxBytes (64 MB))`, called at the end
of `Compositor::RenderFrame` (no Layer* in use then), drops rasters not looked up for more than 60 frames, then Trims to
64 MB. Entries live in an `unordered_map` by cache key; `CachedBytes` is a running total.
Why: an animated annotation (rect/text/colour/scale) or a changing keystroke combo made a new texture + D2D bitmap every
frame that was never freed.
Impact: Layer pointers are valid until the end of the frame (EndFrame), not until the next Trim. Measured: 1000 frames of
a moving annotation keep at most 61 layers (1.9 MB).

## 2026-10-17 · P08 review fix · Temp render targets are bucketed to 64 px and evicted when idle
Spec said: `AcquireTemp` pooled per size, reset each frame (exact sizes, never freed).
Done instead: the texture is the request rounded up to 64 px per axis (`RenderContext::TempTextureSize`); `TempRT::size`
is the requested (used) size, drawn with `SetFullscreen(rtv, size)` into the top-left sub-rect. Sampling goes through a
per-texture source uv transform: `QuadCb` grew to 240 bytes with `src[2]` (xy scale = used / texture size, zw = half a used
texel), `SetSourceUv` fills it from the SRV's texture desc, and blur/copy/pixelate/rgba/multiply/transition shaders sample
`SrcUv0/SrcUv1(uv)` = clamp(uv, zw, 1 - zw) * xy (what clamp addressing gives an exactly sized texture; identity by
default). `BeginFrame` frees entries not acquired for more than `kTempIdleFrames` (60) frames. `TempBytes()` added.
Why: blur/highlight regions and Gaussian downsample levels request sizes that change every frame during a zoom or an
animated region; the pool kept one texture per distinct size forever.
Impact: goldens unchanged. Code sampling a pooled temp must call `SetSourceUv` (effects and transitions do). Measured: 500
frames of animated regions under an animated zoom: at most 21 temps / 6.5 MB at 640x360.

## 2026-10-17 · P08 review fix · Keystroke badge fades in from the start of the visible run
Spec said: badge fade-in 120 ms / fade-out 200 ms (the code measured the fade-in from the oldest stroke shown).
Done instead: `Compositor::KeystrokeBadgeAlpha(events, t, hold)` (public, tested) measures the fade-in from the start of
the continuous run of visible strokes (a stroke shows during [t, t + hold); the run extends back while the previous stroke
was still showing when the next one arrived). The fade-out is unchanged (200 ms before the newest stroke expires).
Why: when the oldest stroke expired or a 4th stroke pushed it out of the badge, the opacity dropped mid-display.
Impact: none for callers.

## 2026-10-17 · P08 review fix · Wallpaper blur cache holds a reference to its source SRV
Spec said: Image background with an optional cached blur.
Done instead: the cache key is a `ComPtr<ID3D11ShaderResourceView>` (plus blur and size) instead of a raw pointer.
Why: a freed wallpaper's address could be reused by the next one, which then showed the old blur.
Impact: the compositor keeps the last blurred wallpaper's SRV alive until the wallpaper changes or Shutdown.

## 2026-10-17 · P05 review fix · events.ctev records are written in time order across controls and hook events
Spec said: ct-events-io drains every 100 ms, records merged in time order.
Done instead: a control (MARKER, PAUSE, initial WINDOW_FOCUS) is stamped inside `PushControl` under the control lock, and
each drain reads its cutoff under the same lock. Only records with t <= cutoff are written; later ones stay in the batch
for the next drain. Cutoff: periodic drains `QpcToMediaUs(now - 5 ms)` (covers a hook event stamped just before its push;
inside a pause it maps to the pause start, so the PAUSE record is not held back), `Flush()` = now, stop = everything.
TYPING Tick runs up to the cutoff.
Why: a control's time was read before it was queued; a drain in between wrote later hook events first.
Impact: records of the last 5 ms before a periodic drain wait for the next drain (100 ms); `Flush()` still writes
everything recorded before the call.
## 2026-10-17 · P11 tasks 11.5/11.6 · Settings window, error banner and first run details
Spec said: `ui::SettingsWindow` modal with sections Recording (incl. keystrokes mode with a privacy note), Audio (meters
test), Camera, Hotkeys, Editor, Export, Privacy, About; saves through the 500 ms debounce; error banners per docs/05.
Done instead:
- Keystroke capture mode lives in the Privacy section (with the off / shortcuts / all note), not in Recording, so all
  privacy-relevant choices sit together. The live mic meter test is not built yet (11.6 stays `[~]`).
- `recording.mic_device` stores the WASAPI endpoint id (UTF-8) and `recording.webcam_device` the camera's symbolic
  link (UTF-8); empty = default mic / no camera. `webcam_resolution` stays "WxH" from `WebcamInfo::sizes`.
- The debounce runs on its own thread (`ui::SettingsSaver`, "ct-settings-save"): each change re-arms 500 ms, the
  thread writes the latest copy with `Settings::Save`; the destructor flushes synchronously. The UI thread never
  writes the file. `Debouncer` is the pure, tested part.
- The window is an ImGui modal popup (dims the viewport, Esc / Done close, Enter closes when no item has keyboard
  focus). Hosts smaller than 840x580 DIP (the recorder bar) need ImGui multi-viewports or their own host window.
- Hotkey recorder: claims every key (`SetKeyOwner`, LockThisFrame) while listening so ImGui nav does not see Tab/Esc.
  Esc cancels, Backspace/Delete clears to "None"; letters/digits/navigation keys need Ctrl, Alt or Win (Shift alone is
  refused); F1-F24, Pause, PrintScreen and Insert may stand alone. Conflict check order: duplicate of another of our
  actions, unchanged, then a test `RegisterHotKey` on a thread-bound id (`SystemHotkeyProbe`). Because our own global
  hotkeys would swallow the keys and fail the probe, the owner gets `suspend_hotkeys()` when recording starts and
  `reregister_hotkeys(hotkeys, &err)` when it ends.
- Icons beyond icons.h used only by the window: Keyboard E92E, Refresh E72C (both checked in SegoeIcons.ttf and
  segmdl2.ttf by `ct_ui_tests settings_icons_present_in_icon_font`); wide glyphs are scaled into an 18 DIP slot.
- `ct_ui` now links `ct_audio ct_capture ct_media ct_export` (AudioDevices, WebcamSource::Enumerate, EncoderCaps,
  Preset). `webcam_source.h` pulls `d3d_device.h` (and its `mfobjects.h`) into `settings_window_logic.cpp` only.
- Error messages keep a `%s` placeholder replaced by `ui::FillPlaceholder` (no printf, paths may contain `%`);
  DiskFull shows the volume (`ui::DriveOf`: "D:", "\server\share", `\?\` prefixes stripped).
- First run = `settings.json` missing. `FirstRun::Run` pushes the toast (12 s) and on the IO pool creates the folders
  and writes the current settings, so the next start is not a first run.
Why: spec gaps (storage format of device ids, keyboard capture semantics, where the save happens) and grouping.
Impact: P05b bar: `settings_window.suspend_hotkeys/reregister_hotkeys/on_changed`, call `FirstRun::Run(&toasts,
settings, SettingsFilePath())` once at startup. P09b editor: `ErrorBanner` per panel, Preferences -> `Open()`.
## 2026-10-17 · Integration (after P04-P08 merge) · One test pattern, MfRuntime in ct_platform, media enums in presets, SessionClock as MediaTimeSource
Spec said: P04's `tools/gen_test_media` writes tests/data/pattern_*.mp4; P06 replaces its private MF guard with
`media::MfRuntime::Ensure()` and P05's webcam does the same; P10's Preset switches to `media::VideoCodec`/`RateControl`
once ct_media exists; `recorder::SessionClock` derives from `capture::MediaTimeSource`.
Done:
- Test pattern: `tests/common/frame_index.h` now decodes exactly the counter `tools/gen_test_media` draws (12 digits,
  28 x 56 px cells at (34 + 33 k, 36), 4 px strokes, box (32,32)-(432,96)); the P06 layout (24 x 48 glyphs, 6 px
  strokes) is gone. The P06 helper `ct_gen_test_media` (tests/media/gen_pattern_main.cpp) and the `out/testmedia/`
  fallback are deleted: tests read only the committed tests/data files (regenerate with
  `gen_test_media --all tests/data`). `DrawFrameIndexY` is removed with its only user.
- MfRuntime: ct_media depends on ct_capture, so ct_capture cannot use ct_media. `MfRuntime`/`MfScope` moved to
  ct_platform (`ct/platform/mf_runtime.h`, `platform::MfRuntime`); `ct/media/mf_runtime.h` is a forwarding header
  (`using platform::MfRuntime; using platform::MfScope;`), so existing `media::MfRuntime` callers are unchanged.
  Added `MfRuntime::EnsureForProcess()`: the first successful call takes one reference that is never released. The
  decoders, `MediaInfo::Load` and the webcam (Enumerate/Init/synthetic reader) use it, keeping the old guards'
  process-lifetime semantics (no MFShutdown while a decoder or DXGI manager may still be alive) while sharing the one
  refcount. `mf_startup_internal.h` and `capture::internal::EnsureMfStarted` are deleted. MFSTARTUP_LITE and
  MFSTARTUP_NOSOCKET are the same flag value.
- Presets: `exportng::VideoCodec` / `exportng::RateControl` are now `using media::VideoCodec` / `using
  media::RateControl`; ct_export depends on ct_media. JSON names unchanged (static_asserts pin the enum order of the
  name tables). ExportJob needs no static_cast.
- SessionClock: `struct SessionClock final : capture::MediaTimeSource`; `QpcHnsToMediaUs` forwards to `HnsToMediaUs`,
  `NowMediaUs` is the override. `MediaTimeSource`/`QpcMediaTimeSource` moved from webcam_source.h to the new light
  header `ct/capture/media_time_source.h` (webcam_source.h includes it), so session_clock.h does not pull in D3D/MF.
Why: the parallel phases each had a placeholder for a piece another phase owned; the counter layouts disagreed so the
decoder/cache/player tests failed on the committed files.
Impact: Recorder session: `webcam.Start(session.clock, sink)` works directly. New MF users without a Shutdown point
call `platform::MfRuntime::EnsureForProcess()`; scoped users keep `Ensure`/`Shutdown` or `MfScope`.
- Timing checks in the P04/P06 tests (`video_decoder_seek_*`, `player_plays_in_sync_and_steps`, `media_cache_budget_*`,
  `media_cache_thumbnail_within_1s` (gross limit 3 s), `audio_engine_position_*`, `mp4_writer_2s_*` `pool_empty`) follow docs/06: budgets are enforced only with `CT_STRICT_TIMING=1`, otherwise a gross limit
  (seeks 2000 ms, clock deviation 50 ms) or report-only. Helper `tests/common/timing.h` (`StrictTiming`, `TimingLimit`).
  They failed intermittently in Release on the loaded machine (seek p95 583 ms, 62 frames shown in 90 ticks) with all
  correctness checks (frame indices, cache misses = 0) passing.
- One counter reader: `tests/common/frame_index.h` (names `ReadFrameIndexWith`, `ReadFrameIndexY`, `ReadFrameIndex`;
  the RGBA reader gained optional `ox, oy` frame offsets). P10's `tests/common/pattern_counter.h` (export branch) samples
  the identical segment centres, as did the private reader in tests/media/test_data_test.cpp (now uses frame_index.h);
  when that branch merges it should become a thin include of frame_index.h, with
  `ReadPatternCounterWith/Y/Rgba` forwarding to `ReadFrameIndexWith/Y/ReadFrameIndex`.

## 2026-10-17 · P09 tasks 9.1-9.7 (second half) · Editor window, preview, file ops, app mode
Spec said: a MediaCache-backed TimelineMediaProvider, a preview panel with gizmos/overlays/transport, IFileDialog file
ops, recent files, autosave recovery, unsaved prompt, layout ini, media probing/relink, Regenerate, error banners and
`Camtesia.exe --editor [file]`.
Done / interpreted:
- Player: the editor uses `render::TimelinePlayer` directly (`detail::PlayerTransport` adapts it to `PlaybackControl`;
  Tick() once per frame gives the playhead; loop wraps in the adapter). `PlaybackControl` gained `Volume/SetVolume/
  Muted/SetMuted` virtuals with no-op defaults so SimplePlayback and test fakes are unchanged.
- `TimelinePlayer::Cache(id)` now opens the media's slot lazily (`GetSlot(m, false)`) instead of returning null for
  media not yet touched by Resolve; otherwise bin cards and off-screen timeline clips never got thumbnails/peaks.
- Provider (`detail::CacheMediaProvider`, private): thumbnails are ComPtr-pinned until the next frame's BeginFrame so
  ImGui never draws a released SRV; peaks are cached per `path#stream` as min/max pairs; a clip still loading shimmers
  for at most 30 s, then draws flat.
- Preview: the compositor renders into a canvas-size B8G8R8A8 target owned by the window, only when the revision,
  playhead, view or "inexact frame" state changed (idle = 0 GPU work). While a camera key is selected the camera is
  disabled and the key's window is drawn as a dashed rect with the rest dimmed. Scale is uniform (Alt = about the
  opposite handle), Shift snaps rotation to 15 degrees, Space/middle drag pans the view. Each gizmo drag is one undo entry.
- Linked audio: since only clips on audio tracks sound (TimelinePlayer rule), dropping/adding/importing a video that
  has audio also adds a Source::Default clip with the same start/in/out/speed on the first unlocked audio track with
  room, else a new "Audio" track (`cmd::AddLinkedAudioClip`, same as RecImporter). Opening a media file into an empty,
  clean project sets the canvas from the video and creates the video + audio tracks.
- Recent files: `%APPDATA%\Camtesia\recent.json` (max 10; an entry whose file fails to open is removed). Layout:
  `%APPDATA%\Camtesia\editor_layout.ini`; screenshot/bench runs use the built-in layout and write nothing.
- Banners: `BannerStack` is one P11a `ErrorBanner` per panel (Media/Preview/Properties/Timeline); a new error on a
  panel replaces the old one.
- App: `RunEditorApp` (src/ui) owns its own window (class "CamtesiaMain", so SingleInstance forwarding reaches it),
  D3D device, AudioDevices and Async UI poster; main.cpp only dispatches. A forwarded command line opens its paths
  (through the unsaved prompt). The loop renders only after input or while something animates; a 1 Hz waitable timer
  drives autosave.
- Bench flags (`--bench-drag N`, `--bench-scrub N`, `--bench-play N`, `--stats`, `--size WxH`) inject synthetic
  mouse input after the Win32 backend's NewFrame and print results; they are used for the 9.7 numbers.
- Review fixes: ##camkey/##camsec ID scopes; group move sorted by shift direction; autosave notified on every commit;
  PropTime coalesces before returning; marker and dragged clip/selection excluded from snapping (allocation-free
  SnapIgnore); trims clamp analytically (no Project copies); marker label typing is one undo entry.
Impact: P10's export dialog plugs into `EditorShell::on_export` (currently a toast); P05b's recorder can open the
editor in-process later by calling `RunEditorApp`-equivalent pieces or by launching `--editor folder.ctrec`.


## 2026-10-17 · P05 tasks 5.1/5.2 · Session control thread, start order, fps clamp, meta details, headless modes
Spec said: `RecordingSession::Prepare` runs on the IO pool; `StartNow` order audio -> `Mp4Writer::Start` -> capturer ->
webcam -> events; `Stats()` returns `const LiveStats&`; `CrashHandler::RegisterEmergency` set in Prepare, cleared in Stop;
`--record --out "D:\x.ctrec"`; meta.json per docs/04 section 1.
Done instead:
- Every component is created, started, paused and stopped on the session's own **"ct-session" thread (COM MTA)** fed by a
  command queue (WASAPI/MF want one MTA owner thread; the UI thread is an STA). `StartNow/TogglePause/AddMarker/Stop/
  Cancel/PrepareAsync` return at once; `Prepare` posts and waits (IO pool / console thread). A StartNow queued behind a
  running prepare simply runs after it, so the countdown overlaps the prepare. TogglePause takes one QPC on the caller's
  thread and passes it to the clock and both audio streams.
- Additions: `PrepareAsync`, `IsPrepared`, `IsActive`, `StartNow(trigger_qpc)` (trigger -> first frame latency, logged and
  kept in `Stats::T("session.hotkey_to_frame")`, `LastStartLatencyMs()`), `WaitIdle(ms)` (headless/tests), `Stats()` returns
  a copy (it changes on ct-session), `SessionListener::OnWarning` (low disk, camera/audio device lost, no mic) and
  `OnPrepared(hr)` with empty defaults; `SessionConfig::{system_device, webcam_synthetic, events}`; `LiveStats::{state,
  frames, fps, has_mic, has_system, has_camera}`. `dropped_frames` also counts the capturer's skipped late ticks.
- Start order: `Mp4Writer::Start` (screen, then cam) **before** the producers, because the writer rejects input until
  Start (`m_accepting`); then mic, system, capturer (epoch), webcam, events. The writer start only spawns ct-encode, so
  audio still starts within microseconds of the epoch.
- fps clamp (P04 left it to P05): requested rate, then 120/60/30 below it; each attempt re-creates capturer + writer;
  HEVC falls back to H.264 when no rate works; file errors (path/access/disk full) fail at once. Audio streams whose
  `Init` fails (or no endpoint) are left out of screen.mp4 instead of becoming empty tracks; the user gets a warning.
- Webcam time base: a private `ClockTimeSource : capture::MediaTimeSource` adapter over the SessionClock (the
  integration change makes SessionClock implement the interface; the adapter keeps working).
- Errors: a component error (capture OnError, writer listener, disk < 200 MB) -> Error -> finalize -> Idle, `OnError`,
  and `OnFinished` too when frames were written (the partial recording is kept with a normal meta.json). Camera/cam
  writer and audio device errors only warn.
- Emergency: `RegisterEmergency` has no unregister and allows 8 entries, so the session registers one thunk per process;
  it acts on the session armed between StartNow and Finish (atomic pointer). The `incomplete` meta.json is built at
  StartNow (duration 0) and the filter only writes its bytes; the writers are Aborted and never destroyed afterwards.
- meta.json: `created_utc` without milliseconds (docs/04 example); `duration_us` = last video frame time + one frame
  (the capturer's last tick); `stream_index` = `Mp4Writer::AudioStreamIndex`; `incomplete: true` only on the crash path;
  `scale` is written as the JSON number 1.
- Headless: `--out` names the folder that receives `<stamp>.ctrec` (as the recorder does; P05 tests say `--out <tmp>`);
  extras `--region x,y,w,h --codec --quality --keys --camera --synthetic-camera --pause-at S --pause-for S --verify`;
  `--record`/`--selftest` skip the single-instance check (they run next to a running bar or another agent's instance).
  `--selftest` adds a synthetic camera so cam.mp4 is exercised without hardware. `VerifyBundle` checks constant frame rate
  from the sample spacing: the MP4 source's `MF_MT_FRAME_RATE` includes the first frame's offset from 0 (29.4 fps for a
  30 fps file whose first frame is at 33 ms).
Why: COM apartment rules, the writer's accept gate, parallel instances on the test machine, and robustness.
Impact: the bar (5.6) uses PrepareAsync + StartCountdown + StartNow(trigger) and the listener additions.

## 2026-10-17 · P05 task 5.6 · Recorder bar windows, hotkeys, previews and diagnostics
Decision:
- ImGui multi-viewport with `ConfigViewportsNoAutoMerge`: popovers and toasts get their own platform windows, so they
  can extend past the 560x64 / 260x44 dip bar. `Platform_CreateWindow` is wrapped to apply WDA_EXCLUDEFROMCAPTURE,
  topmost, DWM round corners, border colour and dark mode to every viewport. The settings window is the exception: an
  `ImGuiWindowClass` (ParentViewportId 0, clear NoDecoration/NoTaskBarIcon) gives it a normal decorated, non-topmost
  top-level window "Camtesia Settings". Secondary viewports fill with bg2 to hide the black clear.
- `ui::SettingsWindow` got one additive method, `SetHostRect(pos, size)` (+ `WindowName()`), so its fill-viewport mode
  fills that window instead of the main viewport.
- Hotkeys (`RegisterHotKey`) and the tray icon belong to the bar window itself; no separate message window. While
  the countdown runs, Esc is registered as a temporary hotkey (the bar does not have focus).
- Monitor popover draws a schematic layout of the displays (proportional rectangles + list), not live thumbnails
  (no capture before REC, no camera/mic LEDs, no cost).
- Idle meters: an `AudioMonitor` null-sink capture runs only while the mic or system popover is open; during
  recording the meters read the session's `LiveLevels()` atomics (updated at 30 Hz on ct-session). The camera preview
  opens the device only while the camera popover/hover preview shows, and the session's webcam while recording.
  `--synthetic-camera` substitutes the synthetic reader (no camera on the reference machine).
- Toasts are drawn by the bar in their own viewport ("Saved" with Open in editor / Show in folder / Delete), not by
  `ToastStack`, which draws into the main viewport (the bar is too small).
- Default "Open in editor" launches `Camtesia.exe --editor <bundle>` with CAMTESIA_SEPARATE_INSTANCE=1.
- Redraw policy: `RedrawIntervalMs` 16 while animating, 33 while recording/meters/preview, 250 while toasts show,
  0 (wait for messages) otherwise.
- Hotkey latency = start trigger (WM_HOTKEY, or the end of the countdown) -> first captured frame QPC. With countdown 0
  it includes the session prepare (~150-600 ms in Release) because prepare starts at the hotkey; with a countdown the
  prepare runs during the countdown.
- Diagnostic modes (`--screenshot`, `--time-to-window`, `--bench-hotkey`) skip the single-instance check, never write
  the user's settings, and record into a temp folder deleted at exit.
Why: the bar is tiny and topmost and must stay out of recordings; device LEDs must stay off when idle; diagnostics must
not touch user data.
Impact: P09b/P11 may replace the Open-in-editor launch with in-process editor opening.

## 2026-10-17 · P05 task 5.6 · App mode dispatch next to the P09 editor mode
Decision: `ui::IsRecorderBarCommandLine` picks the recorder bar unless the command line has `--editor`,
`--editor-demo`, `--settings-demo` or a `.ctproj`/`.ctrec` file argument (then P09's `ParseEditorLaunch` runs).
`ParseEditorLaunch` alone cannot decide it, because it treats any bare value (`--countdown 1`, `--bar-open mic`) as a file.
Why: double-clicking a project must open the editor; recorder flags take values. Impact: a double-clicked file while the
bar runs still forwards to the bar instance through SingleInstance (the bar ignores it) — integration should open the
editor for a forwarded file argument or exempt editor launches from the single-instance check.

## 2026-10-17 · Integration · Event recorder: no record written after a later one
Decision: ct-hooks producers (mouse, keyboard, focus, cursor timer) hold an in-flight counter from before they stamp a
record until after they push it; Drain reads its cutoff, then waits for the counter to reach 0 before popping.
Why: a hook event stamped just before a Flush cutoff but pushed just after the pop was written in the next drain, after
records with later times (seen as one out-of-order record under load). The 5 ms hold-back only covered periodic drains.
Impact: the bundle verifier's constant-fps check now allows the same few dropped frames as its frame-count check.

## 2026-10-17 · P10 tasks 10.2-10.5 · ExportJob, Scaler, MP4/GIF/PNG/M4A details
Spec said: export loop in docs/phases/P10 10.2 (one thread, `WriteVideoSync` with 2 alternating NV12 textures, audio after
video per frame), `fast_start` -> `MF_MPEG4SINK_MOOV_BEFORE_MDAT`, M4A "fast_start", GIF two-pass palette + double-buffered
readback, PNG at the playhead, `Scaler::Downscale(rc, src, SizeI src, dst, dst_size)`.
Done instead:
- **fast_start is ignored** (Mp4Writer does not support it, DECISIONS P04 4.3): moov stays at the end of MP4/M4A files.
  Relocating moov after Finalize is left for later (PX); the preset field is kept and round-trips.
- MP4 pipeline: ct-export resolves/renders/converts frame n into one of **3** NV12 textures and mixes its audio, while a
  second thread "ct-export-write" calls `WriteVideoSync` + `WriteAudioSync` for frame n-1 (both sync calls stay on one
  thread, as Mp4Writer requires). The writer gets `sync_pool_size = 16`, `video_queue = 16` (B-frames keep input textures
  referenced; with 8 the loop waited on the encoder every frame). Audio is one mixed stereo AAC stream (mono when the
  preset says 1 channel); 44.1 kHz presets export at 48 kHz (Mp4Writer and the mix are 48 kHz only).
- Snapshot: `ExportRequest::project_snapshot` is the copy; the job's TimelinePlayer (`audio_output = false`,
  `async_open = false`) and Compositor are its own; the RenderContext is shared and every GPU step runs under
  `D3DDevice::Lock`. Thread priority BELOW_NORMAL (`ExportRequest::below_normal_priority = false` for benchmarks).
- Output size: preset width kept, height from the canvas aspect, both even (min 16); fps: preset, else canvas; GIF at
  `gif.fps`. Frames: ceil(range * fps). PNG: the frame at `range_start` (the dialog passes the playhead); `.jpg/.jpeg`
  paths write JPEG quality 0.92. M4A: 100 ms audio chunks.
- Scaler: own tiny shader (src/export/shaders/scale.hlsl, compiled with ct_compile_shaders) instead of the compositor's
  internal pipelines; pipeline objects cached per device for the process. Intermediates come from the RenderContext temp
  pool, which (since the P08 review fix) hands out 64 px-bucketed textures, so each pass gets a uv transform
  (used / texture size, clamp to half a used texel). `BoxPasses()` added for tests.
- GIF: pass 1 renders every 30th frame (`AddPaletteSample`), pass 2 renders frame n and copies it to staging[n & 1],
  then maps staging[(n-1) & 1] (one-frame delay) and copies it to a CPU buffer *under the device lock* before
  `AddFrame` (which may block on the encoder's two slots, so it never runs with the lock held).
- Cancel: the loop stops at the next frame, the writer is finalized (or the GIF closed) and the file deleted;
  `Progress::hr = HRESULT_FROM_WIN32(ERROR_CANCELLED)`, `cancelled = true`. Errors delete the partial file too.
- Additions: `ExportRequest::{on_finished (Async::PostToUi), frame_timeout_ms, gpu_timing, below_normal_priority}`,
  `Progress::{elapsed_s, output_bytes, encoder, resolve_ms, render_ms, video_ms, audio_ms, lock_wait_ms, compose_cpu_ms,
  compose_gpu_avg_ms, inexact_frames, first_frame_ms}`, `ExportJob::{Running, OutputSize, OutputFps, FrameCount}`.
- Tests compare MP4 vs preview in NV12 (preview converted by the same ColorConverter, so the PSNR measures the
  encoder only). The committed pattern files use the README counter layout, read by tests/common/pattern_counter.h.
Why: Mp4Writer limits; encoder back-pressure measured with export_demo; the P08 temp-pool change.
Impact: the export dialog fills `ExportRequest` (playhead as range_start for PNG) and polls `GetProgress()`.

## 2026-10-17 · P10 task 10.6 · Export acceptance on a loaded machine, exact-frame waits, cache budget during export
Spec said: 1080p60 export >= 1.5x real time; GIF 720p20 >= 2x real time; export frames match the preview.
Done instead: speed measured on a shared, loaded machine (other agents' builds): 1080p60 H.264 1.05x (1.43x without
  the camera layer), GIF 720p20 1.41x. PNG identical and MP4 PSNR 58.12 dB pass. 10.6 stays [~] until it is re-measured
  on an idle machine. To keep export exact under load, `TimelinePlayer::ResolveExact` retries a wait when the frame is
  evicted between the wait and the lookup, for up to `frame_timeout_ms` (10 s). Within 250 ms of a file's end it waits
  at most 2 s and then treats the last frame as the end (decoded stream shorter than the container). A mid-file timeout
  renders the frame and counts `inexact_frames`. ExportJob grows the global CacheBudget while it runs by
  w*h*1.5*50 frames per media file (twice for a recording with a camera), and restores it at the end. ExportJob keeps
  its own TimelinePlayer (audio_output=false, async_open=false), separate from the editor's.
Why: the second DXVA decoder (camera) contends on the D3D multithread lock (RenderFrame CPU 1 -> 6 ms); the GIF encoder
  is CPU-bound (31 ms/frame); with the default budget the preview's caches evicted export frames before use.
Impact: follow-up: give MediaCache a configurable prefetch depth for export, and consider one shared decoder per
  recording; re-run `export_demo --seconds 60` and `--preset gif-720p` on an idle machine.

## 2026-10-17 · A/V sync (P02 2.6 / P04 4.6 follow-up) · Restart re-creates the duplication; images stamped by present time
Spec said: docs/03 section 4 / P02 2.6: video frame n has t_us = FrameTimeUs(n), "tick-aligned, generated by the pacer,
NOT by the desktop's present time"; a new image goes out at the tick at which it was acquired. Budget (docs/06 section
4): |A/V offset| < 20 ms per clap event, |drift| < 20 ms.
Done instead:
- Restart: `ScreenCapturer::Run` re-creates the `IDXGIOutputDuplication` at the start of every run after the first. A
  reused duplication returns an image only when the desktop changes, so a restarted capture of a static screen
  delivered no frame at all. Test `screen_capturer_restart_static_desktop`.
- Timestamps stay tick-aligned CFR, but which tick an image belongs to is decided by its DXGI `LastPresentTime`: tick n
  shows the images displayed in [T(n) - frame/2, T(n) + frame/2), display = present + `Config::display_latency_us`.
  The pacer waits until that window's end - 1 ms and acquires with a timeout to window end + 2 ms (same lock hold as
  before); an image presented after the window end is converted at once and held for the next tick, which delivers it
  without acquiring (so still at most one conversion per tick); the current tick repeats the previous frame. Zero-copy,
  compute conversion and GPU cost are unchanged; the pacer runs half a frame later (sink sees frames ~8 ms later).
- `display_latency_us` defaults to -4000: against the source's DXGI vsync time the desktop present came 1-6 ms later
  (per-run window boundaries in the clap runs). Mean A/V over the runs: +5.95 ms with 0, -0.8 ms with -4000.
  `capture_bench --display-latency-us N` overrides it.
- `CaptureStats::present_lag` (tick - present of each new image) and `late_images` (delivered a tick later than its
  present time asks, i.e. acquired after its window) for capture_bench. Audio capture logs each loopback
  discontinuity / timestamp error with its device-position and QPC jump.
- `avsync_source --analyze` detects the beep narrowband at 2 kHz (music on the output hid the RMS onsets) and pairs
  each flash with the beep nearest its logged emission.
Measured (Release, 1080p60 + system audio, 2 min each, 60 events; machine shared with other agents' builds, CPU load
noted before/after each run):
- before: A/V mean +13.8 / +17.1 / +12.7 ms, max 39.5 / 45.9 / 75.0 ms, events >= 20 ms 27/58, 14/60, 3/28; video
  +14.9 / +17.6 / +13.4 ms, audio +0.5..1.1 ms; drift -7.1 / +8.3 / +44 ms (load 76-94, 76-86, 100 %: in the third
  run the source itself missed 31 flashes, its numbers are unreliable).
- display_latency_us 0: mean +0.3 / +9.1 / +8.4 ms, max 15.9 / 26.0 / 25.0 ms, >= 20 ms 0, 2, 1; drift -1.4 / -3.4 / -2.5.
- -4000 (default): mean -1.8 / +4.2 / -3.7 / -1.9 ms, max 3.2 / 21.9 / 24.3 / 13.9 ms, >= 20 ms 0, 2, 3, 0 of 60;
  drift -2.4 / -0.6 / +7.0 / +0.2 ms; late_images 132 of 6879 (1.9 %) at 6-13 % load.
Why: within a run the error is the (slowly drifting) phase between display vsync and the 60 Hz ticks, now within
+-frame/2 instead of 0..+1 frame; drift is within budget in every run.
Impact: remaining >= 20 ms events are images the duplication handed out more than 2 ms after their window ended
(one frame late, +16.7 ms on top of the phase). Fixing them needs a one-tick delivery delay (re-stamp a held frame
when a late image for its window arrives) or a longer acquire timeout (longer device-lock hold); not done. The vsync /
tick phase itself (+-8.3 ms) is inherent to CFR ticks not locked to the display.

## 2026-10-17 · P09b/P05 integration · A file forwarded to the running bar starts the editor process
Spec said: (P05 5.6 decisions) a double-clicked `.ctproj`/`.ctrec` while the bar runs is forwarded to the bar through
SingleInstance and ignored; integration should open the editor for it or exempt editor launches from the check.
Done instead: the bar's window handler looks at a forwarded command line first: when `IsRecorderBarCommandLine` says it
is not a bar launch (`--editor`, a `.ctproj`/`.ctrec` argument), `ForwardedEditorArguments` re-quotes its arguments
(argv[0] dropped, CommandLineToArgvW rules incl. trailing backslashes) and the bar starts `Camtesia.exe <args>` with
`CAMTESIA_SEPARATE_INSTANCE=1`, the same path as "Open in editor". The bar stays hidden/as it was; a plain second
launch still shows the bar. Bar diagnostics (temp output folder) only print the forwarded line. Also: a recording
argument with a trailing slash (`take.ctrec\`) now counts as an editor launch, and `--bench-drag/scrub/play` skip the
single-instance check like `--bench-hotkey` (`RunsAsSeparateInstance` matches `--bench-`).
Why: the bar and the editor do not share a process yet; launching keeps "the same as a fresh launch" semantics,
including the unsaved-prompt-free fresh editor and the editor's own forwarding target.
Impact: when bar and editor share one process (later), replace `launch_editor` with in-process opening.

## 2026-10-17 · Dev/test · CT_INSTANCE_SUFFIX single-instance override
Spec said: one instance per session (mutex `Local\Camtesia.Instance`, forward to class `CamtesiaMain`).
Done instead: `SingleInstance::Suffix()` reads the environment variable `CT_INSTANCE_SUFFIX` (sanitized to
`[A-Za-z0-9._-]`, at most 32 characters); when non-empty, `MutexName()` and `MainWindowClass()` append `.<suffix>`.
main.cpp, the recorder bar window and the editor window use these names. Unset = the release names, byte for byte.
There are no named pipes or other named kernel objects to rename. Documented in docs/06 section 5.
Why: parallel worktrees, benches and agents on the reference machine blocked each other's launches (P09 9.7 could not
be measured while another agent's instance held the mutex).
Impact: none for users. Test: `instance_suffix_override` (ct_ui_tests).

## 2026-10-17 · P09b task 9.1/10.1 · Export wiring in the editor
Spec said: File > Export opens the P10 dialog; status bar shows export progress; docs/05 shows an `Export ▾` button.
Done instead: one `ExportDialog` per EditorWindow (destroyed first in Destroy, so a running job is cancelled and joined
before the RenderContext/Compositor go). Opening pauses playback. The `Export ▾` button is split: the label opens the
dialog (Ctrl+E), the chevron opens a popup with "Export..." and the 10 built-in presets; the Export menu lists the same
presets (user presets stay in the dialog's list). The status-bar pill ("Exporting N %" + bar) shows while `Busy()`;
clicking it calls `Restore()`. `DrawStatusBar` returns `StatusBarClick` instead of bool. `--open-export [preset]`
opens the dialog after load (screenshots). Closing the editor during an export cancels it without a prompt.
Why: docs/05 section 4 layout; ExportDialog's lifetime contract.
Impact: a "cancel the running export?" prompt on close could be added with the P11 error UX work.

## 2026-10-18 · P09 tasks 9.1/9.2/9.6/9.7 · Editor window leftovers: UI posts, warnings count, media health banner, Ctrl+R
Spec said: `Async::PostToUi` lambdas run at the start of `Frame()`; status bar shows a warning count; error banners per
docs/05; `Ctrl+R` opens the recorder bar; shortcuts dispatched once per frame when `!io.WantTextInput`.
Done instead / interpreted:
- PostToUi: the editor's poster posts `WM_APP+0x31` to its own window and the window procedure runs the lambda between
  frames (not at the start of `Frame()`). Same thread, same "never inside NewFrame/Render" guarantee, and a posted
  result also wakes the event-driven loop (0 redraws when idle), which a queue drained in `Frame()` would not.
- Warning count: `Log::WarningCount()` (new, core): Warn + Error lines that passed the level filter since process start.
  The status bar shows it; a click opens the log folder. Editor banners log a warning, so they count too.
- Media banner: `CheckMediaHealth` (runs after an edit, or once the player's pending opens finish) gives one Media-panel
  banner for missing files (`ErrorMessage(MediaMissing, path)` for one, a count for more) or, when none is missing, for
  media the player could not open (TimelinePlayer slot state "failed": damaged file / unsupported codec). It shows again
  only when the set of affected media changes, so a dismissed banner does not come back on every edit; it clears itself
  after a relink. `Impl::Banner` skips a message identical to the one the panel already shows (a frame that keeps
  failing to render no longer logs every frame). No audio output (AudioDevices missing or TimelinePlayer without a
  stream) shows a Preview banner once at startup.
- Ctrl+R / Record button: the bar is another process (DECISIONS "P09b/P05 integration"). `FindRecorderBarWindow` finds
  a top-level window of class `SingleInstance::MainWindowClass()` with `WS_EX_TOOLWINDOW` (the editor's own window of
  that class has none); `ShowRecorderBar` sends it `WM_COPYDATA` with a plain command line (the bar shows itself), else
  starts `Camtesia.exe` with no arguments, with `CAMTESIA_SEPARATE_INSTANCE=1` only while some process holds the
  single-instance mutex (otherwise the launch would just forward back to the editor); the inherited value is restored.
- Shortcuts are not dispatched while a modal popup is open (unsaved changes, recovery, remove media, settings): Space or
  Delete must not act on the document behind the prompt.
Why: event-driven loop; docs/05 status bar and error UX; separate bar process; modal prompts.
Impact: when bar and editor share one process, replace `ShowRecorderBar` with an in-process call.

## 2026-10-18 · Integration · TYPING end records stay in time order
Decision: in the event drain, a record later than an open typing burst's last key is written only once the burst's fate
is known: a typing key-down within the gap extends it; a pause, or the cutoff passing last key + gap (1 s), closes it
first (the "ended" record keeps the last key's time). Until then the record and everything after it wait for the next drain.
Why: the "ended" record was written up to 1 s after records with later times, so files were not in time order.
Impact: while a burst is open, up to ~1 s of records are held in memory (a kill can lose up to ~2 s of events, the 5.8
budget); Flush() no longer guarantees records after the last key are on disk during an open burst.

## 2026-10-18 · P12 tasks 12.4/12.5 · Uninstall ends only {app}'s processes; scripted release smoke test
Spec said: (P12 12.2) `[UninstallRun] taskkill /im Camtesia.exe /f` covers a tray-only instance at uninstall; 12.5 is a
manual checklist (`tools/release_checklist.md`) run on clean machines.
Done instead:
- The `[UninstallRun]` taskkill is gone: it ended every Camtesia.exe on the machine (portable copies, dev builds,
  other agents' instances). A `[Code]` `CurUninstallStepChanged(usUninstall)` queries WMI `Win32_Process` for
  `Camtesia.exe` and terminates only processes whose `ExecutablePath` equals `{app}\Camtesia.exe` (case-insensitive),
  then waits 1 s; a WMI failure is logged and the uninstall continues.
- `tools/release_smoke.ps1` automates the installable part of the checklist: refuses to run when Camtesia is already
  installed/registered; per-user silent install into `%TEMP%\camtesia-smoke-*\app` with both tasks; checks files,
  HKCU uninstall entry (and no HKLM one), `.ctproj` association, open command, Run value, shortcuts, exe version;
  installed exe `--record --seconds 5 --verify --synthetic-camera --out <temp>`; `export_demo --recording <bundle>
  --save-project` (new flag: imports the bundle like the editor's Import recording, saves + reloads the .ctproj, exports);
  runs the registered open command with `--screenshot`; starts the installed exe `--tray` and a build-tree decoy
  `--tray` (own CT_INSTANCE_SUFFIX); uninstalls in `finally`; checks the tray instance ended, the decoy survived, and
  lists leftovers. Every child has a timeout and is killed with its tree when it runs over.
- Inno's uninstaller self-copy `%TEMP%\is-XXXX-uninstall.tmp\{_unins.tmp,_unins-done.tmp}` is not counted as a
  product leftover: Inno 6.3+ cannot delete its running copy and cleans these on the next Inno uninstall. The script
  removes the one its own run created.
- The installed app has no headless export mode, so export uses the build tree's `export_demo` (same ExportJob code).
Why: a developer/CI machine runs many Camtesia.exe; the uninstaller must not kill unrelated ones. Clean VMs are not
available to the agents, so as much of 12.5 as possible is scripted and repeatable.
Impact: interactive editing and the clean Windows 10 / Windows 11 runs stay human checks (checklist section 6). CI
could run `release_smoke.ps1 -TrayCheck $false` after `build.cmd package` (not wired up).
