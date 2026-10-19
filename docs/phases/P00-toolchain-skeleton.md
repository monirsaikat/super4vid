# P00 · Toolchain and skeleton

**Goal:** from an empty folder to a repository that builds in Debug and Release with zero warnings, opens a
HiDPI-correct Win32 window rendering Dear ImGui on a D3D11 flip-model swapchain, redraws only on input, logs to a
file, writes a minidump on crash, enforces single instance, and runs one test under `ctest`.

**Read first:** `docs/01-toolchain-and-build.md` (all), `docs/02-conventions.md` (all), `docs/06-testing-and-performance.md` section 1.
**Depends on:** nothing. **Size:** S (about 1,500 lines of code).

## Deliverables

```
CMakeLists.txt  CMakePresets.json  build.cmd  build.ps1  .gitignore  .editorconfig  README.md
cmake/CtModule.cmake  cmake/version.h.in
third_party/imgui/  (vendored, CMakeLists.txt, VERSION.txt, LICENSE.txt)
src/core/include/ct/core/{types.h,error.h,log.h,assert.h}  src/core/src/{error.cpp,log.cpp}  src/core/CMakeLists.txt
src/platform/include/ct/platform/{win.h,window.h,d3d_device.h,crash_handler.h,single_instance.h,paths.h}
src/platform/src/{window.cpp,d3d_device.cpp,crash_handler.cpp,single_instance.cpp,paths.cpp}  src/platform/CMakeLists.txt
src/test/include/ct/test/ct_test.h  src/test/src/ct_test.cpp  src/test/CMakeLists.txt
src/ui/include/ct/ui/ct_imconfig.h
src/app/{main.cpp,app.manifest,app.rc,CMakeLists.txt}
tests/CMakeLists.txt  tests/core/{CMakeLists.txt,smoke_test.cpp}
assets/icon.ico
```

## Tasks

### 0.1 Toolchain verified
1. Create `build.cmd` and `build.ps1` exactly as in `docs/01` section 2. `build.ps1` content: `& "$PSScriptRoot\build.cmd" @args; exit $LASTEXITCODE`.
2. Run `build.cmd doctor`. Expected: `cl`, `cmake`, `ninja`, `fxc`, `rc`, `mt` found; CMake >= 3.25; SDK `10.0.26100.0`. If Inno Setup is missing, install it now (`winget install JRSoftware.InnoSetup`); it is needed in P12 only, but verify it today.
3. If `cl` is missing: install the Build Tools workload from `docs/01` section 1 and rerun.

### 0.2 Repository scaffold
1. `git init`, `.gitignore` (`out/`, `*.user`, `*.suo`, `.vs/`, `*.dmp`, `*.log`, `tests/data/tmp/`), `.editorconfig` (UTF-8, LF, 4 spaces, trim trailing whitespace, final newline).
2. `README.md`: 10 lines: what it is, `build.cmd release`, where the exe lands, link to `MASTER_PLAN.md`.
3. Root `CMakeLists.txt`, `CMakePresets.json`, `cmake/CtModule.cmake` verbatim from `docs/01` sections 2-4. Add `cmake/version.h.in`:
   ```c
   #pragma once
   #define CT_VERSION_STRING "@PROJECT_VERSION@"
   #define CT_GIT_HASH "@CT_GIT_HASH@"
   #define CT_BUILD_DATE "@CT_BUILD_DATE@"
   ```
   and in the root CMake: `execute_process(COMMAND git rev-parse --short=7 HEAD OUTPUT_VARIABLE CT_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)`, `string(TIMESTAMP CT_BUILD_DATE "%Y-%m-%d")`, `configure_file(cmake/version.h.in ${CMAKE_BINARY_DIR}/generated/ct/version.h)`, and `target_include_directories(ct_options INTERFACE ${CMAKE_BINARY_DIR}/generated)`.
4. Create all `src/<module>/` directories with empty `include/ct/<module>/` and `src/` subfolders (only `core`, `platform`, `test`, `ui`, `app` get CMake targets in this phase).
5. `assets/icon.ico`: generate a placeholder with `tools/make_icon.py` (Pillow: a 256 px rounded square `#4F8CFF` with a white circle, saved as ICO with 16/32/48/256 sizes). If Python/Pillow is unavailable, create a 32x32 ICO programmatically with a 40-line C++ tool in `tools/`. The icon is replaced by a designer later; any valid multi-size ICO is acceptable.

### 0.3 `ct_core` stub and test harness
Create these headers with the exact signatures (bodies are yours):

```cpp
// ct/core/types.h
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace ct {
struct PointI { int32_t x = 0, y = 0; };
struct SizeI  { int32_t w = 0, h = 0; bool IsEmpty() const { return w <= 0 || h <= 0; } };
struct RectI  { int32_t x = 0, y = 0, w = 0, h = 0;
                int32_t Right() const { return x + w; } int32_t Bottom() const { return y + h; }
                bool Contains(PointI p) const; RectI Intersect(const RectI& o) const; bool IsEmpty() const { return w <= 0 || h <= 0; } };
struct RectF  { float x = 0, y = 0, w = 0, h = 0; };
struct ColorF { float r = 0, g = 0, b = 0, a = 1; static ColorF FromHex(std::string_view hex /* "#RRGGBB" or "#RRGGBBAA" */); std::string ToHex() const; };
using TimeUs = int64_t;
constexpr TimeUs kUsPerSecond = 1'000'000;
} // namespace ct
```

```cpp
// ct/core/error.h  (macros from docs/02 section 4)
namespace ct {
struct Err { std::string message; long hr = 0; void Set(std::string_view m, long h = 0); bool Ok() const { return message.empty(); } };
std::string HrToString(long hr);                       // "0x887A0026 (DXGI_ERROR_ACCESS_LOST)"
void LogHr(long hr, const char* expr, const char* file, int line);
}
```

```cpp
// ct/core/log.h
namespace ct {
enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Off };
struct Log {
    static void Init(const char* log_dir_utf8);        // opens %LOCALAPPDATA%\super4vid\logs\super4vid-<stamp>.log, rotates (keep 5)
    static void SetLevel(LogLevel);                    // default Info; CT_LOG env var: trace|debug|info|warn|error
    static void Write(LogLevel, const char* category, const char* file, int line, const char* fmt, ...);
    static void Tick();                                // flush if > 500 ms since last flush; UI loop calls it
    static void Flush();
    static void Shutdown();
    static const char* FilePathUtf8();
};
}
#define CT_LOG_CATEGORY "app"   // each module redefines before including: #define CT_LOG_CATEGORY "capture"
#define CT_LOG_TRACE(...) ::ct::Log::Write(::ct::LogLevel::Trace, CT_LOG_CATEGORY, __FILE__, __LINE__, __VA_ARGS__)
#define CT_LOG_DEBUG(...) ...  CT_LOG_INFO(...)  CT_LOG_WARN(...)  CT_LOG_ERROR(...)
```
Logger implementation for this phase: `SRWLOCK` + 64 KB buffer + `WriteFile`, flush when the buffer passes 48 KB, on Warn/Error, in `Tick()` every 500 ms, and in `Shutdown()`. Also `OutputDebugStringA` for every line when a debugger is present (`IsDebuggerPresent()`). Line format from `docs/02` section 5. P01 replaces the internals with a lock-free ring without changing this header.

```cpp
// ct/core/assert.h
#define CT_ASSERT(cond)   // Debug: if (!(cond)) { Log error; __debugbreak(); }  Release: ((void)0)
#define CT_VERIFY(cond)   // both configs: evaluates cond; logs on failure; Debug breaks
#define CT_FATAL(msg)     // logs, flushes, ct::CrashHandler::WriteDumpAndExit(msg)
```

Test harness `ct/test/ct_test.h` + `ct_test.cpp`: implement the API in `docs/06` section 1 (registration via static objects in a linked list, `RunAll`, filters, timing with QPC, `CT_TEST_DEVICE` skipping when `getenv("CT_CI")` is set, golden support can be stubbed until P08 but the macro must exist). `tests/core/smoke_test.cpp`: `CT_TEST(rect_intersect)`, `CT_TEST(color_hex_roundtrip)`, `CT_TEST(hr_to_string_names_dxgi)` (checks `HrToString(0x887A0026)` contains `ACCESS_LOST`).

Wire `tests/CMakeLists.txt` -> `add_subdirectory(core)`; `tests/core/CMakeLists.txt` -> `ct_add_test(ct_core_tests SOURCES smoke_test.cpp DEPS ct_core)`.

### 0.4 Dear ImGui vendored
Follow `docs/01` section 5 exactly. `ct_imconfig.h`:
```cpp
#pragma once
#include <ct/core/assert.h>
#define IM_ASSERT(_EXPR) CT_ASSERT(_EXPR)
#define IMGUI_USE_WCHAR32
#define ImDrawIdx unsigned int
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS 0
```
Build: `imgui` target compiles with no warnings at `/W3`.

### 0.5 Window, device, swapchain, ImGui loop

```cpp
// ct/platform/window.h
namespace ct::platform {
class Window {
public:
    struct Desc { const wchar_t* class_name; const wchar_t* title; int w_dip, h_dip; bool frameless; bool topmost; bool tool_window; bool resizable; HICON icon; };
    using MessageHandler = LRESULT(*)(void* user, HWND, UINT, WPARAM, LPARAM, bool& handled);
    bool Create(const Desc&, MessageHandler, void* user);
    HWND Hwnd() const;  UINT Dpi() const;  float DpiScale() const;  SizeI ClientSizePx() const;
    void Show(int cmd = SW_SHOW);  void SetTitle(const wchar_t*);  void SetDarkTitleBar(bool);  void SetRoundedCorners(bool);
    void SetExcludeFromCapture(bool);           // SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE); ignore failure on < 2004
    void Destroy();
private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
};
}
```
Handle in `WndProc` before forwarding: `WM_DPICHANGED` (resize to the suggested rect, update dpi), `WM_SIZE` (store size, flag swapchain resize), `WM_GETMINMAXINFO` (min 640x400 dip for the editor), `WM_CLOSE` -> handler decides, `WM_DESTROY` -> `PostQuitMessage` only for the main window. `WM_NCCREATE`: `EnableNonClientDpiScaling` is not needed with Per-Monitor V2 manifest.

```cpp
// ct/platform/d3d_device.h  (minimal here; P02 extends with the MF device manager and NV12 checks)
namespace ct::platform {
class D3DDevice {
public:
    HRESULT Init(bool enable_debug_layer);      // adapter by DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, FL 11_1/11_0, flags BGRA_SUPPORT|VIDEO_SUPPORT (+DEBUG), WARP fallback
    ID3D11Device5* Device() const;  ID3D11DeviceContext4* Context() const;  IDXGIFactory6* Factory() const;
    bool IsWarp() const;  std::string AdapterName() const;
    HRESULT CheckRemoved() const;               // Device()->GetDeviceRemovedReason()
    void ReportLiveObjects() const;             // Debug only
    class Lock { public: explicit Lock(D3DDevice&); ~Lock(); };   // CRITICAL_SECTION around the immediate context
    void Shutdown();
};
class SwapChain {
public:
    HRESULT Init(D3DDevice&, HWND);             // CreateSwapChainForHwnd: BGRA8, 2 buffers, FLIP_DISCARD, SCALING_NONE, ALPHA_IGNORE, FRAME_LATENCY_WAITABLE_OBJECT; SetMaximumFrameLatency(1); MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)
    HRESULT Resize(SizeI px);                   // release RTV, ResizeBuffers(0,0,0,UNKNOWN,flags), recreate RTV
    ID3D11RenderTargetView* Rtv() const;  HANDLE WaitableObject() const;  SizeI Size() const;
    HRESULT Present(bool vsync);               // Present(vsync?1:0, 0); map DXGI_ERROR_DEVICE_REMOVED/RESET to the caller
};
}
```
After `D3D11CreateDevice` succeeds: `QueryInterface(ID3D10Multithread)` -> `SetMultithreadProtected(TRUE)`. In Debug with the debug layer: `ID3D11InfoQueue::SetBreakOnSeverity(CORRUPTION, TRUE)` and `(ERROR, TRUE)`. If `D3D11CreateDevice` with `D3D11_CREATE_DEVICE_DEBUG` fails with `DXGI_ERROR_SDK_COMPONENT_MISSING`, log a warning and retry without the flag.

`src/app/main.cpp` flow:
```
wWinMain:
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX); CrashHandler::Install();
  Paths::Init(); Log::Init(Paths::LogsDir()); CT_LOG_INFO("super4vid %s (%s) starting", CT_VERSION_STRING, CT_GIT_HASH);
  if (!SingleInstance::Acquire(L"Local\\super4vid.Instance", cmdline)) return 0;   // forwarded to the running instance via WM_COPYDATA
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  parse args: --crash-test, --time-to-window, --stats (AllocConsole + freopen), default = ui
  D3DDevice dev; dev.Init(CT_DEBUG);  Window win; win.Create(...);  SwapChain sc; sc.Init(dev, win.Hwnd());
  ImGui: CreateContext; io.ConfigFlags |= DockingEnable | NavEnableKeyboard; ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(device, context); load font (docs/05 section 1, size * dpi_scale)
  loop (event-driven, below); on exit: ImGui shutdown, sc/dev shutdown, dev.ReportLiveObjects() in Debug, Log::Shutdown()
```
Event-driven loop (this is the heart of "0 redraws when idle"):
```cpp
HANDLE waits[2] = { app.wake_event, sc.WaitableObject() };
int frames_requested = 2;                   // ImGui needs a couple of frames after input to settle hover/active state
for (;;) {
    DWORD timeout = frames_requested > 0 ? 0 : INFINITE;
    MsgWaitForMultipleObjectsEx(1, &app.wake_event, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) goto done; TranslateMessage(&msg); DispatchMessageW(&msg); frames_requested = 2; }
    if (app.redraw_requested.exchange(false)) frames_requested = 2;    // set by other threads + SetEvent(wake_event)
    Log::Tick();
    if (frames_requested > 0 && !app.minimized) {
        WaitForSingleObject(sc.WaitableObject(), 1000);                  // back-pressure, never more than 1 frame queued
        if (app.needs_resize) sc.Resize(win.ClientSizePx());
        RenderOneFrame();                                                // ImGui NewFrame .. Render .. Present(vsync)
        --frames_requested;
        if (app.continuous) frames_requested = 1;                        // playback / animations
    }
}
```
`RenderOneFrame` in this phase: clear to `#0F1115`, `ImGui::ShowDemoWindow()` in Debug, a window with the version string and the measured frame time in Release. Dark title bar via `DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE (20), &TRUE, 4)`.

### 0.6 Logging, crash dumps, single instance, paths
```cpp
// ct/platform/crash_handler.h
namespace ct::platform {
struct CrashHandler {
    static void Install();                                         // SetUnhandledExceptionFilter + _set_invalid_parameter_handler + _set_purecall_handler + set_terminate
    static void SetDumpDir(const char* utf8);
    using EmergencyCallback = void(*)(void* user);
    static void RegisterEmergency(EmergencyCallback, void* user);  // called inside the filter, max 8, must not allocate (P05 registers "finalize the recording")
    [[noreturn]] static void WriteDumpAndExit(const char* reason);
};
struct SingleInstance { static bool Acquire(const wchar_t* mutex_name, const wchar_t* cmdline_to_forward); static void Release(); };
struct Paths { static void Init(); static const std::string& LogsDir(); static const std::string& CrashDir(); static const std::string& SettingsDir(); static const std::string& AutosaveDir(); static const std::string& DefaultRecordingsDir(); static const std::string& ExeDir(); static bool Portable(); };
}
```
Dump: `MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithDataSegs), &ex_info, nullptr, nullptr)` to `crash-YYYYMMDD-HHMMSS-<pid>.dmp`. Then run emergency callbacks, `Log::Flush()`, `TerminateProcess(GetCurrentProcess(), 0xC0000409)`. The filter must not use the CRT heap more than necessary (preformat the path at `Install`). `--crash-test` dereferences null after a log line.

Single instance: `CreateMutexW`; `GetLastError() == ERROR_ALREADY_EXISTS` -> `FindWindowW(L"super4vidMain", nullptr)` -> `COPYDATASTRUCT{ dwData = 1, cbData = bytes, lpData = cmdline }` -> `SendMessageTimeoutW(..., 2000)` -> return false.

Paths: `SHGetKnownFolderPath(FOLDERID_LocalAppData / RoamingAppData / Videos)` + `\super4vid\...`, `CreateDirectoryW` recursively; portable mode if `portable.txt` exists next to the exe.

### 0.7 Acceptance
- `build.cmd debug` and `build.cmd release`: 0 warnings, 0 errors. `build.cmd test`: `ct_core_tests` passes in both.
- Run the Release exe: window appears with the dark title bar, text crisp at 150 % DPI, resizing works, moving between monitors with different DPI rescales fonts.
- With the mouse still for 10 s, `--stats` prints `redraws/s: 0`; Task Manager shows 0 % CPU and 0 % GPU for the process.
- `super4vid.exe --crash-test` produces a `.dmp` in `%LOCALAPPDATA%\super4vid\crashes\` that opens in WinDbg/VS and shows the faulting line.
- Starting a second instance while one runs exits immediately and the first instance logs `forwarded command line`.
- Debug build exit prints no live D3D objects besides the device/context and no CRT leaks.

## Pitfalls
- Do not call `ImGui_ImplWin32_EnableDpiAwareness()`; the manifest already declares Per-Monitor V2 (calling both is harmless but confusing).
- `ImGui_ImplWin32_WndProcHandler` must run before your own handling in `WndProc` and its return value respected.
- `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` requires the same flag in `ResizeBuffers` and is incompatible with fullscreen exclusive mode (we never use it).
- Wait on the waitable object **before** rendering, not after presenting, or you add a frame of latency.
- `/W4` flags unused parameters in Win32 callbacks: name them and `(void)` them, do not lower the warning level.
- Static CRT + `_HAS_EXCEPTIONS=0`: do not include `<functional>`-heavy headers in hot code; `std::function` itself is fine for thread entry points.
