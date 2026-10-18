// Editor mode of Camtesia.exe: window, device, ImGui, event loop, drag and drop, forwarding, measurement modes.
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/editor_app.h>

#include <ct/audio/devices.h>
#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/paths.h>
#include <ct/platform/single_instance.h>
#include <ct/platform/win.h>
#include <ct/platform/window.h>
#include <ct/ui/editor_shell.h>
#include <ct/ui/editor_window.h>
#include <ct/ui/fonts.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ct::ui {

namespace {

constexpr UINT kWmInvoke = WM_APP + 0x31;   // lParam = std::function<void()>* from Async::PostToUi

using platform::D3DDevice;
using platform::SwapChain;
using platform::Window;

int64_t QpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}
double QpcFreq() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}

void OpenConsole() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN && GetFileType(out) != FILE_TYPE_CHAR) {
        setvbuf(stdout, nullptr, _IONBF, 0);
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
}

struct Percentiles {
    double p50 = 0, p95 = 0, max = 0, mean = 0;
};
Percentiles Stats(std::vector<double> v) {
    Percentiles p;
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    p.p50 = v[v.size() / 2];
    p.p95 = v[std::min(v.size() - 1, v.size() * 95 / 100)];
    p.max = v.back();
    double s = 0;
    for (double x : v) s += x;
    p.mean = s / static_cast<double>(v.size());
    return p;
}

struct App {
    Window win;
    D3DDevice dev;
    SwapChain sc;
    audio::AudioDevices devices;
    bool devices_ok = false;
    EditorWindow editor;
    ImGuiStyle base_style;
    std::string ini_path;
    bool quit = false;
    bool closing = false;
    uint32_t redraws_this_second = 0;
    uint32_t redraws_last_second = 0;
    double last_frame_ms = 0;
    double qpc_freq = 1;
    // Bench (P09 9.7).
    EditorLaunch launch;
    int bench_frame = 0;
    std::vector<double> frame_ms;    // wall time between presents
    std::vector<double> cpu_ms;      // RenderOneFrame CPU time (incl. present wait)
    std::vector<double> build_ms;    // ImGui NewFrame..Render (UI CPU)
    int64_t last_present_qpc = 0;
    std::vector<double> scrub_cold, scrub_cached;
    int scrub_phase = 0;             // 0 waiting for media, 1 cold seeks, 2 cached seeks, 3 done
    int scrub_i = 0;
    bool scrub_waiting = false;
    int64_t scrub_settle_until = 0;
    int64_t scrub_seek_qpc = 0;      // when the pending seek was issued
    int scrub_timeouts = 0;          // seeks not exact within 10 s
    std::mt19937 rng{12345};
    int64_t bench_start_qpc = 0;
    bool bench_done = false;
    // --bench-drag sanity: the dragged clip, where it started, and how many frames had a drag active.
    project::Id drag_clip;
    TimeUs drag_clip_start = -1;
    int drag_active_frames = 0;
    int drag_moves = 0;              // frames in which the dragged clip's start changed
    TimeUs drag_last_start = -1;
};

App* g_app = nullptr;

void PostToUi(void* user, std::function<void()>* fn) {
    HWND hwnd = static_cast<HWND>(user);
    if (!hwnd || !PostMessageW(hwnd, kWmInvoke, 0, reinterpret_cast<LPARAM>(fn))) delete fn;
}

void ApplyDpi(App& app) {
    const float scale = app.win.DpiScale();
    ImGuiStyle& style = ImGui::GetStyle();
    style = app.base_style;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    CT_LOG_INFO("editor ui scale %.2f (dpi %u)", scale, app.win.Dpi());
}

std::vector<std::string> DroppedFiles(HDROP drop) {
    std::vector<std::string> out;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring w(len + 1, L'\0');
        DragQueryFileW(drop, i, w.data(), len + 1);
        w.resize(len);
        out.push_back(WideToUtf8(w));
    }
    DragFinish(drop);
    return out;
}

LRESULT Handler(void* user, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, bool& handled) {
    App& app = *static_cast<App*>(user);
    if (msg == kWmInvoke) {
        auto* fn = reinterpret_cast<std::function<void()>*>(lparam);
        if (fn) {
            (*fn)();
            delete fn;
        }
        handled = true;
        return 0;
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        handled = true;
        return 1;
    }
    switch (msg) {
    case WM_DPICHANGED:
        if (ImGui::GetCurrentContext()) ApplyDpi(app);
        break;
    case WM_CLOSE:
        if (!app.closing && !app.editor.CloseRequested()) {
            handled = true;   // the unsaved-changes prompt is up
            return 0;
        }
        break;
    case WM_DROPFILES: {
        std::vector<std::string> paths = DroppedFiles(reinterpret_cast<HDROP>(wparam));
        CT_LOG_INFO("dropped %zu path(s)", paths.size());
        app.editor.OpenPaths(paths);
        handled = true;
        return 0;
    }
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
        if (cds && cds->dwData == platform::SingleInstance::kCopyDataCommandLine && cds->lpData) {
            const size_t chars = cds->cbData / sizeof(wchar_t);
            std::wstring cmd(static_cast<const wchar_t*>(cds->lpData), chars);
            while (!cmd.empty() && cmd.back() == L'\0') cmd.pop_back();
            CT_LOG_INFO("forwarded command line: %ls", cmd.c_str());
            EditorLaunch l;
            if (ParseEditorLaunch(cmd.c_str(), &l)) app.editor.OpenPaths(l.paths);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            else if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            handled = true;
            return TRUE;
        }
        break;
    }
    default: break;
    }
    return 0;
}

// --- bench input (synthetic mouse after the Win32 backend's own events, so they win) ------------------------------

void BenchInput(App& app) {
    const EditorLaunch& L = app.launch;
    EditorState& s = app.editor.State();
    ImGuiIO& io = ImGui::GetIO();
    ++app.bench_frame;
    if (L.bench == "--bench-drag") {
        const int warm = 60;
        if (app.bench_frame < warm) return;
        if (app.bench_frame == warm) {
            // Select nothing, pick a clip in the middle of the visible lanes.
            app.editor.Shell().Timeline().Fit(s);
            return;
        }
        static project::Id clip;
        static ImVec2 start;
        const int f = app.bench_frame - warm - 1;
        if (f == 0) {
            // The last clip of the first track has free time after it, so every frame of the drag really moves it
            // (in the middle of a packed track each +-150 px position overlaps a neighbour and the move is refused).
            // Zoom so clips are wide enough to grab and scroll so that clip sits ~400 px into the lanes.
            app.editor.Shell().Timeline().ZoomBy(s, 16.0, s.project.Duration() / 2);
            const project::Clip* last = nullptr;
            if (!s.project.tracks.empty())
                for (const auto& c : s.project.clips)
                    if (c.track == s.project.tracks.front().id && (!last || c.start_us > last->start_us)) last = &c;
            if (last) {
                clip = last->id;
                s.scroll_us = std::max<TimeUs>(0, last->start_us - static_cast<TimeUs>(400.0 / std::max(1e-6, s.pps) * 1e6));
            }
            return;
        }
        if (f == 1) {
            ImVec2 mn, mx;
            const bool visible = !clip.empty() && app.editor.Shell().Timeline().ClipRect(s, clip, &mn, &mx) && mx.x - mn.x > 40 &&
                                 mn.x > 200 && mn.x + 340 < io.DisplaySize.x && mn.y > 0 && mx.y < io.DisplaySize.y;
            if (visible) start = ImVec2(mn.x + 20, (mn.y + mx.y) * 0.5f);   // body, clear of the 8 px trim handle
            else {
                std::printf("bench-drag: clip %s rect (%.0f,%.0f)-(%.0f,%.0f), display %.0fx%.0f, pps %.2f, scroll %.2f s\n", clip.c_str(), mn.x, mn.y,
                            mx.x, mx.y, io.DisplaySize.x, io.DisplaySize.y, s.pps, static_cast<double>(s.scroll_us) / 1e6);
                clip.clear();
            }
            if (clip.empty()) {
                std::printf("bench-drag: no clip visible to drag\n");
                app.bench_done = true;
                return;
            }
            io.AddMousePosEvent(start.x, start.y);
            return;
        }
        if (f == 2) {
            io.AddMousePosEvent(start.x, start.y);
            io.AddMouseButtonEvent(0, true);
            app.drag_clip = clip;
            if (const project::Clip* c = s.project.FindClip(clip)) app.drag_clip_start = c->start_us;
            app.frame_ms.clear();
            app.cpu_ms.clear();
            app.build_ms.clear();
            return;
        }
        const int n = std::max(60, L.bench_count);
        if (s.DragActive()) ++app.drag_active_frames;
        if (const project::Clip* c = s.project.FindClip(app.drag_clip)) {
            if (app.drag_last_start >= 0 && c->start_us != app.drag_last_start) ++app.drag_moves;
            app.drag_last_start = c->start_us;
        }
        if (f < n + 3) {
            // Triangle wave 0..+300 px (into the free time after the clip), 3 px per frame.
            const int k = (f - 3) % 200;
            const float dx = static_cast<float>(k < 100 ? k * 3 : (200 - k) * 3);
            io.AddMousePosEvent(start.x + dx, start.y);
            io.AddMouseButtonEvent(0, true);   // re-assert (ignored while already down): nothing else may release it
            return;
        }
        io.AddMouseButtonEvent(0, false);
        app.bench_done = true;
    } else if (L.bench == "--bench-play") {
        if (app.bench_frame == 90) {
            s.playback->Seek(0);
            s.playback->Play(1.0);
            app.frame_ms.clear();
            app.cpu_ms.clear();
            app.build_ms.clear();
            app.bench_start_qpc = QpcNow();
        }
        if (app.bench_frame > 90 && static_cast<double>(QpcNow() - app.bench_start_qpc) / app.qpc_freq > std::max(2, L.bench_count)) {
            s.playback->Pause();
            app.bench_done = true;
        }
        // Loop over the project.
        if (app.bench_frame > 90 && !s.playback->Playing() && !app.bench_done) {
            s.playback->Seek(0);
            s.playback->Play(1.0);
        }
    } else if (L.bench == "--bench-scrub") {
        const TimeUs dur = s.project.Duration();
        const EditorWindow::Stats& st = app.editor.GetStats();
        if (app.scrub_phase == 0) {
            if (app.bench_frame > 120 && st.pending_opens == 0 && dur > 0) app.scrub_phase = 1;
            return;
        }
        if (app.scrub_waiting) {   // the latency callback clears it
            // A seek whose exact frame never lands is reported, not waited for forever (the 9.7 hang).
            if (static_cast<double>(QpcNow() - app.scrub_seek_qpc) / app.qpc_freq > 10.0) {
                ++app.scrub_timeouts;
                app.scrub_waiting = false;
                ++app.scrub_i;
                std::printf("bench-scrub: seek %d to %.3f s not exact after 10 s\n", app.scrub_i, static_cast<double>(s.Playhead()) / 1e6);
            }
            return;
        }
        if (QpcNow() < app.scrub_settle_until) return;
        const int n = std::max(5, L.bench_count);
        const TimeUs frame = s.FrameUs();
        if (app.scrub_phase == 1) {
            if (app.scrub_i >= n) {
                app.scrub_phase = 2;
                app.scrub_i = 0;
                return;
            }
            // Cold: a random time away from the current playhead, outside its paused prefetch window
            // ([t - 0.25 s, t + 0.5 s]): 3 s, or a third of a short project.
            const TimeUs away = std::min<TimeUs>(3'000'000, dur / 3);
            TimeUs t = 0;
            for (int tries = 0; tries < 50; ++tries) {
                t = static_cast<TimeUs>(std::uniform_real_distribution<double>(0, static_cast<double>(dur - frame))(app.rng));
                if (std::llabs(t - s.Playhead()) > away) break;
            }
            s.playback->Seek(t);
            app.scrub_waiting = true;
            app.scrub_seek_qpc = QpcNow();
        } else if (app.scrub_phase == 2) {
            if (app.scrub_i >= n) {
                app.scrub_phase = 3;
                app.bench_done = true;
                return;
            }
            // Cached: one frame forward / back from a playhead whose neighbourhood the paused prefetch filled.
            TimeUs t = std::clamp<TimeUs>(s.Playhead() + ((app.scrub_i % 2) ? -frame : frame), 0, dur - frame);
            if (t == s.Playhead()) t = std::clamp<TimeUs>(s.Playhead() + ((app.scrub_i % 2) ? frame : -frame), 0, dur - frame);   // at an end
            s.playback->Seek(t);
            app.scrub_waiting = true;
            app.scrub_seek_qpc = QpcNow();
        }
    }
}

bool RenderOneFrame(App& app) {
    const int64_t start = QpcNow();
    app.editor.BeforeFrame();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (!app.launch.bench.empty()) BenchInput(app);
    ImGui::NewFrame();
    app.editor.Shell().status.frame_ms = app.last_frame_ms;
    app.editor.SetRedrawsPerSecond(app.redraws_last_second);
    app.editor.Frame();
    ImGui::Render();
    const int64_t built = QpcNow();
    {
        D3DDevice::Lock lock(app.dev);
        ID3D11DeviceContext4* ctx = app.dev.Context();
        ID3D11RenderTargetView* rtv = app.sc.Rtv();
        const float clear[4] = {0x0F / 255.0f, 0x11 / 255.0f, 0x15 / 255.0f, 1.0f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    const HRESULT hr = app.sc.Present(true);
    const int64_t end = QpcNow();
    app.last_frame_ms = static_cast<double>(end - start) * 1000.0 / app.qpc_freq;
    if (!app.launch.bench.empty()) {
        if (app.last_present_qpc) app.frame_ms.push_back(static_cast<double>(end - app.last_present_qpc) * 1000.0 / app.qpc_freq);
        app.cpu_ms.push_back(app.last_frame_ms);
        app.build_ms.push_back(static_cast<double>(built - start) * 1000.0 / app.qpc_freq);
    }
    app.last_present_qpc = end;
    ++app.redraws_this_second;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        CT_LOG_ERROR("Present: %s, removed reason %s", HrToString(hr).c_str(), HrToString(app.dev.CheckRemoved()).c_str());
        return false;
    }
    return true;
}

void ReportBench(App& app) {
    const EditorLaunch& L = app.launch;
    const EditorWindow::Stats& st = app.editor.GetStats();
    if (L.bench == "--bench-drag" || L.bench == "--bench-play") {
        const Percentiles f = Stats(app.frame_ms), c = Stats(app.cpu_ms), b = Stats(app.build_ms);
        const double fps = f.mean > 0 ? 1000.0 / f.mean : 0;
        std::printf("%s: %zu frames, %.1f fps (frame p50 %.2f p95 %.2f max %.2f ms), UI build p50 %.2f p95 %.2f ms, frame CPU+wait p95 %.2f ms\n",
                    L.bench.c_str(), app.frame_ms.size(), fps, f.p50, f.p95, f.max, b.p50, b.p95, c.p95);
        int over = 0;
        for (double x : app.frame_ms) over += x > 1000.0 / 60.0 * 1.5 ? 1 : 0;
        std::printf("  frames longer than 25 ms: %d, clips %zu, undo entries %zu\n", over, app.editor.State().project.clips.size(),
                    app.editor.State().undo.UndoEntries());
        if (L.bench == "--bench-drag") {
            const project::Clip* dc = app.editor.State().project.FindClip(app.drag_clip);
            std::printf("  dragged clip %s: start %.3f s -> %.3f s, drag active in %d frames\n", app.drag_clip.c_str(),
                        static_cast<double>(app.drag_clip_start) / 1e6, dc ? static_cast<double>(dc->start_us) / 1e6 : -1.0,
                        app.drag_active_frames);
            std::printf("  clip moved in %d frames%s\n", app.drag_moves,
                        app.drag_moves * 2 < static_cast<int>(app.frame_ms.size()) ? " -- INVALID: the drag did not edit in most frames" : "");
        }
        if (L.bench == "--bench-play")
            std::printf("  preview: %llu frames rendered while playing, %llu without every exact frame, render CPU last %.2f ms\n",
                        static_cast<unsigned long long>(st.playing_frames), static_cast<unsigned long long>(st.frames_inexact_playing), st.last_render_cpu_ms);
    } else if (L.bench == "--bench-scrub") {
        const Percentiles a = Stats(app.scrub_cold), b = Stats(app.scrub_cached);
        std::printf("bench-scrub: cold %zu seeks p50 %.1f p95 %.1f max %.1f ms; cached %zu seeks p50 %.1f p95 %.1f max %.1f ms\n",
                    app.scrub_cold.size(), a.p50, a.p95, a.max, app.scrub_cached.size(), b.p50, b.p95, b.max);
        std::printf("  seeks not exact within 10 s: %d\n", app.scrub_timeouts);
    }
    CT_LOG_INFO("bench %s done", L.bench.c_str());
}

int Loop(App& app) {
    const EditorLaunch& L = app.launch;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -10'000'000LL;
    SetWaitableTimer(timer, &due, 1000, nullptr, nullptr, FALSE);
    HANDLE handles[1] = {timer};
    int screenshot_frames = L.frames;
    int frames_requested = 2;
    const bool forced = !L.screenshot.empty() || !L.bench.empty();
    while (!app.quit) {
        const DWORD timeout = frames_requested > 0 ? 0 : INFINITE;
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) {
            // 1 Hz: autosave tick (no redraw) and --stats.
            app.editor.OnTimer();
            app.redraws_last_second = app.redraws_this_second;
            if (L.stats) std::printf("redraws/s: %u\n", app.redraws_this_second);
            app.redraws_this_second = 0;
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
        }
        if (app.quit) break;
        if (frames_requested > 0 && !app.win.IsMinimized()) {
            WaitForSingleObject(app.sc.WaitableObject(), 1000);
            if (app.win.ConsumeResize()) CT_LOG_IF_FAILED(app.sc.Resize(app.win.ClientSizePx()));
            if (!RenderOneFrame(app)) break;
            --frames_requested;
            if (app.editor.WantsContinuousRedraw() || forced) frames_requested = std::max(frames_requested, 1);
            if (!L.screenshot.empty() && --screenshot_frames <= 0) {
                SaveWindowScreenshot(app.win.Hwnd(), L.screenshot.c_str());
                app.closing = true;
                PostMessageW(app.win.Hwnd(), WM_CLOSE, 0, 0);
            }
            if (!L.bench.empty() && app.bench_done) {
                ReportBench(app);
                app.launch.bench.clear();
                app.closing = true;
                PostMessageW(app.win.Hwnd(), WM_CLOSE, 0, 0);
            }
        } else if (frames_requested > 0) {
            frames_requested = 0;
        }
    }
    if (timer) CloseHandle(timer);
    return 0;
}

} // namespace

bool ParseEditorLaunch(const wchar_t* cmdline, EditorLaunch* out) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmdline ? cmdline : L"", &argc);
    bool editor = false, demo = false;
    for (int i = 1; argv && i < argc; ++i) {
        const wchar_t* a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? WideToUtf8(argv[++i]) : std::string(); };
        if (wcscmp(a, L"--editor") == 0) editor = true;
        else if (wcscmp(a, L"--editor-demo") == 0) demo = true;
        else if (wcscmp(a, L"--project") == 0) ++i;   // --editor-demo's value
        else if (wcscmp(a, L"--screenshot") == 0) out->screenshot = next();
        else if (wcscmp(a, L"--frames") == 0) out->frames = std::atoi(next().c_str());
        else if (wcscmp(a, L"--stats") == 0) out->stats = true;
        else if (wcscmp(a, L"--seek") == 0) out->seek_s = std::atof(next().c_str());
        else if (wcscmp(a, L"--select-clip") == 0) out->select_clip = next();
        else if (wcscmp(a, L"--overlay") == 0) out->show_stats_overlay = true;
        else if (wcscmp(a, L"--open-export") == 0) {
            out->open_export = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') out->export_preset = WideToUtf8(argv[++i]);
        }
        else if (wcscmp(a, L"--fit") == 0) out->fit_mode = std::atoi(next().c_str());
        else if (wcscmp(a, L"--size") == 0) {
            const std::string s = next();
            std::sscanf(s.c_str(), "%dx%d", &out->width_dip, &out->height_dip);
        } else if (wcscmp(a, L"--bench-drag") == 0 || wcscmp(a, L"--bench-scrub") == 0 || wcscmp(a, L"--bench-play") == 0) {
            out->bench = WideToUtf8(a);
            if (i + 1 < argc && argv[i + 1][0] != L'-') out->bench_count = std::atoi(WideToUtf8(argv[++i]).c_str());
        } else if (a[0] == L'-' && a[1] == L'-') {
            continue;   // other modes' options (--project, --tray, ...)
        } else {
            editor = true;   // bare file argument (Explorer "Open with", the .ctproj association)
            out->paths.push_back(WideToUtf8(a));
        }
    }
    if (argv) LocalFree(argv);
    return editor && !demo;
}

int RunEditorApp(void* instance, int show_cmd, const EditorLaunch& launch) {
    App* app_ptr = new App();
    App& app = *app_ptr;
    g_app = &app;
    app.launch = launch;
    app.qpc_freq = QpcFreq();
    if (launch.stats || !launch.bench.empty()) OpenConsole();
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
        return 1;
    }
    app.devices_ok = SUCCEEDED(app.devices.Init());
    Window::Desc desc;
    const std::wstring window_class = platform::SingleInstance::MainWindowClass();   // + CT_INSTANCE_SUFFIX (dev/test)
    desc.class_name = window_class.c_str();   // SingleInstance forwards to this class
    desc.title = L"Camtesia";
    desc.w_dip = launch.width_dip;
    desc.h_dip = launch.height_dip;
    desc.resizable = true;
    desc.icon = LoadIconW(static_cast<HINSTANCE>(instance), MAKEINTRESOURCEW(101));   // IDI_APP (src/app/resource.h)
    desc.quit_on_destroy = true;
    desc.min_client_dip = SizeI{640, 400};
    if (app.win.Create(desc, &Handler, &app) && SUCCEEDED(app.sc.Init(app.dev, app.win.Hwnd()))) {
        app.win.SetDarkTitleBar(true);
        DragAcceptFiles(app.win.Hwnd(), TRUE);
        // WM_DROPFILES / WM_COPYDATA from a non-elevated Explorer into an elevated editor.
        ChangeWindowMessageFilterEx(app.win.Hwnd(), WM_DROPFILES, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(app.win.Hwnd(), WM_COPYDATA, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(app.win.Hwnd(), 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
        Async::SetUiPoster(&PostToUi, app.win.Hwnd());

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        // Layout persistence (docs/phases/P09 9.1). Screenshots and benches use the default layout.
        app.ini_path = platform::Paths::SettingsDir() + "\\editor_layout.ini";
        io.IniFilename = (launch.screenshot.empty() && launch.bench.empty()) ? app.ini_path.c_str() : nullptr;
        // Benches drive a synthetic mouse; the window often is not (or stops being) the foreground window when launched
        // from a script, and the Win32 backend's focus-loss event would release the synthetic button mid-drag.
        if (!launch.bench.empty()) io.ConfigDebugIgnoreFocusLoss = true;
        EditorShell::SetupImGui();
        app.base_style = ImGui::GetStyle();
        ImGui_ImplWin32_Init(app.win.Hwnd());
        ImGui_ImplDX11_Init(app.dev.Device(), app.dev.Context());
        ApplyDpi(app);

        app.editor.on_close_confirmed = [&app] {
            app.closing = true;
            PostMessageW(app.win.Hwnd(), WM_CLOSE, 0, 0);
        };
        app.editor.Create(app.win.Hwnd(), app.dev, app.devices_ok ? &app.devices : nullptr);
        app.editor.on_scrub_latency = [&app](double ms) {
            if (!app.scrub_waiting) return;
            app.scrub_waiting = false;
            if (app.scrub_phase == 1) app.scrub_cold.push_back(ms);
            else if (app.scrub_phase == 2) app.scrub_cached.push_back(ms);
            ++app.scrub_i;
            // Let the paused prefetch fill the window around the new playhead before the next cached step.
            app.scrub_settle_until = QpcNow() + static_cast<int64_t>(app.qpc_freq * (app.scrub_phase == 2 ? 0.15 : 0.05));
        };
        if (!launch.paths.empty()) app.editor.OpenPaths(launch.paths);
        if (launch.show_stats_overlay) app.editor.Shell().Run(Action::ToggleStats);
        if (launch.fit_mode >= 0) app.editor.Shell().Preview().fit_mode = std::clamp(launch.fit_mode, 0, 3);
        if (launch.seek_s >= 0 || !launch.select_clip.empty() || launch.open_export) {
            // After the files opened (their posts run before this one is dequeued).
            const double seek = launch.seek_s;
            const std::string sel = launch.select_clip;
            const bool open_export = launch.open_export;
            const std::string preset = launch.export_preset;
            EditorWindow* ed = &app.editor;
            // Two hops: the open job posts from the IO pool after its work; give it time by re-posting until loaded.
            std::function<void(int)> apply;
            auto attempt = std::make_shared<std::function<void(int)>>();
            *attempt = [ed, seek, sel, open_export, preset, attempt](int left) {
                EditorState& s = ed->State();
                const bool loaded = !s.project.clips.empty();
                if (!loaded && left > 0) {
                    Async::Run([attempt, left] {
                        Sleep(20);
                        Async::PostToUi([attempt, left] { (*attempt)(left - 1); });
                    });
                    return;
                }
                if (seek >= 0) s.playback->Seek(static_cast<TimeUs>(seek * 1e6));
                if (!sel.empty() && s.project.FindClip(sel)) s.selection.Set(sel);
                if (open_export) ed->OpenExportDialog(preset);
            };
            Async::PostToUi([attempt] { (*attempt)(200); });
        }

        app.win.Show(show_cmd);
        exit_code = Loop(app);

        Async::SetUiPoster(nullptr, nullptr);
        app.editor.Destroy();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        Fonts::Unload();
        ImGui::DestroyContext();
    } else {
        CT_LOG_ERROR("editor window or swapchain creation failed");
    }
    app.sc.Shutdown();
    app.win.Destroy();
    if (app.devices_ok) app.devices.Shutdown();
    app.dev.Shutdown();
    app.dev.ReportLiveObjects();
    g_app = nullptr;
    delete app_ptr;
    return exit_code;
}

} // namespace ct::ui
