# Contributing to super4vid

This page is for developers and coding agents who join the codebase. It summarizes the rules; the full specs are
in [MASTER_PLAN.md](MASTER_PLAN.md) and [docs/](docs/). When this page and a spec disagree, the spec wins.

## 1. Prerequisites

| Tool | Version | Install |
|---|---|---|
| Visual Studio 2022 Build Tools | 17.14+ (MSVC 14.44+), C++ workload, Windows 11 SDK 10.0.26100 | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.Windows11SDK.26100"` |
| CMake + Ninja | CMake 3.25+ (3.29+ recommended), Ninja 1.11+ | Included in the workload above |
| Git | 2.40+ | `winget install Git.Git` |
| Inno Setup | 6.3+ (only for `build.cmd package`) | `winget install JRSoftware.InnoSetup` |
| Graphics Tools (optional) | - | `DISM /Online /Add-Capability /CapabilityName:Tools.Graphics.DirectX~~~~0.0.1.0` (admin). Enables the D3D11 debug layer in Debug builds. |
| Windows Performance Toolkit (optional) | ADK 10.1.26100 | `winget install Microsoft.WindowsADK`, select WPT |
| Python 3 (optional, dev tools only) | 3.11+ | `winget install Python.Python.3.12` |

Nothing needs to be on `PATH`. `build.cmd` finds Visual Studio 2022 with `vswhere.exe` and runs `vcvars64.bat`.
Details: [docs/01-toolchain-and-build.md](docs/01-toolchain-and-build.md).

## 2. Build

```
build.cmd doctor     # tool versions, SDK, Inno Setup presence
build.cmd debug      # configure + build Debug   -> out/build/ninja-debug/bin
build.cmd release    # configure + build Release -> out/build/ninja-release/bin  (default)
build.cmd test       # build both configs and run ctest for both
build.cmd package    # release build + installer + portable zip + symbols zip + SHA256SUMS.txt -> out/dist/
build.cmd clean      # delete out/
```

`build.ps1` forwards to `build.cmd`. Both configs must build with zero warnings (`/W4 /WX`).

**Low-RAM machines.** MSVC with `/MP` and Ninja can start one compiler per core, and each can take 1 GB or more.
Limit parallelism before building:

```
set CMAKE_BUILD_PARALLEL_LEVEL=2
build.cmd release
```

(PowerShell: `$env:CMAKE_BUILD_PARALLEL_LEVEL = "2"`.) `cmake --build` passes this to Ninja as `-j`. Use it
whenever several agents compile on one machine.

`build.cmd doctor` only looks for Inno Setup in `%ProgramFiles(x86)%`. A per-user winget install shows "MISSING" there,
but `tools/package.ps1` still finds it in `%LOCALAPPDATA%\Programs\Inno Setup 6`.

Useful app flags (Release or Debug exe): `--stats` (per-second counters on a console), `--time-to-window` (startup
time, then exit), `--crash-test` (writes a dump), `--tray` (start hidden). Set `CT_LOG=trace` for verbose logs.
Tool executables: `capture_bench.exe`, `audio_bench.exe` (more arrive with later phases).

## 3. Repository map

```
MASTER_PLAN.md      scope, non-negotiables, architecture, phases, agent protocol, budgets, defaults
PROGRESS.md         task checklist; the only place task status lives
DECISIONS.md        append-only log of every deviation from the specs
CMakeLists.txt  CMakePresets.json  build.cmd  build.ps1
cmake/              CtModule.cmake (ct_add_module, ct_add_test, ct_compile_shaders), version.h.in
docs/               01 toolchain · 02 conventions · 03 architecture · 04 file formats · 05 UI design
                    06 testing and performance · 07 API reference map · phases/P00..P12, PX · user-guide.md
src/<module>/       include/ct/<module>/*.h (public), src/*.cpp, shaders/*.hlsl (if any)
src/test/           ct_test harness (header + one .cpp)
src/app/            super4vid.exe: main.cpp, app.rc, app.manifest.in, resource.h
third_party/imgui/  Dear ImGui docking branch (the only third-party code; never edit it)
tests/<module>/     one test executable per module; tests/data/ for fixtures and goldens
tools/              package.ps1, make_icon.py, capture_bench/, audio_bench/
installer/          super4vid.iss, LICENSE.txt
.github/workflows/  build.yml (build, test, package; release on v* tags)
assets/             icon.ico (presets/*.json arrive with P10)
```

## 4. Modules and dependency order

Each `src/<module>` is a static library `ct_<module>`. Dependencies only point downward:

```
core <- {events, platform} <- {capture, audio, media} <- {recorder, playback} <- {project, render} <- export <- ui <- app
```

| Module | Owns |
|---|---|
| `core` | strings, time, log, error, containers, threads, json, fs, config, trace |
| `events` | `.ctev` reader/writer, `EventLog` time queries. Pure, depends on `core` only. |
| `platform` | Win32 window, DPI, D3D11 device, COM/MF runtime, hotkeys, hooks, tray, paths, crash dumps |
| `capture` | displays, desktop duplication, BGRA->NV12 conversion, cursor tracker, webcam, capture pacing |
| `audio` | WASAPI capture/loopback/render, resampler, mixer, meters, device notifications |
| `media` | encoders, MP4 sink writer, source reader, media info, GIF encoder, image IO |
| `recorder` | recording session state machine, event recorder, bundle writer |
| `playback` | decoder cache, prefetch, audio engine, clock |
| `project` | model, serialization, edit ops, undo, autosave, import, auto-zoom. Depends on `core` and `events` only. |
| `render` | compositor, GPU passes, D2D text, cursor renderer, effects |
| `export` | export jobs and presets (namespace `ct::exportng`) |
| `ui` | ImGui shell, theme, widgets, timeline, panels, recorder bar, overlays |
| `app` | `wWinMain`, command-line modes, single instance |

Extra rules: `ui` never includes Media Foundation headers; `render` and `playback` never include ImGui; `project`
includes nothing Windows-specific. See [docs/03-architecture.md](docs/03-architecture.md) for threads, clocks and
data flow.

## 5. Agent protocol (MASTER_PLAN section 7, in brief)

1. **Read order each session:** `MASTER_PLAN.md` -> `PROGRESS.md` -> your phase file -> only the docs that phase
   file lists under "Read first". Do not load the whole `docs/` folder.
2. **Take the first unchecked task** of your phase. Do not reorder tasks. Do not start the next phase before every
   acceptance criterion of the current one is ticked.
3. **Per task:** read the interface skeleton in the phase file -> implement -> `build.cmd debug` and
   `build.cmd release` green with zero warnings -> `build.cmd test` green -> do the "Manual check" -> commit and tick
   the box in `PROGRESS.md` in the same commit.
4. **Never:** add a dependency, disable a warning globally, use exceptions, allocate on capture/audio hot paths after
   start, call blocking Media Foundation or file APIs on the UI thread, or leave a `TODO` without a matching unchecked
   item in `PROGRESS.md`.
5. **When the spec cannot be followed:** try the documented fallback in the phase file first. Otherwise pick the
   closest default from MASTER_PLAN section 9, implement it, and add a `DECISIONS.md` entry.
6. **When a Windows API fails twice:** log the `HRESULT` with `ct::HrToString`, check the D3D11 debug layer or MF
   trace, read the page listed in [docs/07-api-reference-map.md](docs/07-api-reference-map.md), verify every flag,
   retry. Only then record a decision.
7. **Headers are contracts.** Do not change a public header signature from an earlier phase without a
   `DECISIONS.md` entry.
8. **A phase is done when** all tasks are ticked, all tests pass in Debug and Release, every acceptance number is
   measured and written next to its criterion in `PROGRESS.md`, there are no new warnings, `DECISIONS.md` is up to
   date, and a Debug run shows no debug-layer errors and no leaked COM objects at exit.

`PROGRESS.md` legend: `[ ]` not started · `[~]` in progress (one per agent at a time) · `[x]` done · `[-]` dropped
(needs a `DECISIONS.md` entry). Steps only a person can check are marked `HUMAN:`.

## 6. Conventions highlights

Full rules: [docs/02-conventions.md](docs/02-conventions.md).

**Language.** C++20, MSVC. No exceptions, no RTTI, no `std::iostream`/`std::regex`/`std::locale`/`std::thread`.
COM pointers are `Microsoft::WRL::ComPtr<T>`. Ownership is `std::unique_ptr` or values; raw pointers never own.
No global constructors with side effects.

**Naming.**

| Thing | Style | Example |
|---|---|---|
| Types, functions | PascalCase | `Mp4Writer`, `AcquireFrame()` |
| Variables | snake_case | `frame_index` |
| Members | `m_` + snake_case | `m_device` |
| Constants | `k` + PascalCase | `kMaxTextures` |
| Macros | `CT_` prefix | `CT_RETURN_IF_FAILED` |
| Files | snake_case | `desktop_duplicator.cpp` |
| Time variables | unit suffix | `t_us`, `duration_100ns`, `period_ms` |

**Errors.** Three function shapes:

```cpp
HRESULT Foo();          // talks to COM / D3D / MF / WASAPI
bool    Bar(Err* err);  // logic; on false, *err holds a user-presentable message
void    Baz();          // cannot fail
```

Use `CT_RETURN_IF_FAILED`, `CT_LOG_IF_FAILED`, `CT_RETURN_IF_NULL`, `CT_RETURN_FALSE_IF`. Never ignore a
`FAILED(hr)`. `CT_ASSERT` is Debug only, `CT_VERIFY` also runs in Release, `CT_FATAL` writes a dump and exits (for
programming errors only).

**Logging.** `CT_LOG_TRACE/DEBUG/INFO/WARN/ERROR`, printf-style, UTF-8 (`%ls` for wide strings). The logger never
blocks producers. Never log per frame at INFO; hot paths use `CT_TRACE_SCOPE("Name")` (ETW TraceLogging).
Logs go to `%LOCALAPPDATA%\super4vid\logs\` and `OutputDebugString`.

**Threads.** Create threads with `ct::Thread` (name, priority, optional MMCSS class). One owner thread per resource;
others talk to it through `SpscQueue<T>` or a message to the UI thread. `ct::SrwLock` for rarely written shared
state, `std::atomic` for flags and counters. No mutex, allocation, INFO logging or foreign COM calls on capture,
encode or audio threads after `Start()`. The UI thread never blocks longer than 2 ms; slow work goes to
`ct::Async::Run` and returns via a posted message. No `Sleep(1)` loops.

**Time.** `int64_t` microseconds (`t_us`) everywhere, from QPC relative to the session epoch. 100 ns units only inside
`ct_media` at Media Foundation calls. Video frames are tick-aligned: frame `n` is at `n * 1'000'000 / fps`.

**Memory.** Pools for every per-frame object; pool exhaustion is a counted drop, never an allocation. Debug exit must
show a clean CRT leak check and `ReportLiveDeviceObjects`.

**Strings and paths.** UTF-8 `std::string` inside; convert at the Win32 boundary with `ct::Utf8ToWide` /
`ct::WideToUtf8`. User-visible text lives in `src/ui/include/ct/ui/strings.h`.

**Performance.** Budgets in MASTER_PLAN section 8 are acceptance criteria. Every optimization commit states the
metric with before and after numbers.

## 7. Tests, goldens and benchmarks

The harness is `src/test/include/ct/test/ct_test.h` (target `ct_test`). No third-party framework.

| Macro | Runs |
|---|---|
| `CT_TEST(name)` | Always. Unit tests. |
| `CT_TEST_DEVICE(name)` | Needs GPU, audio or webcam. Prints `SKIPPED` when `CT_CI` is set or the hardware is missing. |
| `CT_TEST_GOLDEN(name)` | Compares an image with `tests/data/golden/<name>.png`. |
| `CT_BENCH(name, n)` | Only with `--bench`. Prints ops/s and ns/op. |

Checks: `CT_CHECK`, `CT_CHECK_EQ`, `CT_CHECK_NEAR`, `CT_CHECK_HR`, `CT_REQUIRE` (stops the test), `CT_SKIP`,
`CT_FAIL`. Runner flags: `--filter <substr>`, `--list`, `--bench`, `--repeat N`, `--update-golden`, `--verbose`.
The exit code is the number of failures.

- **One executable per module** (`ct_core_tests`, `ct_project_tests`, ...), registered with `ctest`. Tests run with
  `tests/` as the working directory, so `tests/data/...` paths work.
- **Run one module:** `out\build\ninja-release\bin\ct_project_tests.exe --filter autozoom`, or all of them with
  `ctest --preset ninja-release`.
- **CI behaviour:** `CT_CI=1 ctest --preset ninja-release` runs unit and fuzz-lite tests and skips device tests.
- **Goldens** pass when the maximum channel difference is <= 2 and PSNR >= 45 dB (phase files may relax this for
  text). On failure, `<name>.actual.png` and `<name>.diff.png` are written next to the golden. Regenerate with
  `--update-golden` on the reference machine, look at every changed image, and commit it with a reason. Golden tests
  need a GPU, so they are skipped in CI but **required** locally. Data fixtures such as `tests/data/autozoom/*.json`
  use the same flag.
- **Fuzz-lite:** random JSON, random `.ctev` bytes and random edit sequences, 10,000 iterations; must not crash and
  must keep invariants.
- **Test data** in `tests/data/` stays under 20 MB in total and is listed in `tests/data/README.md`.
- **Measurements** for budgets (startup, idle redraws, CPU/GPU, drops, A/V sync) follow
  [docs/06-testing-and-performance.md](docs/06-testing-and-performance.md) section 5. Numbers come from Release
  builds without the debug layer.

## 8. How to add things

### A module

1. Create `src/<name>/CMakeLists.txt`:
   ```cmake
   ct_add_module(<name>
     SOURCES src/foo.cpp src/bar.cpp
     DEPS ct_core ct_platform)        # only modules below it in the dependency order
   ```
2. Public headers go in `src/<name>/include/ct/<name>/`, include as `<ct/<name>/foo.h>`. Start each header with a
   2-5 line comment: what it does, thread model, lifetime.
3. Add `add_subdirectory(src/<name>)` to the root `CMakeLists.txt` after its dependencies.
4. Use namespace `ct::<name>`.

### A test

1. Create `tests/<name>/CMakeLists.txt`:
   ```cmake
   ct_add_test(ct_<name>_tests DEPS ct_<name> SOURCES
     foo_test.cpp
     bar_test.cpp)
   ```
2. Add `add_subdirectory(<name>)` to `tests/CMakeLists.txt`.
3. Write tests with `CT_TEST(...)`. `ct_test` supplies a default `main()` that calls `ct::test::RunAll`; define your
   own `main` only when the module needs setup first (see `tests/capture/main.cpp`).
4. For a new test file in an existing module, add it to that module's `SOURCES` list.

### A shader

1. Put the HLSL file in `src/<module>/shaders/`.
2. Compile it at build time in the module's `CMakeLists.txt` (fxc must be on `PATH`, which `build.cmd` ensures):
   ```cmake
   ct_compile_shaders(ct_<module> SHADERS
     shaders/blur.hlsl:ps_5_0:ps_main
     shaders/fullscreen.hlsl:vs_5_0:vs_main)
   ```
3. Each `file:profile:entry` becomes `<build>/shaders/<stem>_<entry>.h` with a byte array `g_<stem>_<entry>`.
   Include it from a `.cpp` and create the shader once in `Init()`, never per frame.

### A deviation

See the next section.

## 9. The DECISIONS.md rule

Every deviation from `MASTER_PLAN.md` or `docs/` gets an entry in [DECISIONS.md](DECISIONS.md), appended at the
bottom, in the same commit as the change. Do not silently diverge. Format:

```
## YYYY-MM-DD · Pxx task x.y · <short title>
Spec said: ...
Done instead: ...
Why: ... (include the HRESULT / behaviour observed, OS build, GPU)
Impact: ... (which later phases/headers are affected)
```

This includes interface changes to a public header from an earlier phase, dropped tasks (`[-]`), and missed budgets.

## 10. Commits

- One commit per task: `Pxx: <what> (task x.y)`, for example `P04: NV12 texture pool with reuse check (task 4.3)`.
- The body says what changed and how it was verified (test names, measured numbers).
- Tick the task in `PROGRESS.md` in the same commit.
- Fixes found in review: `Pxx: <what> (review fix)`. Documentation-only changes: `docs: <what>` or `docs/04: <what>`.
- Never commit `out/`, `*.user`, recordings, or anything over 2 MB except `assets/icon.ico` and the test media listed
  in `tests/data/README.md`.
- `main` always builds in both configs.

## 11. Releases

The release process is in [docs/phases/P12-installer-release.md](docs/phases/P12-installer-release.md) sections
12.1 and 12.5. In short: bump `project(super4vid VERSION x.y.z)` in the root `CMakeLists.txt` (everything else
derives from it), update `CHANGELOG.md`, run `build.cmd clean`, `build.cmd test` and `build.cmd package`, run the
checklist on a clean Windows 11 machine and a Windows 10 VM, then push a `vX.Y.Z` tag. CI builds, tests, packages,
checks `SHA256SUMS.txt` and publishes the GitHub release.
