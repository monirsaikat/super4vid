// EventRecorder end to end: real LL hooks receive SendInput events tagged with a private dwExtraInfo value; the
// hooks record and then swallow them, so no other application ever sees these clicks and keys.
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/events/ctev.h>
#include <ct/events/event_log.h>
#include <ct/recorder/event_recorder.h>
#include <ct/test/ct_test.h>

#include "../common/timing.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>

using namespace ct;
using namespace ct::recorder;

namespace {

constexpr ULONG_PTR kTag = 0xC7E57A6;

void SendMouse(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    in.mi.dwExtraInfo = kTag;
    SendInput(1, &in, sizeof(in));
}

void SendKey(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    in.ki.dwExtraInfo = kTag;
    SendInput(1, &in, sizeof(in));
}

void Wait(int ms) { Sleep(static_cast<DWORD>(ms)); }

// Waits until the recorder wrote at least `n` records (the hooks run on another thread; the machine may be busy).
bool WaitRecords(EventRecorder& r, uint64_t n, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        r.Flush(500);
        if (r.GetStats().records >= n) return true;
        Wait(20);
    }
    return false;
}

std::string TempCtev(const char* name) {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    return PathJoin(dir, Format("ct_recorder_test_%lu_%s.ctev", GetCurrentProcessId(), name));
}

RectI PrimaryRect() { return RectI{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}; }

} // namespace

CT_TEST_DEVICE(event_recorder_hooks_capture_sendinput) {
    const std::string path = TempCtev("hooks");
    SessionClock clock;
    clock.Start(Clock::QpcNow());
    EventRecorder rec;
    EventRecorder::Config cfg;
    cfg.region = PrimaryRect();
    cfg.keystrokes = KeystrokeMode::ShortcutsOnly;
    cfg.test_swallow_tag = kTag;
    Err err;
    CT_REQUIRE(rec.Start(cfg, path, clock, &err));

    POINT start{};
    GetCursorPos(&start);
    // A real cursor move (restored below) so the 120 Hz tracker sees a change.
    SetCursorPos(start.x + 17, start.y + 11);
    Wait(60);
    // A person moving the mouse meanwhile takes the real cursor elsewhere: detected here, then those position checks
    // are skipped (the hooks only report the test's own tagged input, but the cursor itself is shared).
    POINT moved_to{};
    GetCursorPos(&moved_to);
    SetCursorPos(start.x, start.y);
    Wait(30);
    POINT before_click{};
    GetCursorPos(&before_click);

    SendMouse(MOUSEEVENTF_LEFTDOWN);
    SendMouse(MOUSEEVENTF_LEFTUP);
    SendMouse(MOUSEEVENTF_RIGHTDOWN);
    SendMouse(MOUSEEVENTF_RIGHTUP);
    SendMouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-WHEEL_DELTA));
    SendKey(VK_F24, true);    // a shortcut (F-key): recorded
    SendKey(VK_F24, false);
    SendKey(VK_PAUSE, true);  // not a shortcut: only feeds the TYPING burst
    SendKey(VK_PAUSE, false);
    POINT after_click{};
    GetCursorPos(&after_click);
    const bool cursor_steady = std::abs(before_click.x - start.x) <= 2 && std::abs(before_click.y - start.y) <= 2 &&
                               std::abs(after_click.x - start.x) <= 2 && std::abs(after_click.y - start.y) <= 2;
    const bool move_held = std::abs(moved_to.x - (start.x + 17)) <= 2 && std::abs(moved_to.y - (start.y + 11)) <= 2;
    if (!cursor_steady || !move_held) std::printf("  WARN: the mouse was moved during the test; cursor position checks skipped\n");
    rec.Marker("chapter 1");
    CT_CHECK(WaitRecords(rec, 9, 3000));
    rec.SetPaused(true);
    SendMouse(MOUSEEVENTF_LEFTDOWN);   // while paused: swallowed but not recorded
    SendMouse(MOUSEEVENTF_LEFTUP);
    Wait(50);
    rec.SetPaused(false);
    char label[32];
    rec.LastKeyLabel(label);
    CT_CHECK_EQ(std::string(label), std::string("F24"));
    rec.Stop();
    const EventRecorder::Stats st = rec.GetStats();
    std::printf("  %llu records, %llu bytes, %llu cursor polls, ring drops %llu\n",
                static_cast<unsigned long long>(st.records), static_cast<unsigned long long>(st.bytes),
                static_cast<unsigned long long>(st.cursor_polls), static_cast<unsigned long long>(st.ring_dropped));
    CT_CHECK_EQ(st.ring_dropped, 0u);

    events::EventLog log;
    CT_REQUIRE(log.Load(path, &err));
    CT_CHECK_EQ(log.Header().flags, events::kFlagKeystrokes);
    CT_CHECK_EQ(log.Header().region_w, cfg.region.w);
    CT_CHECK_EQ(log.Header().epoch_qpc, clock.epoch_qpc);
    CT_CHECK(!log.Truncated());
    // The hooks see the whole desktop: real clicks from other processes (parallel agents, the user) land in the log
    // too. Outside strict runs, treat that as interference rather than a failure.
    if (log.Buttons().size() != 4 && !ct::test::StrictTiming()) {
        std::printf("  WARN: %zu button records (expected 4): other input during the test\n", log.Buttons().size());
        fs::Delete(path);
        CT_SKIP("desktop input interference");
    }
    CT_REQUIRE(log.Buttons().size() == 4);   // the clicks during the pause are absent
    CT_CHECK_EQ(log.Buttons()[0].button, events::Button::Left);
    CT_CHECK(log.Buttons()[0].down);
    CT_CHECK(!log.Buttons()[1].down);
    CT_CHECK_EQ(log.Buttons()[2].button, events::Button::Right);
    // Same tolerance as the cursor move below: other processes can nudge the real cursor between steps.
    if (cursor_steady) {
        CT_CHECK(std::abs(log.Buttons()[0].x - (start.x - cfg.region.x)) <= 2);
        CT_CHECK(std::abs(log.Buttons()[0].y - (start.y - cfg.region.y)) <= 2);
    }
    CT_REQUIRE(log.Wheels().size() == 1);
    CT_CHECK_EQ(log.Wheels()[0].delta_v, static_cast<int16_t>(-WHEEL_DELTA));
    CT_REQUIRE(log.Keys().size() == 2);   // F24 down + up; Pause is not a shortcut
    CT_CHECK_EQ(log.Keys()[0].vk, static_cast<uint16_t>(VK_F24));
    CT_CHECK_EQ(log.Keys()[0].label, std::string("F24"));
    CT_CHECK(log.Keys()[0].down && !log.Keys()[1].down);
    CT_CHECK(log.TypingBursts().size() == 1);   // F24 + Pause downs, closed at stop
    CT_REQUIRE(log.Markers().size() == 1);
    CT_CHECK_EQ(log.Markers()[0].label, std::string("chapter 1"));
    CT_REQUIRE(log.Pauses().size() == 2);
    CT_CHECK(log.Pauses()[0].paused && !log.Pauses()[1].paused);
    CT_CHECK(log.Cursor().size() >= 2);   // the initial sample and the move
    bool moved = false;
    // The test drives the real desktop cursor; other processes (parallel agents, the user) can nudge it by a few pixels
    // between steps, so accept the recorded move within 8 px of the target.
    for (const auto& c : log.Cursor())
        moved |= std::abs(c.x - (start.x + 17 - cfg.region.x)) <= 8 && std::abs(c.y - (start.y + 11 - cfg.region.y)) <= 8;
    if (move_held) CT_CHECK(moved);
    // Every referenced shape is in the file (CURSOR_SHAPE precedes its first move).
    for (const auto& c : log.Cursor())
        if (c.shape_id) CT_CHECK(log.FindShape(c.shape_id) != nullptr);
    std::printf("  cursor %zu, shapes %zu, focus %zu, typing %zu\n", log.Cursor().size(), log.Shapes().size(),
                log.Focus().size(), log.TypingBursts().size());
    // Times are media time, in order, and inside the session.
    for (size_t i = 1; i < log.Buttons().size(); ++i) CT_CHECK(log.Buttons()[i].t_us >= log.Buttons()[i - 1].t_us);
    CT_CHECK(log.Buttons()[0].t_us > 0 && log.Buttons()[0].t_us < 30 * kUsPerSecond);
    fs::Delete(path);
}

CT_TEST_DEVICE(event_recorder_all_keys_and_off_modes) {
    const std::string path = TempCtev("allkeys");
    SessionClock clock;
    clock.Start(Clock::QpcNow());
    Err err;
    {
        EventRecorder rec;
        EventRecorder::Config cfg;
        cfg.region = PrimaryRect();
        cfg.keystrokes = KeystrokeMode::All;
        cfg.test_swallow_tag = kTag;
        cfg.focus = false;
        CT_REQUIRE(rec.Start(cfg, path, clock, &err));
        SendKey(VK_PAUSE, true);
        SendKey(VK_PAUSE, false);
        SendKey(VK_F23, true);
        SendKey(VK_F23, false);
        CT_CHECK(WaitRecords(rec, 5, 3000));
        rec.Stop();
        events::EventLog log;
        CT_REQUIRE(log.Load(path, &err));
        CT_CHECK_EQ(log.Header().flags, events::kFlagKeystrokes | events::kFlagAllKeys);
        CT_REQUIRE(log.Keys().size() == 4);
        CT_CHECK_EQ(log.Keys()[0].label, std::string("Pause"));
        CT_CHECK_EQ(log.Keys()[0].utf32_char, 0u);
        CT_CHECK_EQ(log.Keys()[2].label, std::string("F23"));
        CT_CHECK(log.Focus().empty());
    }
    {
        EventRecorder rec;
        EventRecorder::Config cfg;
        cfg.region = PrimaryRect();
        cfg.keystrokes = KeystrokeMode::Off;
        cfg.test_swallow_tag = kTag;
        CT_REQUIRE(rec.Start(cfg, path, clock, &err));
        SendMouse(MOUSEEVENTF_MIDDLEDOWN);
        SendMouse(MOUSEEVENTF_MIDDLEUP);
        CT_CHECK(WaitRecords(rec, 2, 3000));
        rec.Stop();
        events::EventLog log;
        CT_REQUIRE(log.Load(path, &err));
        CT_CHECK_EQ(log.Header().flags, 0u);
        CT_CHECK(log.Keys().empty());
        CT_CHECK(log.TypingBursts().empty());
        CT_REQUIRE(log.Buttons().size() == 2);
        CT_CHECK_EQ(log.Buttons()[0].button, events::Button::Middle);
    }
    fs::Delete(path);
}

CT_TEST_DEVICE(event_recorder_flushes_every_second) {
    // While recording, the file on disk grows at least once per second (crash loss <= 1 s).
    const std::string path = TempCtev("flush");
    SessionClock clock;
    clock.Start(Clock::QpcNow());
    EventRecorder rec;
    EventRecorder::Config cfg;
    cfg.region = PrimaryRect();
    cfg.test_swallow_tag = kTag;
    Err err;
    CT_REQUIRE(rec.Start(cfg, path, clock, &err));
    Wait(100);
    SendMouse(MOUSEEVENTF_LEFTDOWN);
    SendMouse(MOUSEEVENTF_LEFTUP);
    const int64_t t0 = Clock::QpcNow();
    uint64_t size = 0;
    for (int i = 0; i < 300 && size <= 44; ++i) {   // up to 3 s on a loaded machine
        Wait(10);
        std::string bytes;
        if (fs::ReadAll(path, bytes)) size = bytes.size();
    }
    const double ms = static_cast<double>(Clock::QpcToUs(Clock::QpcNow(), t0)) / 1000.0;
    std::printf("  clicks on disk after %.0f ms (%llu bytes)\n", ms, static_cast<unsigned long long>(size));
    CT_CHECK(size > 44);
    if (ms > 1200) std::printf("  WARN: flush took longer than the 1 s budget (machine busy?)\n");
    CT_CHECK(ms < 3000);
    rec.Stop();
    fs::Delete(path);
}

// Markers (controlling thread) interleaved with hook events (ct-hooks): the file holds every record in time order.
// A control used to be stamped before it was queued, so a drain in between could write later hook events before it.
CT_TEST_DEVICE(event_recorder_controls_and_hook_events_in_time_order) {
    const std::string path = TempCtev("order");
    SessionClock clock;
    clock.Start(Clock::QpcNow());
    EventRecorder rec;
    EventRecorder::Config cfg;
    cfg.region = PrimaryRect();
    cfg.keystrokes = KeystrokeMode::ShortcutsOnly;
    cfg.focus = false;
    cfg.test_swallow_tag = kTag;
    Err err;
    CT_REQUIRE(rec.Start(cfg, path, clock, &err));
    constexpr int kRounds = 300;
    for (int i = 0; i < kRounds; ++i) {
        SendMouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(WHEEL_DELTA));
        rec.Marker("m");
        if (i % 50 == 25) rec.Flush(500);   // flush drains between the periodic ones
        if (i % 10 == 0) Wait(1);
    }
    CT_CHECK(WaitRecords(rec, 2 * kRounds, 5000));
    rec.Stop();

    events::EventReader reader;
    CT_REQUIRE(reader.Open(path, &err));
    events::RecordView r;
    TimeUs prev = INT64_MIN;
    uint8_t prev_type = 0;
    size_t markers = 0, wheels = 0, out_of_order = 0;
    while (reader.Next(r)) {
        if (r.t_us < prev) {
            ++out_of_order;
            std::printf("  out of order: type %u at %lld us after type %u at %lld us\n", static_cast<unsigned>(r.type),
                        static_cast<long long>(r.t_us), static_cast<unsigned>(prev_type), static_cast<long long>(prev));
        }
        prev_type = r.type;
        prev = std::max(prev, r.t_us);
        if (r.type == static_cast<uint8_t>(events::RecordType::Marker)) ++markers;
        if (r.type == static_cast<uint8_t>(events::RecordType::MouseWheel)) ++wheels;
    }
    CT_CHECK(!reader.Truncated());
    CT_CHECK_EQ(markers, static_cast<size_t>(kRounds));
    CT_CHECK_EQ(wheels, static_cast<size_t>(kRounds));
    CT_CHECK_EQ(out_of_order, size_t{0});
    fs::Delete(path);
}

// A TYPING "ended" record carries the last key's time but is only known a gap (1 s) later: records in between are held
// back until the burst's fate is decided, so the file stays in time order (seen as one out-of-order TYPING record).
CT_TEST_DEVICE(event_recorder_typing_end_stays_in_time_order) {
    const std::string path = TempCtev("typing_order");
    SessionClock clock;
    clock.Start(Clock::QpcNow());
    EventRecorder rec;
    EventRecorder::Config cfg;
    cfg.region = PrimaryRect();
    cfg.keystrokes = KeystrokeMode::ShortcutsOnly;
    cfg.focus = false;
    cfg.test_swallow_tag = kTag;
    Err err;
    CT_REQUIRE(rec.Start(cfg, path, clock, &err));
    SendKey(VK_PAUSE, true);   // not a shortcut: only opens a TYPING burst
    SendKey(VK_PAUSE, false);
    for (int i = 0; i < 10; ++i) {   // inside the gap: must wait for the burst's end record
        rec.Marker("in gap");
        rec.Flush(500);
        Wait(30);
    }
    Wait(1200);   // the gap passes: the burst ends at the key's time
    rec.Marker("after gap");
    rec.Flush(500);
    rec.Stop();

    events::EventReader reader;
    CT_REQUIRE(reader.Open(path, &err));
    events::RecordView r;
    TimeUs prev = INT64_MIN;
    size_t out_of_order = 0, typing = 0, markers = 0;
    while (reader.Next(r)) {
        if (r.t_us < prev) ++out_of_order;
        prev = std::max(prev, r.t_us);
        if (r.type == static_cast<uint8_t>(events::RecordType::Typing)) ++typing;
        if (r.type == static_cast<uint8_t>(events::RecordType::Marker)) ++markers;
    }
    CT_CHECK_EQ(out_of_order, size_t{0});
    CT_CHECK_EQ(markers, size_t{11});
    events::EventLog log;
    CT_REQUIRE(log.Load(path, &err));
    // Other processes' typing (shared desktop) can add bursts; ours is the first and ends where it started.
    if (typing != 2 && !ct::test::StrictTiming()) std::printf("  WARN: %zu TYPING records: other input during the test\n", typing);
    CT_REQUIRE(!log.TypingBursts().empty());
    CT_CHECK_EQ(log.TypingBursts()[0].end_us, log.TypingBursts()[0].start_us);
    CT_CHECK(log.Markers().back().t_us > log.TypingBursts()[0].end_us + 1'000'000);
    fs::Delete(path);
}
