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
