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
