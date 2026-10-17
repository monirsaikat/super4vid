#define CT_LOG_CATEGORY "app"
#include <ct/ui/recorder_app.h>

#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/image_io.h>
#include <ct/platform/paths.h>
#include <ct/platform/single_instance.h>
#include <ct/platform/window.h>
#include <ct/ui/first_run.h>
#include <ct/ui/fonts.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ct::ui {

namespace {

constexpr UINT kInvokeMsg = WM_APP + 40;   // Async::PostToUi payload (std::function<void()>* in lParam)

struct Extra {
    bool synthetic_camera = false;
    bool bar_record = false;
    bool bar_toast = false;
    bool bar_settings = false;
    bool no_exclude = false;
    int countdown = -1;
    int bench_hotkey = 0;
    RecorderBar::Popup open = RecorderBar::Popup::None;
};

Extra ParseExtra() {
    Extra e;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const wchar_t* a = argv[i];
        const bool next = i + 1 < argc;
        if (!wcscmp(a, L"--synthetic-camera")) e.synthetic_camera = true;
        else if (!wcscmp(a, L"--bar-record")) e.bar_record = true;
        else if (!wcscmp(a, L"--bar-toast")) e.bar_toast = true;
        else if (!wcscmp(a, L"--bar-settings")) e.bar_settings = true;
        else if (!wcscmp(a, L"--no-exclude")) e.no_exclude = true;
        else if (!wcscmp(a, L"--countdown") && next) e.countdown = _wtoi(argv[++i]);
        else if (!wcscmp(a, L"--bench-hotkey") && next) e.bench_hotkey = _wtoi(argv[++i]);
        else if (!wcscmp(a, L"--bar-open") && next) {
            const wchar_t* p = argv[++i];
            e.open = !wcscmp(p, L"monitor") ? RecorderBar::Popup::Monitor
                     : !wcscmp(p, L"fps")   ? RecorderBar::Popup::Fps
                     : !wcscmp(p, L"mic")   ? RecorderBar::Popup::Mic
                     : !wcscmp(p, L"system") ? RecorderBar::Popup::System
                     : !wcscmp(p, L"camera") ? RecorderBar::Popup::Camera
                                             : RecorderBar::Popup::None;
        }
    }
    if (argv) LocalFree(argv);
    return e;
}

double MsSinceProcessStart() {
    FILETIME creation, exit_time, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) return -1.0;
    GetSystemTimePreciseAsFileTime(&now);
    const auto v = [](const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; };
    return static_cast<double>(v(now) - v(creation)) / 10'000.0;
}

struct App {
    platform::D3DDevice dev;
    platform::Window win;
    platform::SwapChain sc;
    RecorderBar bar;
    ImGuiStyle base_style;
    bool quit = false;
    bool drop_invokes = false;
    bool exclude = true;
    HICON icon = nullptr;
    uint32_t redraws = 0, messages = 0;
    UINT last_message = 0;
};

App* g_app = nullptr;
void (*g_orig_create_window)(ImGuiViewport*) = nullptr;

// Secondary viewports (popovers, toasts, tooltips): excluded from capture, topmost, DWM-rounded with the border token.
void CreateViewportWindow(ImGuiViewport* vp) {
    g_orig_create_window(vp);
    HWND hwnd = static_cast<HWND>(vp->PlatformHandleRaw ? vp->PlatformHandleRaw : vp->PlatformHandle);
    if (!hwnd) return;
    const DWORD round = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof(round));
    const COLORREF border = RGB(0x2C, 0x32, 0x3E);
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (g_app && g_app->exclude && !SetWindowDisplayAffinity(hwnd, 0x11 /*WDA_EXCLUDEFROMCAPTURE*/))
        CT_LOG_WARN("popover: WDA_EXCLUDEFROMCAPTURE failed (%lu)", GetLastError());
    if (vp->Flags & ImGuiViewportFlags_NoDecoration) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {   // the settings window: a normal titled top-level window
        SetWindowTextW(hwnd, L"Camtesia Settings");
        if (g_app && g_app->icon) {
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_app->icon));
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_app->icon));
        }
    }
}

void ApplyDpi(App& app) {
    const float scale = app.win.DpiScale();
    ImGuiStyle& style = ImGui::GetStyle();
    style = app.base_style;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

void PostToUi(void* user, std::function<void()>* fn) {
    HWND hwnd = static_cast<HWND>(user);
    if (!PostMessageW(hwnd, kInvokeMsg, 0, reinterpret_cast<LPARAM>(fn))) delete fn;
}

LRESULT Handler(void* user, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, bool& handled) {
    App& app = *static_cast<App*>(user);
    if (msg == kInvokeMsg) {
        auto* fn = reinterpret_cast<std::function<void()>*>(lparam);
        if (fn && !app.drop_invokes) (*fn)();
        delete fn;
        handled = true;
        return 0;
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        handled = true;
        return 1;
    }
    const LRESULT r = app.bar.HandleMessage(hwnd, msg, wparam, lparam, handled);
    if (handled) return r;
    switch (msg) {
    case WM_DPICHANGED:
        if (ImGui::GetCurrentContext()) ApplyDpi(app);
        break;
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (cds && cds->dwData == platform::SingleInstance::kCopyDataCommandLine && cds->lpData) {
            CT_LOG_INFO("forwarded command line: %.*ls", static_cast<int>(cds->cbData / sizeof(wchar_t)),
                        static_cast<const wchar_t*>(cds->lpData));
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

// The bar and every visible secondary viewport, captured from the screen (only meaningful with --no-exclude, which
// --screenshot implies).
bool SaveScreenshot(App& app, const char* path) {
    RECT u;
    GetWindowRect(app.win.Hwnd(), &u);
    for (ImGuiViewport* vp : ImGui::GetPlatformIO().Viewports) {
        if (vp == ImGui::GetMainViewport() || !vp->PlatformWindowCreated) continue;
        const RECT r{static_cast<LONG>(vp->Pos.x), static_cast<LONG>(vp->Pos.y), static_cast<LONG>(vp->Pos.x + vp->Size.x),
                     static_cast<LONG>(vp->Pos.y + vp->Size.y)};
        UnionRect(&u, &u, &r);
    }
    InflateRect(&u, 16, 16);
    const int w = u.right - u.left, h = u.bottom - u.top;
    HDC screen = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mdc, bmp);
        ok = BitBlt(mdc, 0, 0, w, h, screen, u.left, u.top, SRCCOPY | CAPTUREBLT) != FALSE;
        SelectObject(mdc, old);
        if (ok) {
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                rgba[i * 4 + 0] = src[i * 4 + 2];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 0];
                rgba[i * 4 + 3] = 255;
            }
            ok = platform::ImageIo::WritePngRgba(path, rgba.data(), w, h, w * 4);
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(nullptr, screen);
    CT_LOG_INFO("bar screenshot %s: %s (%dx%d)", ok ? "saved" : "FAILED", path, w, h);
    return ok;
}

bool RenderFrame(App& app) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    app.bar.Frame();
    // Secondary platform windows are cleared to black by the renderer; fill them with the popover colour so the
    // rounded ImGui background never shows dark corners inside the DWM-rounded window.
    for (ImGuiViewport* vp : ImGui::GetPlatformIO().Viewports)
        if (vp != ImGui::GetMainViewport())
            ImGui::GetBackgroundDrawList(vp)->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), col::bg2);
    ImGui::Render();
    {
        platform::D3DDevice::Lock lock(app.dev);
        ID3D11DeviceContext4* ctx = app.dev.Context();
        app.bar.RenderGpu(ctx);
        ID3D11RenderTargetView* rtv = app.sc.Rtv();
        const ImVec4 c = ToVec4(col::bg1);
        const float clear[4] = {c.x, c.y, c.z, 1.0f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
    const HRESULT hr = app.sc.Present(true);
    ++app.redraws;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        CT_LOG_ERROR("Present: %s", HrToString(hr).c_str());
        return false;
    }
    return true;
}

void SendChord(WORD vk) {
    const WORD mods[] = {VK_CONTROL, VK_MENU, VK_SHIFT};
    INPUT in[8]{};
    int n = 0;
    for (WORD m : mods) {
        in[n].type = INPUT_KEYBOARD;
        in[n++].ki.wVk = m;
    }
    in[n].type = INPUT_KEYBOARD;
    in[n++].ki.wVk = vk;
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = vk;
    in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    for (int i = 2; i >= 0; --i) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = mods[i];
        in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(static_cast<UINT>(n), in, sizeof(INPUT));
}

// --bench-hotkey: real RegisterHotKey/WM_HOTKEY path with an unused chord, countdown 0.
struct HotkeyBench {
    int runs = 0, done = 0, phase = 0;   // 0 wait idle, 1 sent start, 2 recording, 3 sent stop
    int64_t phase_qpc = 0, send_qpc = 0;
    std::vector<double> totals, from_hotkey;
    void Step(App& app) {
        const int64_t now = Clock::QpcNow();
        const double since_ms = static_cast<double>(now - phase_qpc) * 1000.0 / Clock::QpcFrequency();
        switch (phase) {
        case 0:
            if (app.bar.IsIdle() && since_ms > 1500) {
                send_qpc = Clock::QpcNow();
                SendChord(VK_F22);
                phase = 1;
                phase_qpc = now;
            }
            break;
        case 1:
            if (app.bar.LastStartLatencyMs() > 0) {
                const double total = static_cast<double>(app.bar.LastTriggerQpc() - send_qpc) * 1000.0 / Clock::QpcFrequency() +
                                     app.bar.LastStartLatencyMs();
                totals.push_back(total);
                from_hotkey.push_back(app.bar.LastStartLatencyMs());
                std::printf("hotkey -> first frame: %.1f ms (start trigger -> first frame %.1f ms; the trigger is the hotkey, or the "
                            "end of the countdown)\n", total, app.bar.LastStartLatencyMs());
                phase = 2;
                phase_qpc = now;
            } else if (since_ms > 10000) {
                std::printf("hotkey bench: no recording started (hotkey not delivered?)\n");
                app.quit = true;
            }
            break;
        case 2:
            if (since_ms > 1000) {
                SendChord(VK_F22);
                phase = 3;
                phase_qpc = now;
            }
            break;
        case 3:
            if (app.bar.IsIdle()) {
                phase = 0;
                phase_qpc = now;
                if (++done >= runs) {
                    std::sort(totals.begin(), totals.end());
                    if (!totals.empty())
                        std::printf("hotkey -> first frame over %zu runs: min %.1f, median %.1f, max %.1f ms\n", totals.size(),
                                    totals.front(), totals[totals.size() / 2], totals.back());
                    app.quit = true;
                }
            }
            break;
        default: break;
        }
    }
};

void LaunchEditor(const std::string& path) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = L"--editor";
    if (!path.empty()) args += L" \"" + Utf8ToWide(path) + L"\"";
    // The editor runs as its own process until bar and editor share one (main.cpp skips the single-instance check).
    SetEnvironmentVariableW(L"CAMTESIA_SEPARATE_INSTANCE", L"1");
    ShellExecuteW(nullptr, L"open", exe, args.c_str(), nullptr, SW_SHOWNORMAL);
    SetEnvironmentVariableW(L"CAMTESIA_SEPARATE_INSTANCE", nullptr);
}

} // namespace

bool RunsAsSeparateInstance(const wchar_t* cmdline) {
    if (GetEnvironmentVariableW(L"CAMTESIA_SEPARATE_INSTANCE", nullptr, 0) > 0) return true;
    if (!cmdline) return false;
    return wcsstr(cmdline, L"--screenshot") || wcsstr(cmdline, L"--time-to-window") || wcsstr(cmdline, L"--bench-hotkey");
}

int RunRecorderApp(const RecorderAppOptions& options) {
    const Extra extra = ParseExtra();
    App* app_ptr = new App();
    App& app = *app_ptr;
    g_app = &app;
    const bool screenshot = !options.screenshot.empty();
    app.exclude = !(extra.no_exclude || screenshot);
    app.icon = options.icon;
    int exit_code = 1;

#if defined(CT_DEBUG)
    const bool debug_layer = true;
#else
    const bool debug_layer = false;
#endif
    HRESULT hr = app.dev.Init(debug_layer);
    if (FAILED(hr)) {
        CT_LOG_ERROR("no usable D3D11 device: %s", HrToString(hr).c_str());
        MessageBoxW(nullptr, L"Camtesia needs a Direct3D 11 capable graphics driver.", L"Camtesia", MB_ICONERROR);
        delete app_ptr;
        g_app = nullptr;
        return 1;
    }

    const std::string settings_path = SettingsFilePath();
    const bool first_run = IsFirstRun();
    Settings settings = Settings::Defaults();
    {
        Err err;
        if (!settings.Load(settings_path, &err)) CT_LOG_WARN("settings: %s (using defaults)", err.message.c_str());
    }
    HotkeyBench bench;
    bench.runs = extra.bench_hotkey;
    std::string temp_out;
    if (bench.runs > 0 || screenshot) {
        // Diagnostics record into a temp folder (deleted at exit), never into the user's recordings.
        temp_out = fs::TempPath("");
        settings.recording.output_dir = temp_out;
        setvbuf(stdout, nullptr, _IONBF, 0);
    }
    if (bench.runs > 0) settings.hotkeys.toggle = Hotkey{MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F22};

    platform::Window::Desc desc;
    desc.class_name = L"CamtesiaMain";   // single-instance forwarding finds this window
    desc.title = L"Camtesia";
    desc.w_dip = 560;
    desc.h_dip = 64;
    desc.frameless = true;
    desc.topmost = true;
    desc.tool_window = true;
    desc.resizable = false;
    desc.icon = options.icon;
    desc.quit_on_destroy = true;
    if (app.win.Create(desc, &Handler, &app) && SUCCEEDED(app.sc.Init(app.dev, app.win.Hwnd()))) {
        Async::SetUiPoster(&PostToUi, app.win.Hwnd());

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigViewportsNoAutoMerge = true;     // popovers/toasts always get their own (excluded) window
        io.ConfigViewportsNoTaskBarIcon = true;
        io.ConfigViewportsNoDecoration = true;
        io.IniFilename = nullptr;
        Theme::Apply(ImGui::GetStyle());
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowBorderSize = 0;   // DWM draws the border of every platform window
        st.PopupBorderSize = 0;
        Fonts::Load(*io.Fonts);
        app.base_style = ImGui::GetStyle();
        ImGui_ImplWin32_Init(app.win.Hwnd());
        ImGui_ImplDX11_Init(app.dev.Device(), app.dev.Context());
        g_orig_create_window = ImGui::GetPlatformIO().Platform_CreateWindow;
        ImGui::GetPlatformIO().Platform_CreateWindow = &CreateViewportWindow;
        ApplyDpi(app);

        RecorderBar::Options bo;
        bo.start_hidden = options.tray;
        bo.exclude_from_capture = app.exclude;
        bo.synthetic_camera = extra.synthetic_camera;
        bo.countdown_override = extra.countdown >= 0 ? extra.countdown : (bench.runs > 0 || extra.bar_record ? 0 : -1);
        RecorderBar::Hooks hooks = options.hooks;
        if (!hooks.open_editor) hooks.open_editor = &LaunchEditor;
        hooks.quit = [&app]() { app.quit = true; };
        // Diagnostics runs never write the user's settings (the hotkey bench uses its own chord).
        app.bar.Init(app.dev, app.win, settings, screenshot || bench.runs > 0 ? std::string() : settings_path, bo, std::move(hooks));
        if (!options.tray) ShowWindow(app.win.Hwnd(), SW_SHOWNOACTIVATE);
        if (first_run && !screenshot && bench.runs == 0 && FirstRun::Run(nullptr, settings, settings_path)) app.bar.PushToast(str::kFirstRunToast, 12.0f);

        HANDLE stats_timer = nullptr;
        if (options.stats) {
            stats_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            LARGE_INTEGER due;
            due.QuadPart = -10'000'000LL;
            SetWaitableTimer(stats_timer, &due, 1000, nullptr, nullptr, FALSE);
        }
        bool first_frame = true;
        int frames_requested = 2;
        int frames_rendered = 0;
        int64_t next_due_qpc = 0;
        int64_t script_t0 = 0;
        bool shot_taken = false, script_started = false;
        bench.phase_qpc = Clock::QpcNow();
        while (!app.quit) {
            DWORD timeout = INFINITE;
            const int interval = app.bar.RedrawIntervalMs();
            if (frames_requested > 0 || screenshot) timeout = 0;
            else if (bench.runs > 0) timeout = 20;
            else if (interval > 0) {
                const int64_t left = (next_due_qpc - Clock::QpcNow()) * 1000 / Clock::QpcFrequency();
                timeout = static_cast<DWORD>(std::clamp<int64_t>(left, 0, interval));
            }
            HANDLE handles[1] = {stats_timer};
            const DWORD wait = MsgWaitForMultipleObjectsEx(stats_timer ? 1 : 0, stats_timer ? handles : nullptr, timeout,
                                                           QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (stats_timer && wait == WAIT_OBJECT_0) {
                std::printf("redraws/s: %u  messages/s: %u (last 0x%04X)\n", app.redraws, app.messages, app.last_message);
                CT_LOG_INFO("stats: redraws/s: %u messages/s: %u (last 0x%04X)", app.redraws, app.messages, app.last_message);
                app.redraws = 0;
                app.messages = 0;
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
                ++app.messages;
                app.last_message = msg.message;
            }
            if (app.quit) break;
            Log::Tick();
            if (bench.runs > 0) bench.Step(app);

            const bool due = interval > 0 && Clock::QpcNow() >= next_due_qpc;
            if (frames_requested > 0 || due || screenshot) {
                if (app.win.ConsumeResize()) CT_LOG_IF_FAILED(app.sc.Resize(app.win.ClientSizePx()));
                WaitForSingleObject(app.sc.WaitableObject(), 100);
                if (!RenderFrame(app)) break;
                ++frames_rendered;
                if (frames_requested > 0) --frames_requested;
                if (interval > 0) next_due_qpc = Clock::QpcNow() + static_cast<int64_t>(interval) * Clock::QpcFrequency() / 1000;
                if (first_frame) {
                    first_frame = false;
                    const double ms = MsSinceProcessStart();
                    CT_LOG_INFO("time-to-window (recorder bar): %.1f ms", ms);
                    if (options.time_to_window) {
                        std::printf("time-to-window: %.1f ms\n", ms);
                        app.quit = true;
                    }
                }
            }

            // --screenshot scripts.
            if (screenshot && !shot_taken) {
                if (!script_started && frames_rendered >= 2) {
                    script_started = true;
                    script_t0 = Clock::QpcNow();
                    if (extra.open != RecorderBar::Popup::None) app.bar.OpenPopup(extra.open);
                    if (extra.bar_toast) app.bar.ShowSavedToast(platform::Paths::DefaultRecordingsDir() + "\\2026-10-17 12-03-44.ctrec");
                    if (extra.bar_record) app.bar.ToggleRecording();
                    if (extra.bar_settings) app.bar.OpenSettings();
                }
                const double t_ms = script_started ? static_cast<double>(Clock::QpcNow() - script_t0) * 1000.0 / Clock::QpcFrequency() : 0;
                const double wait_ms = extra.bar_record ? 3500 : 1500;   // device lists and caps load asynchronously
                if (script_started && t_ms >= wait_ms && frames_rendered >= options.frames) {
                    SaveScreenshot(app, options.screenshot.c_str());
                    shot_taken = true;
                    if (extra.bar_record) app.bar.CancelRecording();
                }
            }
            if (shot_taken && app.bar.IsIdle()) app.quit = true;
        }
        if (stats_timer) CloseHandle(stats_timer);
        exit_code = 0;

        app.bar.Shutdown();
        // Late posts (session callbacks, async jobs) are dropped from now on; the queued ones are deleted unrun.
        app.drop_invokes = true;
        MSG msg;
        while (PeekMessageW(&msg, app.win.Hwnd(), kInvokeMsg, kInvokeMsg, PM_REMOVE)) delete reinterpret_cast<std::function<void()>*>(msg.lParam);
        Async::SetUiPoster([](void*, std::function<void()>* fn) { delete fn; }, nullptr);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        Fonts::Unload();
        ImGui::DestroyContext();
    } else {
        CT_LOG_ERROR("recorder bar window or swapchain creation failed");
    }
    app.sc.Shutdown();
    app.win.Destroy();
    app.dev.Shutdown();
    app.dev.ReportLiveObjects();
    if (!temp_out.empty()) fs::DeleteTree(temp_out);
    g_app = nullptr;
    delete app_ptr;
    return exit_code;
}

} // namespace ct::ui
