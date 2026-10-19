// events_demo: exercises the P05 input/overlay half without a RecordingSession.
//
//   events_demo --seconds 10 --out x.ctev [--monitor N] [--keys off|shortcuts|all] [--hz 120] [--selftest]
//       records events for a monitor (primary by default) and prints a summary read back with EventLog.
//       --selftest synthesizes input while recording (cursor moves via SetCursorPos, restored; clicks, wheel and
//       keys via SendInput tagged so the hooks record and swallow them: no other app sees them) and checks the file.
//   events_demo --region [--window] [--auto [--snapshot DIR]] [--frame S]
//       shows the region selection overlay and prints the chosen rect. --auto drives it with posted messages
//       (drag, nudge, Enter) for a headless check and can save what it drew as PNGs; --frame shows the recording frame around the result for S s.
//   events_demo --countdown [N] [--monitor N]
//       shows the N-second countdown (default 3) centred on the monitor; Esc cancels.

#include <ct/capture/display.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/events/event_log.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>
#include <ct/recorder/countdown_overlay.h>
#include <ct/recorder/event_recorder.h>
#include <ct/recorder/recording_frame.h>
#include <ct/recorder/region_overlay.h>

#include <dxgi1_6.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::recorder;

namespace {

constexpr ULONG_PTR kTag = 0xC7E5DE30;

struct Args {
    int seconds = 10;
    std::string out;
    int monitor = 0;
    KeystrokeMode keys = KeystrokeMode::ShortcutsOnly;
    int hz = 120;
    bool selftest = false;
    bool region = false, window = false, autodrive = false;
    int frame_s = 0;
    std::string snapshot_dir;
    bool countdown = false;
    int countdown_s = 3;
};

bool Parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&](const char*& v) { if (i + 1 >= argc) return false; v = argv[++i]; return true; };
        const char* v = nullptr;
        if (s == "--seconds" && next(v)) a.seconds = std::atoi(v);
        else if (s == "--out" && next(v)) a.out = v;
        else if (s == "--monitor" && next(v)) a.monitor = std::atoi(v);
        else if (s == "--hz" && next(v)) a.hz = std::atoi(v);
        else if (s == "--keys" && next(v)) {
            const std::string k = v;
            if (k == "off") a.keys = KeystrokeMode::Off;
            else if (k == "all") a.keys = KeystrokeMode::All;
            else if (k == "shortcuts") a.keys = KeystrokeMode::ShortcutsOnly;
            else return false;
        } else if (s == "--selftest") a.selftest = true;
        else if (s == "--region") a.region = true;
        else if (s == "--window") a.window = true;
        else if (s == "--auto") a.autodrive = true;
        else if (s == "--frame" && next(v)) a.frame_s = std::atoi(v);
        else if (s == "--snapshot" && next(v)) a.snapshot_dir = v;
        else if (s == "--countdown") {
            a.countdown = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') a.countdown_s = std::atoi(argv[++i]);
        } else return false;
    }
    return true;
}

std::vector<capture::DisplayInfo> Displays() {
    Microsoft::WRL::ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(f.GetAddressOf())))) return {};
    return capture::EnumerateDisplays(f.Get());
}

void Pump(int ms) {
    const int64_t end = Clock::QpcNow() + Clock::QpcFrequency() * ms / 1000;
    MSG msg;
    while (Clock::QpcNow() < end) {
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void SendTagged(INPUT in) {
    if (in.type == INPUT_MOUSE) in.mi.dwExtraInfo = kTag;
    else in.ki.dwExtraInfo = kTag;
    SendInput(1, &in, sizeof(in));
}
void Mouse(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendTagged(in);
}
void Key(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendTagged(in);
}

int RunRecord(const Args& a) {
    const auto displays = Displays();
    if (a.monitor < 0 || a.monitor >= static_cast<int>(displays.size())) {
        std::fprintf(stderr, "monitor %d not found (%zu displays)\n", a.monitor, displays.size());
        return 1;
    }
    const capture::DisplayInfo& d = displays[static_cast<size_t>(a.monitor)];
    const std::string out = a.out.empty() ? PathJoin(platform::Paths::LogsDir(), "events_demo.ctev") : a.out;
    SessionClock clock;
    clock.Start(Clock::QpcNow());
    EventRecorder rec;
    EventRecorder::Config cfg;
    cfg.region = d.rect;
    cfg.keystrokes = a.keys;
    cfg.cursor_hz = a.hz;
    cfg.test_swallow_tag = a.selftest ? kTag : 0;
    Err err;
    if (!rec.Start(cfg, out, clock, &err)) {
        std::fprintf(stderr, "EventRecorder::Start failed: %s\n", err.message.c_str());
        return 1;
    }
    std::printf("recording events of monitor %d (%dx%d at %d,%d) for %d s into %s\n", a.monitor, d.rect.w, d.rect.h,
                d.rect.x, d.rect.y, a.seconds, out.c_str());
    POINT start{};
    GetCursorPos(&start);
    const int64_t end = Clock::QpcNow() + Clock::QpcFrequency() * a.seconds;
    int step = 0;
    while (Clock::QpcNow() < end) {
        Pump(100);
        if (!a.selftest) continue;
        ++step;
        if (step <= 10) SetCursorPos(start.x + step * 5, start.y + step * 3);   // 10 moves over 1 s
        if (step == 11) SetCursorPos(start.x, start.y);
        if (step == 12) { Mouse(MOUSEEVENTF_LEFTDOWN); Mouse(MOUSEEVENTF_LEFTUP); }
        if (step == 13) { Mouse(MOUSEEVENTF_RIGHTDOWN); Mouse(MOUSEEVENTF_RIGHTUP); }
        if (step == 14) Mouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-WHEEL_DELTA));
        if (step == 15) { Key(VK_F24, true); Key(VK_F24, false); }
        if (step == 16) { Key(VK_PAUSE, true); Key(VK_PAUSE, false); }
        if (step == 17) rec.Marker("selftest");
        if (step == 18) rec.SetPaused(true);
        if (step == 19) { Mouse(MOUSEEVENTF_LEFTDOWN); Mouse(MOUSEEVENTF_LEFTUP); }   // during the pause: not recorded
        if (step == 20) rec.SetPaused(false);
    }
    if (a.selftest) SetCursorPos(start.x, start.y);
    rec.Stop();
    const EventRecorder::Stats st = rec.GetStats();

    events::EventLog log;
    if (!log.Load(out, &err)) {
        std::fprintf(stderr, "EventLog::Load failed: %s\n", err.message.c_str());
        return 1;
    }
    std::printf("file: %llu bytes, %llu records written, ring drops %llu, cursor polls %llu, hook reinstalls %u%s\n",
                static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.records),
                static_cast<unsigned long long>(st.ring_dropped), static_cast<unsigned long long>(st.cursor_polls),
                st.hook_reinstalls, log.Truncated() ? " (TRUNCATED)" : "");
    std::printf("header: v%u, %u bytes, region %dx%d at (%d,%d), flags 0x%x, last t %.3f s\n", log.Header().version,
                log.Header().header_size, log.Header().region_w, log.Header().region_h, log.Header().region_x,
                log.Header().region_y, log.Header().flags, static_cast<double>(log.LastTimeUs()) / 1e6);
    std::printf("cursor moves %zu, shapes %zu, buttons %zu, wheels %zu, keys %zu (strokes %zu), focus %zu, markers %zu, "
                "pauses %zu, typing bursts %zu\n",
                log.Cursor().size(), log.Shapes().size(), log.Buttons().size(), log.Wheels().size(), log.Keys().size(),
                log.Strokes().size(), log.Focus().size(), log.Markers().size(), log.Pauses().size(),
                log.TypingBursts().size());
    for (const auto& s : log.Shapes()) std::printf("  shape %08x %ux%u hotspot (%d,%d)\n", s.id, s.w, s.h, s.hot_x, s.hot_y);
    for (const auto& f : log.Focus())
        std::printf("  focus %.3f s \"%s\" (%s) %dx%d\n", static_cast<double>(f.t_us) / 1e6, f.title.c_str(), f.exe.c_str(),
                    f.rect.w, f.rect.h);
    for (const auto& b : log.TypingBursts())
        std::printf("  typing %.3f - %.3f s\n", static_cast<double>(b.start_us) / 1e6, static_cast<double>(b.end_us) / 1e6);
    for (const auto& k : log.Strokes()) std::printf("  stroke %.3f s %s\n", static_cast<double>(k.t_us) / 1e6, k.text.c_str());
    if (log.Cursor().size() >= 2) {
        // Sample spacing: the tracker writes only changes, so report the smallest gap (should be ~1/hz).
        TimeUs min_gap = INT64_MAX;
        for (size_t i = 1; i < log.Cursor().size(); ++i) min_gap = std::min(min_gap, log.Cursor()[i].t_us - log.Cursor()[i - 1].t_us);
        std::printf("  smallest cursor sample gap %.2f ms\n", static_cast<double>(min_gap) / 1000.0);
    }
    if (!a.selftest) return 0;

    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++failures;
    };
    bool moved = false;
    for (const auto& c : log.Cursor()) moved |= c.x == start.x + 50 - d.rect.x && c.y == start.y + 30 - d.rect.y;
    expect(log.Cursor().size() >= 10, "cursor moves recorded (>= 10)");
    expect(moved, "the last synthetic position is in the file");
    expect(!log.Shapes().empty(), "at least one CURSOR_SHAPE");
    expect(log.Buttons().size() == 4, "4 button records (the clicks during the pause are absent)");
    expect(log.Wheels().size() == 1, "1 wheel record");
    const size_t want_keys = a.keys == KeystrokeMode::Off ? 0 : a.keys == KeystrokeMode::All ? 4 : 2;
    expect(log.Keys().size() == want_keys, "key records match the keystroke mode");
    // F24 and Pause are pressed 100 ms apart (one burst), but a busy machine can stretch the gap past 1 s.
    expect(a.keys == KeystrokeMode::Off ? log.TypingBursts().empty() : !log.TypingBursts().empty(),
           "typing bursts match the mode");
    expect(log.Markers().size() == 1, "1 marker");
    expect(log.Pauses().size() == 2, "pause + resume");
    expect(st.ring_dropped == 0, "no ring drops");
    expect(!log.Truncated(), "file complete");
    std::printf("selftest: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}

// --auto: drives the overlay with posted messages, as the user would with the mouse and keyboard.
struct AutoDrive {
    RectI virt;
    int step = 0;
    RegionOverlay* overlay = nullptr;
    std::string snapshot_dir;
    void Snap(const char* name) const {
        if (snapshot_dir.empty()) return;
        const std::string path = PathJoin(snapshot_dir, Format("region_%s.png", name));
        std::printf("snapshot %s: %s\n", name, overlay->SaveSnapshot(path.c_str()) ? path.c_str() : "FAILED");
    }
    static AutoDrive* self;
    static HWND Overlay() {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, L"super4vidLayered", nullptr)) != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(h)) return h;
        }
        return nullptr;
    }
    static void CALLBACK Tick(HWND, UINT, UINT_PTR id, DWORD) {
        HWND w = Overlay();
        if (!w) return;
        auto client = [&](int x, int y) { return MAKELPARAM(x - self->virt.x, y - self->virt.y); };
        const int x0 = self->virt.x + 101, y0 = self->virt.y + 101;   // near the virtual screen's top-left
        switch (self->step++) {
        case 0: PostMessageW(w, WM_MOUSEMOVE, 0, client(x0, y0)); break;
        case 1: self->Snap("hover"); PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, client(x0, y0)); break;
        case 2: PostMessageW(w, WM_MOUSEMOVE, MK_LBUTTON, client(x0 + 300, y0 + 150)); break;
        case 3: PostMessageW(w, WM_MOUSEMOVE, MK_LBUTTON, client(x0 + 640, y0 + 360)); break;   // 641x361 -> 640x360
        case 4: PostMessageW(w, WM_LBUTTONUP, 0, client(x0 + 640, y0 + 360)); break;
        case 5: self->Snap("adjust"); PostMessageW(w, WM_KEYDOWN, VK_RIGHT, 0); break;   // nudge +1 px
        case 6: PostMessageW(w, WM_KEYDOWN, VK_RETURN, 0); break;
        default: KillTimer(nullptr, id); break;
        }
    }
};
AutoDrive* AutoDrive::self = nullptr;

int RunRegion(const Args& a) {
    const auto displays = Displays();
    if (displays.empty()) {
        std::fprintf(stderr, "no displays\n");
        return 1;
    }
    AutoDrive drive;
    drive.virt = capture::VirtualScreenRect();
    if (a.autodrive) {
        AutoDrive::self = &drive;
        SetTimer(nullptr, 0, 150, &AutoDrive::Tick);
    }
    const auto mode = a.window ? RegionOverlay::Mode::Window : RegionOverlay::Mode::Region;
    const int64_t t0 = Clock::QpcNow();
    struct Ctx { bool done = false; RegionOverlay::Result result; } ctx;
    RegionOverlay overlay;
    drive.overlay = &overlay;
    drive.snapshot_dir = a.snapshot_dir;
    if (!overlay.Open(displays, mode, [](void* u, const RegionOverlay::Result& res) {
            auto* c = static_cast<Ctx*>(u);
            c->result = res;
            c->done = true;
        }, &ctx)) {
        std::fprintf(stderr, "RegionOverlay::Open failed\n");
        return 1;
    }
    MSG msg;
    while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const RegionOverlay::Result r = ctx.result;
    std::printf("overlay: slowest incremental update %.2f ms\n", overlay.MaxUpdateMs());
    const double ms = static_cast<double>(Clock::QpcToUs(Clock::QpcNow(), t0)) / 1000.0;
    if (!r.confirmed) {
        std::printf("region: cancelled (%.0f ms)\n", ms);
        return a.autodrive ? 1 : 0;
    }
    std::printf("region: %dx%d at (%d,%d) on display %d (%.0f ms)\n", r.region.w, r.region.h, r.region.x, r.region.y,
                r.display_index, ms);
    int rc = 0;
    if (a.autodrive) {
        const RectI want{drive.virt.x + 102, drive.virt.y + 101, 640, 360};
        const bool ok = r.region.x == want.x && r.region.y == want.y && r.region.w == want.w && r.region.h == want.h;
        std::printf("auto: expected %dx%d at (%d,%d): %s\n", want.w, want.h, want.x, want.y, ok ? "ok" : "FAIL");
        rc = ok ? 0 : 1;
    }
    if (a.frame_s > 0) {
        std::vector<RectI> rects;
        for (const auto& d : displays) rects.push_back(d.rect);
        RecordingFrame frame;
        const bool shown = frame.Show(r.region, rects, displays[0].dpi);
        std::printf("frame: %s, excluded from capture: %s\n", shown ? "shown" : "FAILED", frame.ExcludedFromCapture() ? "yes" : "no");
        Pump(a.frame_s * 1000);
    }
    return rc;
}

int RunCountdown(const Args& a) {
    const auto displays = Displays();
    if (a.monitor < 0 || a.monitor >= static_cast<int>(displays.size())) return 1;
    const capture::DisplayInfo& d = displays[static_cast<size_t>(a.monitor)];
    const int64_t t0 = Clock::QpcNow();
    const bool done = CountdownOverlay::RunModal(d.rect, d.dpi, a.countdown_s);
    const double ms = static_cast<double>(Clock::QpcToUs(Clock::QpcNow(), t0)) / 1000.0;
    std::printf("countdown %d s: %s after %.0f ms\n", a.countdown_s, done ? "completed" : "cancelled", ms);
    return done ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Args a;
    if (!Parse(argc, argv, a)) {
        std::fprintf(stderr, "usage: events_demo --seconds 10 --out x.ctev [--monitor N] [--keys off|shortcuts|all] "
                             "[--hz 120] [--selftest]\n       events_demo --region [--window] [--auto] [--frame S]\n"
                             "       events_demo --countdown [N] [--monitor N]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    platform::Paths::Init();
    Log::Init(platform::Paths::LogsDir().c_str());
    int rc = 0;
    if (a.region) rc = RunRegion(a);
    else if (a.countdown) rc = RunCountdown(a);
    else rc = RunRecord(a);
    Log::Shutdown();
    CoUninitialize();
    return rc;
}
