// platform::HooksThread (P05 5.3): 120 Hz timer, LL mouse hook with swallowed tagged SendInput, stall watchdog.
#include <ct/core/time.h>
#include <ct/platform/hooks_thread.h>
#include <ct/test/ct_test.h>

#include <atomic>
#include <cstdio>

using namespace ct;
using namespace ct::platform;

namespace {

constexpr ULONG_PTR kTag = 0xC7E57A7;

struct CountingHandler final : HooksHandler {
    std::atomic<int> downs{0}, timers{0};
    void OnMouseHook(UINT msg, const MSLLHOOKSTRUCT& info) override {
        if (msg == WM_LBUTTONDOWN && info.dwExtraInfo == kTag) downs.fetch_add(1);
    }
    void OnKeyboardHook(UINT, const KBDLLHOOKSTRUCT&) override {}
    void OnForeground(HWND) override {}
    void OnTimer() override { timers.fetch_add(1); }
};

void TaggedClick() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    in[0].mi.dwExtraInfo = in[1].mi.dwExtraInfo = kTag;
    SendInput(2, in, sizeof(INPUT));
}

bool WaitFor(const std::atomic<int>& v, int at_least, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 10) {
        if (v.load() >= at_least) return true;
        Sleep(10);
    }
    return v.load() >= at_least;
}

} // namespace

CT_TEST_DEVICE(hooks_thread_timer_click_and_watchdog) {
    CountingHandler h;
    HooksThread hooks;
    HooksThread::Config cfg;
    cfg.keyboard = false;
    cfg.focus = false;
    cfg.timer_hz = 120;
    cfg.swallow_tag = kTag;
    CT_REQUIRE(hooks.Start(cfg, &h));
    const int64_t t0 = Clock::QpcNow();
    CT_CHECK(WaitFor(h.timers, 60, 3000));   // ~0.5 s of 120 Hz ticks
    const double secs = static_cast<double>(Clock::QpcToUs(Clock::QpcNow(), t0)) / 1e6;
    std::printf("  60 timer ticks in %.3f s (expected ~0.5 s)\n", secs);
    if (secs > 0.75) std::printf("  WARN: timer slower than 120 Hz (machine busy?)\n");

    TaggedClick();
    CT_CHECK(WaitFor(h.downs, 1, 2000));
    CT_CHECK(!hooks.CheckWatchdog());   // healthy loop

    // Stall the loop for 1.5 s: the watchdog must notice and the hooks must work again afterwards.
    hooks.DebugStall(1500);
    bool detected = false;
    for (int t = 0; t < 3000 && !detected; t += 100) {
        Sleep(100);
        detected = hooks.CheckWatchdog();
    }
    CT_CHECK(detected);
    for (int t = 0; t < 3000 && hooks.Reinstalls() == 0; t += 20) Sleep(20);
    CT_CHECK_EQ(hooks.Reinstalls(), 1u);
    const int before = h.downs.load();
    TaggedClick();
    CT_CHECK(WaitFor(h.downs, before + 1, 2000));
    hooks.Stop();
    CT_CHECK(!hooks.IsRunning());
}
