// Camtesia.exe entry point: window + D3D11 flip-model swapchain + Dear ImGui, driven by an event loop
// that renders only after input (0 redraws/s when idle).
#define CT_LOG_CATEGORY "app"
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/crash_handler.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/paths.h>
#include <ct/platform/single_instance.h>
#include <ct/platform/win.h>
#include <ct/platform/window.h>
#include <ct/ui/editor_shell.h>
#include <ct/ui/settings_window.h>
#include <ct/ui/theme.h>
#include <ct/version.h>

#include "resource.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <objbase.h>
#include <shellapi.h>
#if defined(CT_DEBUG)
#include <crtdbg.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using ct::platform::D3DDevice;
using ct::platform::SwapChain;
using ct::platform::Window;

constexpr float kBaseFontPx = 14.0f;   // docs/05 section 1: body text 14 px at 96 dpi

struct Options {
    bool crash_test = false;
    bool time_to_window = false;
    bool stats = false;
    bool tray = false;   // --tray: start hidden ("start with Windows" installer task); the tray icon itself is P05
    bool editor_demo = false;     // --editor-demo [--project p.ctproj]: P09 editor shell on a sample project
    std::string project;
    bool settings_demo = false;   // --settings-demo [--section audio]: P11 settings window filling the window
    std::string section;
    std::string screenshot;       // --screenshot out.png --frames N: save the window after N frames and exit
    int frames = 5;
};

struct App {
    Window win;
    D3DDevice dev;
    SwapChain sc;
    HANDLE wake_event = nullptr;            // other threads: set redraw_requested, then SetEvent(wake_event)
    std::atomic<bool> redraw_requested{false};
    bool continuous = false;                // playback/animations keep rendering
    ImGuiStyle base_style;                  // unscaled; re-applied at every DPI change
    double last_frame_ms = 0.0;
    int64_t qpc_freq = 1;
    uint32_t redraws_this_second = 0;
    uint32_t messages_this_second = 0;      // --stats: what woke the loop
    UINT last_message = 0;
    bool quit = false;
    ct::ui::EditorShell* editor = nullptr;
    ct::ui::SettingsWindow* settings = nullptr;
};

int64_t QpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double MsSinceProcessStart() {
    FILETIME creation, exit_time, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) return -1.0;
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    const auto to_u64 = [](const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    return static_cast<double>(to_u64(now) - to_u64(creation)) / 10'000.0;   // 100 ns -> ms
}

Options ParseArgs() {
    Options opt;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (wcscmp(argv[i], L"--crash-test") == 0) opt.crash_test = true;
        else if (wcscmp(argv[i], L"--time-to-window") == 0) opt.time_to_window = true;
        else if (wcscmp(argv[i], L"--stats") == 0) opt.stats = true;
        else if (wcscmp(argv[i], L"--tray") == 0) opt.tray = true;
        else if (wcscmp(argv[i], L"--editor-demo") == 0) opt.editor_demo = true;
        else if (wcscmp(argv[i], L"--settings-demo") == 0) opt.settings_demo = true;
        else if (wcscmp(argv[i], L"--section") == 0 && i + 1 < argc) opt.section = ct::WideToUtf8(argv[++i]);
        else if (wcscmp(argv[i], L"--project") == 0 && i + 1 < argc) opt.project = ct::WideToUtf8(argv[++i]);
        else if (wcscmp(argv[i], L"--screenshot") == 0 && i + 1 < argc) opt.screenshot = ct::WideToUtf8(argv[++i]);
        else if (wcscmp(argv[i], L"--frames") == 0 && i + 1 < argc) opt.frames = _wtoi(argv[++i]);
    }
    if (argv) LocalFree(argv);
    return opt;
}

// --stats / --time-to-window print to stdout when it is redirected to a file or pipe (scripts), otherwise to
// the parent console if there is one, else to a new console.
void OpenConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN &&
        GetFileType(out) != FILE_TYPE_CHAR) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // System fonts only (docs/05 section 1). With ImGui >= 1.92 glyphs rasterize on demand at any size,
    // so DPI changes only update style.FontScaleDpi.
    const char* candidates[] = {"C:\\Windows\\Fonts\\SegUIVar.ttf", "C:\\Windows\\Fonts\\segoeui.ttf"};
    for (const char* path : candidates) {
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        if (io.Fonts->AddFontFromFileTTF(path, kBaseFontPx)) {
            CT_LOG_INFO("UI font: %s", path);
            return;
        }
    }
    CT_LOG_WARN("no system UI font found; using the ImGui default font");
    io.Fonts->AddFontDefault();
}

void ApplyDpi(App& app) {
    const float scale = app.win.DpiScale();
    ImGuiStyle& style = ImGui::GetStyle();
    style = app.base_style;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    CT_LOG_INFO("ui scale %.2f (dpi %u)", scale, app.win.Dpi());
}

LRESULT AppMessageHandler(void* user, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, bool& handled) {
    App& app = *static_cast<App*>(user);
    // ImGui first, and its result is respected (docs/phases/P00 pitfalls).
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        handled = true;
        return 1;
    }
    switch (msg) {
    case WM_DPICHANGED:
        if (ImGui::GetCurrentContext()) ApplyDpi(app);
        break;
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (cds && cds->dwData == ct::platform::SingleInstance::kCopyDataCommandLine && cds->lpData) {
            const int chars = static_cast<int>(cds->cbData / sizeof(wchar_t));
            CT_LOG_INFO("forwarded command line: %.*ls", chars, static_cast<const wchar_t*>(cds->lpData));
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);   // started with --tray
            SetForegroundWindow(hwnd);
            handled = true;
            return TRUE;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

bool RenderOneFrame(App& app) {
    const int64_t start = QpcNow();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (app.settings) {
        app.settings->Draw();
        app.continuous = app.settings->WantsRedraw();
        if (!app.settings->IsOpen()) PostQuitMessage(0);
    } else if (app.editor) {
        app.editor->status.frame_ms = app.last_frame_ms;
        app.editor->Frame();
        app.continuous = app.editor->WantsContinuousRedraw();
    } else {
#if defined(CT_DEBUG)
    ImGui::ShowDemoWindow();
#endif
    ImGui::SetNextWindowPos(ImVec2(24.0f * app.win.DpiScale(), 24.0f * app.win.DpiScale()), ImGuiCond_FirstUseEver);
    ImGui::Begin("Camtesia", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Camtesia %s (%s, %s)", CT_VERSION_STRING, CT_GIT_HASH, CT_BUILD_DATE);
    ImGui::Text("Adapter: %s", app.dev.AdapterName().c_str());
    ImGui::Text("DPI: %u (%.0f%%)", app.win.Dpi(), app.win.DpiScale() * 100.0f);
    ImGui::Text("Frame time: %.2f ms (CPU)", app.last_frame_ms);
    ImGui::End();
    }

    ImGui::Render();
    {
        D3DDevice::Lock lock(app.dev);
        ID3D11DeviceContext4* ctx = app.dev.Context();
        ID3D11RenderTargetView* rtv = app.sc.Rtv();
        const float clear[4] = {0x0F / 255.0f, 0x11 / 255.0f, 0x15 / 255.0f, 1.0f};   // bg0 #0F1115
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    const HRESULT hr = app.sc.Present(true);
    app.last_frame_ms = static_cast<double>(QpcNow() - start) * 1000.0 / static_cast<double>(app.qpc_freq);
    ++app.redraws_this_second;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // Device-lost recovery lands in P11; for now exit cleanly with the reason in the log.
        CT_LOG_ERROR("Present: %s, removed reason %s", ct::HrToString(hr).c_str(),
                     ct::HrToString(app.dev.CheckRemoved()).c_str());
        return false;
    }
    if (FAILED(hr)) CT_LOG_WARN("Present failed: %s", ct::HrToString(hr).c_str());
    return true;
}

int RunUi(App& app, const Options& opt) {
    HANDLE stats_timer = nullptr;
    if (opt.stats) {
        stats_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -10'000'000LL;   // 1 s, relative
        SetWaitableTimer(stats_timer, &due, 1000, nullptr, nullptr, FALSE);
    }
    HANDLE handles[2] = {app.wake_event, stats_timer};
    const DWORD handle_count = stats_timer ? 2 : 1;

    bool first_frame = true;
    int screenshot_frames = opt.frames;
    int frames_requested = 2;   // ImGui needs a couple of frames after input to settle hover/active state
    while (!app.quit) {
        const DWORD timeout = frames_requested > 0 ? 0 : INFINITE;
        const DWORD wait = MsgWaitForMultipleObjectsEx(handle_count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (stats_timer && wait == WAIT_OBJECT_0 + 1) {
            // A kernel timer, not WM_TIMER, so printing stats does not itself cause redraws.
            std::printf("redraws/s: %u  messages/s: %u (last 0x%04X)\n", app.redraws_this_second,
                        app.messages_this_second, app.last_message);
            CT_LOG_INFO("stats: redraws/s: %u messages/s: %u (last 0x%04X)", app.redraws_this_second,
                        app.messages_this_second, app.last_message);
            app.redraws_this_second = 0;
            app.messages_this_second = 0;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                app.quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            frames_requested = 2;
            ++app.messages_this_second;
            app.last_message = msg.message;
        }
        if (app.quit) break;
        if (app.redraw_requested.exchange(false)) frames_requested = 2;   // set by other threads + SetEvent(wake_event)
        ct::Log::Tick();

        if (frames_requested > 0 && !app.win.IsMinimized()) {
            WaitForSingleObject(app.sc.WaitableObject(), 1000);            // back-pressure, never more than 1 frame queued
            if (app.win.ConsumeResize()) CT_LOG_IF_FAILED(app.sc.Resize(app.win.ClientSizePx()));
            if (!RenderOneFrame(app)) break;
            --frames_requested;
            if (!opt.screenshot.empty()) {
                frames_requested = 1;
                if (--screenshot_frames <= 0) {
                    ct::ui::SaveWindowScreenshot(app.win.Hwnd(), opt.screenshot.c_str());
                    PostQuitMessage(0);
                }
            }
            if (app.continuous) frames_requested = 1;
            if (first_frame) {
                first_frame = false;
                const double ms = MsSinceProcessStart();
                CT_LOG_INFO("time-to-window: %.1f ms", ms);
                if (opt.time_to_window) {
                    std::printf("time-to-window: %.1f ms\n", ms);
                    PostQuitMessage(0);
                }
            }
        } else if (frames_requested > 0) {
            frames_requested = 0;   // minimized: nothing to draw until the next message
        }
    }
    if (stats_timer) CloseHandle(stats_timer);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
#if defined(CT_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    // Leak reports also go to stderr so they are visible without a debugger (e.g. `Camtesia.exe 2>leaks.txt`).
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    ct::platform::CrashHandler::Install();
    ct::platform::Paths::Init();
    ct::platform::CrashHandler::SetDumpDir(ct::platform::Paths::CrashDir().c_str());
    // Single-instance first: a forwarding second launch must not prune logs or open a file of its own (lines logged
    // before Log::Init stay in the ring and are written once the file exists).
    if (!ct::platform::SingleInstance::Acquire(L"Local\\Camtesia.Instance", GetCommandLineW())) {
        ct::Log::Shutdown();
        return 0;   // forwarded to the running instance via WM_COPYDATA
    }
    ct::Log::Init(ct::platform::Paths::LogsDir().c_str());
    CT_LOG_INFO("Camtesia %s (%s) starting", CT_VERSION_STRING, CT_GIT_HASH);

    const Options opt = ParseArgs();
    if (opt.stats || opt.time_to_window) OpenConsole();
    if (opt.crash_test) {
        CT_LOG_WARN("--crash-test: dereferencing a null pointer");
        volatile int* volatile null_ptr = nullptr;
        *null_ptr = 42;
    }

    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com_hr)) CT_LOG_ERROR("CoInitializeEx failed: %s", ct::HrToString(com_hr).c_str());

    App* app_ptr = new App();   // big and long-lived; not on the stack
    App& app = *app_ptr;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    app.qpc_freq = freq.QuadPart;
    app.wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    int exit_code = 1;
#if defined(CT_DEBUG)
    const bool debug_layer = true;
#else
    const bool debug_layer = false;
#endif
    HRESULT hr = app.dev.Init(debug_layer);
    if (FAILED(hr)) {
        CT_LOG_ERROR("no usable D3D11 device: %s", ct::HrToString(hr).c_str());
        MessageBoxW(nullptr, L"Camtesia needs a Direct3D 11 capable graphics driver.", L"Camtesia", MB_ICONERROR);
    } else {
        Window::Desc desc;
        desc.class_name = L"CamtesiaMain";
        desc.title = L"Camtesia";
        desc.w_dip = opt.editor_demo ? 1600 : opt.settings_demo ? 840 : 1280;
        desc.h_dip = opt.editor_demo ? 900 : opt.settings_demo ? 580 : 800;
        desc.resizable = true;
        desc.icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
        desc.quit_on_destroy = true;
        desc.min_client_dip = ct::SizeI{640, 400};
        if (app.win.Create(desc, &AppMessageHandler, &app) && SUCCEEDED(app.sc.Init(app.dev, app.win.Hwnd()))) {
            app.win.SetDarkTitleBar(true);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
            io.IniFilename = nullptr;   // layout persistence is decided in P09
            if (opt.editor_demo || opt.settings_demo) ct::ui::EditorShell::SetupImGui();   // theme + system fonts + icons
            else ImGui::StyleColorsDark();
            app.base_style = ImGui::GetStyle();
            ImGui_ImplWin32_Init(app.win.Hwnd());
            ImGui_ImplDX11_Init(app.dev.Device(), app.dev.Context());
            if (!opt.editor_demo && !opt.settings_demo) LoadFonts();
            if (opt.settings_demo)
                app.settings = ct::ui::SettingsWindow::CreateDemo(opt.section, app.dev.AdapterName(), [&app] {
                    app.redraw_requested = true;
                    SetEvent(app.wake_event);
                }).release();
            if (opt.editor_demo) {
                app.editor = new ct::ui::EditorShell();
                app.editor->SetHostWindow(app.win.Hwnd());
                std::string path = opt.project.empty() ? ct::platform::Paths::ExeDir() + "/../../../../tests/data/projects/sample.ctproj" : opt.project;
                std::string err;
                if (!app.editor->LoadProject(path.c_str(), &err)) CT_LOG_WARN("--editor-demo: %s", err.c_str());
                app.editor->EnableDemo();
            }
            ApplyDpi(app);

            app.win.Show(opt.tray ? SW_HIDE : show_cmd);
            exit_code = RunUi(app, opt);

            delete app.editor;
            app.editor = nullptr;
            delete app.settings;
            app.settings = nullptr;
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        } else {
            CT_LOG_ERROR("window or swapchain creation failed");
        }
        app.sc.Shutdown();
        app.win.Destroy();
        app.dev.Shutdown();
        app.dev.ReportLiveObjects();   // Debug: logs any D3D object still alive besides the device
    }

    if (app.wake_event) CloseHandle(app.wake_event);
    delete app_ptr;
    if (SUCCEEDED(com_hr)) CoUninitialize();
    ct::platform::SingleInstance::Release();
    CT_LOG_INFO("exit %d", exit_code);
    ct::Log::Shutdown();
    return exit_code;
}
