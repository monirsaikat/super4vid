# 01 · Toolchain and build

Everything about installing tools, the CMake layout, flags, and the one-command build. P00 creates these files
exactly as written here; later phases only add `add_subdirectory` lines and module `CMakeLists.txt`.

## 1. Required tools

| Tool | Version | Install (PowerShell, as the normal user) | Verify |
|---|---|---|---|
| Visual Studio 2022 Build Tools | 17.14+ (MSVC 14.44+) | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.Windows11SDK.26100"` | `build.cmd doctor` |
| Windows 11 SDK | 10.0.26100 | included above | `fxc` on PATH after vcvars |
| CMake + Ninja | CMake 3.29+, Ninja 1.11+ | included in the "C++ CMake tools for Windows" component of the workload above | `cmake --version`, `ninja --version` |
| Git | 2.40+ | `winget install Git.Git` | `git --version` |
| Inno Setup | 6.3+ | `winget install JRSoftware.InnoSetup` | `"%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"` exists |
| Graphics Tools (optional feature) | - | `DISM /Online /Add-Capability /CapabilityName:Tools.Graphics.DirectX~~~~0.0.1.0` (admin) | D3D11 debug layer works in Debug builds |
| Windows Performance Toolkit (optional, P11) | ADK 10.1.26100 | `winget install Microsoft.WindowsADK` then select WPT | `wpr -status` |
| Python 3 (optional, dev tools only) | 3.11+ | `winget install Python.Python.3.12` | `python --version` |

Reference machine state found on 2026-10-17: Build Tools at
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` with MSVC `14.44.35207`, SDKs `10.0.19041.0` and
`10.0.26100.0`, CMake and Ninja under `Common7\IDE\CommonExtensions\Microsoft\CMake`. Inno Setup was **not** installed.

Nothing is on `PATH` by default. `build.cmd` locates Visual Studio with `vswhere.exe` and calls `vcvars64.bat`,
which puts `cl`, `link`, `cmake`, `ninja`, `fxc`, `rc` and `mt` on the PATH for that shell.

## 2. One-command build

```
build.cmd doctor     # prints tool versions, SDK version, Inno Setup presence
build.cmd debug      # configure + build Debug  -> out/build/ninja-debug/bin
build.cmd release    # configure + build Release -> out/build/ninja-release/bin
build.cmd test       # builds both configs and runs ctest for both
build.cmd package    # release build + installer + portable zip -> out/dist/
build.cmd clean      # deletes out/
```

`build.ps1` is a 3-line wrapper that forwards to `build.cmd` so PowerShell users get the same thing.

### build.cmd (create exactly this)

```bat
@echo off
setlocal
set "ROOT=%~dp0"
set "CMD=%~1"
if "%CMD%"=="" set "CMD=release"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo [build] vswhere.exe not found - install VS 2022 Build Tools & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (echo [build] No VS installation with the C++ toolset found & exit /b 1)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo [build] vcvars64 failed & exit /b 1)

if /i "%CMD%"=="doctor"  goto :doctor
if /i "%CMD%"=="debug"   (set "PRESET=ninja-debug")   & goto :build
if /i "%CMD%"=="release" (set "PRESET=ninja-release") & goto :build
if /i "%CMD%"=="test"    goto :test
if /i "%CMD%"=="package" goto :package
if /i "%CMD%"=="clean"   (if exist "%ROOT%out" rmdir /s /q "%ROOT%out") & exit /b 0
echo usage: build.cmd [doctor^|debug^|release^|test^|package^|clean]
exit /b 2

:build
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
exit /b 0

:test
for %%p in (ninja-debug ninja-release) do (
  cmake --preset %%p || exit /b 1
  cmake --build --preset %%p || exit /b 1
  ctest --preset %%p --output-on-failure || exit /b 1
)
exit /b 0

:package
call "%~f0" release || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\package.ps1" || exit /b 1
exit /b 0

:doctor
echo VS:      %VSPATH%
where cl cmake ninja fxc rc mt 2>nul
cmake --version | findstr /i version
ninja --version
echo SDK:     %WindowsSDKVersion%
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" (echo Inno:    OK) else (echo Inno:    MISSING  ^(winget install JRSoftware.InnoSetup^))
exit /b 0
```

### CMakePresets.json

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "base", "hidden": true, "generator": "Ninja",
      "binaryDir": "${sourceDir}/out/build/${presetName}",
      "architecture": { "value": "x64", "strategy": "external" },
      "cacheVariables": {
        "CMAKE_C_COMPILER": "cl.exe", "CMAKE_CXX_COMPILER": "cl.exe",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
      }
    },
    { "name": "ninja-debug",   "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "ninja-release", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } }
  ],
  "buildPresets": [
    { "name": "ninja-debug",   "configurePreset": "ninja-debug" },
    { "name": "ninja-release", "configurePreset": "ninja-release" }
  ],
  "testPresets": [
    { "name": "ninja-debug",   "configurePreset": "ninja-debug",   "output": { "outputOnFailure": true } },
    { "name": "ninja-release", "configurePreset": "ninja-release", "output": { "outputOnFailure": true } }
  ]
}
```

## 3. Root CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.25)
project(super4vid VERSION 0.1.0 LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "super4vid builds with MSVC only. See docs/01-toolchain-and-build.md")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # /MT, /MTd
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)                        # /GL + /LTCG
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
enable_testing()

# Replace CMake's default MSVC flags so nothing conflicts with ours.
set(CMAKE_CXX_FLAGS         "/DWIN32 /D_WINDOWS")
set(CMAKE_CXX_FLAGS_DEBUG   "")
set(CMAKE_CXX_FLAGS_RELEASE "/DNDEBUG")
set(CMAKE_C_FLAGS           "/DWIN32 /D_WINDOWS")
set(CMAKE_C_FLAGS_DEBUG     "")
set(CMAKE_C_FLAGS_RELEASE   "/DNDEBUG")

add_library(ct_options INTERFACE)
target_compile_options(ct_options INTERFACE
  /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /Zc:throwingNew
  /utf-8 /EHs-c- /GR- /MP /Zi /FS /diagnostics:caret /guard:cf /sdl-
  $<$<CONFIG:Debug>:/Od /RTC1>
  $<$<CONFIG:Release>:/O2 /Ob3 /Oi /Ot /Gy /Gw>)
target_compile_definitions(ct_options INTERFACE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX STRICT _CRT_SECURE_NO_WARNINGS
  _HAS_EXCEPTIONS=0 _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000008
  CT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  CT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  CT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  $<$<CONFIG:Debug>:CT_DEBUG=1>)
target_link_options(ct_options INTERFACE
  /DEBUG:FULL /INCREMENTAL:NO /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
  $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)

add_library(ct_winlibs INTERFACE)
target_link_libraries(ct_winlibs INTERFACE
  d3d11 dxgi dxguid d3dcompiler d2d1 dwrite windowscodecs
  mfplat mf mfreadwrite mfuuid wmcodecdspuuid
  ole32 oleaut32 uuid user32 gdi32 shell32 shcore shlwapi dwmapi avrt winmm
  comctl32 propsys dbghelp advapi32 version)

include(cmake/CtModule.cmake)

add_subdirectory(third_party/imgui)
add_subdirectory(src/core)
add_subdirectory(src/platform)
# P02+: add_subdirectory(src/capture) ... one line per module as phases land
add_subdirectory(src/app)
add_subdirectory(tests)
```

`NTDDI_VERSION=0x0A000008` is Windows 10 2004 (`NTDDI_WIN10_VB`), our minimum. Do not raise it; use runtime
checks (`IsWindows10OrGreater`-style via `RtlGetVersion`, or `GetProcAddress`) for newer APIs.

## 4. cmake/CtModule.cmake

```cmake
# ct_add_module(<name> SOURCES ... DEPS ...)  -> static lib ct_<name>, public include dir include/
function(ct_add_module name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
  add_library(ct_${name} STATIC ${M_SOURCES})
  target_include_directories(ct_${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(ct_${name} PUBLIC ct_options ct_winlibs ${M_DEPS})
  set_target_properties(ct_${name} PROPERTIES FOLDER "modules")
endfunction()

# ct_add_test(<exe> SOURCES ... DEPS ...)  -> registers with ctest, runs from tests/ so tests/data is reachable
function(ct_add_test name)
  cmake_parse_arguments(T "" "" "SOURCES;DEPS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ct_options ct_test ${T_DEPS})
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
  set_target_properties(${name} PROPERTIES FOLDER "tests")
endfunction()

# ct_compile_shaders(<target> SHADERS file.hlsl:profile:entry ...)
# Produces <build>/shaders/<stem>_<entry>.h with a byte array named g_<stem>_<entry>.
function(ct_compile_shaders target)
  cmake_parse_arguments(S "" "" "SHADERS" ${ARGN})
  find_program(FXC_EXE fxc REQUIRED)     # on PATH after vcvars64
  set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
  file(MAKE_DIRECTORY ${out_dir})
  foreach(spec IN LISTS S_SHADERS)
    string(REPLACE ":" ";" parts ${spec})
    list(GET parts 0 file)
    list(GET parts 1 profile)
    list(GET parts 2 entry)
    get_filename_component(stem ${file} NAME_WE)
    set(out ${out_dir}/${stem}_${entry}.h)
    add_custom_command(OUTPUT ${out}
      COMMAND ${FXC_EXE} /nologo /T ${profile} /E ${entry} /WX
              $<IF:$<CONFIG:Debug>,/Zi;/Od,/O3> /Fh ${out} /Vn g_${stem}_${entry}
              ${CMAKE_CURRENT_SOURCE_DIR}/${file}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${file}
      COMMAND_EXPAND_LISTS COMMENT "fxc ${file} (${entry})")
    target_sources(${target} PRIVATE ${out})
  endforeach()
  target_include_directories(${target} PRIVATE ${out_dir})
endfunction()
```

Module template (`src/<module>/CMakeLists.txt`):

```cmake
ct_add_module(capture
  SOURCES src/desktop_duplicator.cpp src/display.cpp src/color_convert.cpp src/cursor_tracker.cpp
  DEPS ct_platform)
ct_compile_shaders(ct_capture SHADERS shaders/bgra_to_nv12.hlsl:ps_5_0:ps_luma shaders/bgra_to_nv12.hlsl:ps_5_0:ps_chroma shaders/fullscreen.hlsl:vs_5_0:vs_main)
```

## 5. Dear ImGui target (`third_party/imgui/CMakeLists.txt`)

Vendor the **docking** branch: clone the newest tag whose name ends in `-docking` (e.g. `v1.91.9b-docking`),
copy `*.h *.cpp` from the root plus `backends/imgui_impl_win32.*` and `backends/imgui_impl_dx11.*`, plus `LICENSE.txt`.
Write the tag into `third_party/imgui/VERSION.txt`. Never edit ImGui sources; configure via `ct_imconfig.h`.

```cmake
add_library(imgui STATIC
  imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui_demo.cpp
  backends/imgui_impl_win32.cpp backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/backends ${CMAKE_SOURCE_DIR}/src/ui/include)
target_compile_options(imgui PRIVATE /W3 /utf-8 /EHs-c- /GR- /MP /Zi /FS $<$<CONFIG:Debug>:/Od> $<$<CONFIG:Release>:/O2 /Gy /Gw>)
target_compile_definitions(imgui PUBLIC
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DEFINE_MATH_OPERATORS IMGUI_USER_CONFIG="ct/ui/ct_imconfig.h"
  _HAS_EXCEPTIONS=0 UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  $<$<CONFIG:Release>:IMGUI_DISABLE_DEMO_WINDOWS>)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)
set_target_properties(imgui PROPERTIES FOLDER "third_party")
```

`ct_imconfig.h` (in `src/ui/include/ct/ui/`): `#define IM_ASSERT(x) CT_ASSERT(x)`, `#define ImDrawIdx unsigned int`,
`#define IMGUI_USE_WCHAR32`.

## 6. Flags explained (so nobody "fixes" them)

| Flag | Why |
|---|---|
| `/EHs-c- /GR- /D_HAS_EXCEPTIONS=0` | No exceptions/RTTI: smaller, faster, no hidden control flow. STL still works; allocation failure terminates. |
| `/W4 /WX /permissive-` | Warnings are errors. Third-party code is compiled separately at `/W3` without `/WX`. |
| `/Zc:preprocessor` | Standards-conforming preprocessor (needed for `__VA_OPT__` in our log macros). |
| `/utf-8` | Source and execution charset UTF-8; string literals are UTF-8. |
| `/MT` (static CRT) | Zero redistributables. |
| `/O2 /Ob3 /Oi /Ot /Gy /Gw` + IPO | Speed, aggressive inlining, function/data-level linking for `/OPT:REF`. |
| `/Zi /DEBUG:FULL` in Release | PDBs always exist; they ship to the symbol archive, not to users. |
| `/guard:cf /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT` | Standard exploit mitigations; measurable cost is zero for this workload. |
| No `/arch:AVX2` | Baseline x64 (SSE2). Hot CPU loops (audio mix, pixel ops in GIF/thumbnails) get an AVX2 path selected at runtime via `__cpuidex`. |
| `/MP /FS` | Parallel compile in each target, shared PDB writes. |

## 7. App manifest (`src/app/app.manifest`, added as a source file so CMake embeds it)

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="super4vid.App" version="0.1.0.0"/>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges>
    <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
  </requestedPrivileges></security></trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application>
    <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
  </application></compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings>
    <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
    <heapType xmlns="http://schemas.microsoft.com/SMI/2020/WindowsSettings">SegmentHeap</heapType>
  </windowsSettings></application>
  <dependency><dependentAssembly>
    <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0"
                      processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/>
  </dependentAssembly></dependency>
</assembly>
```

## 8. Version stamping and resources

- `cmake/version.h.in` -> `${CMAKE_BINARY_DIR}/generated/ct/version.h` via `configure_file`. Contents:
  `CT_VERSION_STRING "0.1.0"`, `CT_VERSION_MAJOR/MINOR/PATCH`, `CT_GIT_HASH "abc1234"` (from
  `execute_process(COMMAND git rev-parse --short=7 HEAD ...)`, `"nogit"` if unavailable), `CT_BUILD_DATE`.
- `src/app/app.rc`: icon `IDI_APP ICON "assets/icon.ico"` and a `VS_VERSION_INFO VERSIONINFO` block whose strings
  come from `version.h` (CompanyName "super4vid", FileDescription "super4vid Screen Recorder and Editor",
  OriginalFilename "super4vid.exe", LegalCopyright "Copyright (C) 2026"). Translation `0x409, 1200`.
- Bumping the version = editing one line in the root `CMakeLists.txt` (`project(... VERSION x.y.z)`).

## 9. Output layout

```
out/build/ninja-release/bin/super4vid.exe       (+ super4vid.pdb)
out/build/ninja-release/bin/ct_core_tests.exe  ... one per module
out/build/ninja-release/bin/capture_bench.exe  audio_bench.exe player_demo.exe (tool exes, P02/P03/P06)
out/dist/super4vidSetup-0.1.0.exe  super4vid-0.1.0-portable.zip  super4vid-0.1.0-symbols.zip
```

## 10. Debugging aids

- Debug builds create the D3D11 device with `D3D11_CREATE_DEVICE_DEBUG` when the Graphics Tools feature is installed,
  and set `ID3D11InfoQueue` to break on `CORRUPTION` and `ERROR`. On exit, `ReportLiveDeviceObjects(D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL)` must print nothing but the device itself.
- Set `CT_LOG=trace` in the environment for verbose logs; logs go to `%LOCALAPPDATA%\super4vid\logs\super4vid-YYYYMMDD-HHMMSS.log` and `OutputDebugString`.
- Media Foundation: `mftrace.exe` (SDK `bin\10.0.26100.0\x64`) wraps the exe and dumps every MF call; use it when a sink writer or source reader rejects a media type.
- `super4vid.exe --stats` prints per-second counters (captured/encoded/dropped frames, queue depths, audio gaps) to the console.
- Attach WinDbg or VS to `.dmp` files from `%LOCALAPPDATA%\super4vid\crashes\`; PDBs for a release build are in the symbols zip of that version.

## 11. CI (`.github/workflows/build.yml`, created in P12 but documented here)

```yaml
name: build
on: [push, pull_request]
jobs:
  windows:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4
      - uses: ilammy/msvc-dev-cmd@v1
        with: { arch: x64 }
      - run: cmake --preset ninja-release
      - run: cmake --build --preset ninja-release
      - run: ctest --preset ninja-release --output-on-failure
        env: { CT_CI: "1" }           # device tests (GPU/audio/webcam) print SKIPPED and pass
      - run: choco install innosetup --no-progress -y
      - run: .\build.cmd package
      - uses: actions/upload-artifact@v4
        with: { name: super4vid-dist, path: out/dist/* }
```

Tests that need real hardware check `getenv("CT_CI")` (or detect the absence of a hardware adapter / audio endpoint)
and report `SKIPPED` instead of failing.
