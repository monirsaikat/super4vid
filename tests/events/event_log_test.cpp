// ct/events/event_log.h: typed arrays, sorting, cursor interpolation, strokes, typing bursts, time-window queries.
#include <ct/core/fs.h>
#include <ct/events/event_log.h>
#include <ct/test/ct_test.h>

#include <string>
#include <vector>

using namespace ct;
using namespace ct::events;

namespace {

EventHeader Header1080() {
    EventHeader h;
    h.qpc_frequency = 10'000'000;
    h.region_w = 1920;
    h.region_h = 1080;
    h.flags = kFlagKeystrokes;
    return h;
}

KeyPayload MakeKey(uint16_t vk, bool down, uint8_t mods, const char* label) {
    KeyPayload k;
    k.vk = vk;
    k.down = down ? 1 : 0;
    k.modifiers = mods;
    CopyLabel(k.label, sizeof(k.label), label);
    return k;
}

EventLog FromWriter(const EventWriter& w) {
    EventLog log;
    Err err;
    const bool ok = log.LoadFromMemory(w.Bytes(), &err);
    CT_CHECK(ok);
    return log;
}

} // namespace

CT_TEST(event_log_cursor_interpolation) {
    EventWriter w;
    w.OpenMemory(Header1080());
    w.CursorMove(1'000'000, 100, 200, 7, true);
    w.CursorMove(2'000'000, 300, 600, 8, true);
    w.CursorMove(3'000'000, 300, 600, 8, false);
    EventLog log = FromWriter(w);
    CT_CHECK(!log.CursorAt(999'999).valid);
    CursorState s = log.CursorAt(1'500'000);
    CT_CHECK(s.valid);
    CT_CHECK_NEAR(s.x, 200.0, 1e-3);
    CT_CHECK_NEAR(s.y, 400.0, 1e-3);
    CT_CHECK_EQ(s.shape_id, uint32_t(7));   // shape of the earlier sample
    CT_CHECK(s.visible);
    s = log.CursorAt(2'000'000);
    CT_CHECK_NEAR(s.x, 300.0, 1e-3);
    CT_CHECK_EQ(s.shape_id, uint32_t(8));
    s = log.CursorAt(10'000'000);   // after the last sample: hold it
    CT_CHECK(!s.visible);
    CT_CHECK_NEAR(s.y, 600.0, 1e-3);
    CT_CHECK_EQ(log.LastCursorChangeAt(2'500'000), TimeUs(2'000'000));
    CT_CHECK_EQ(log.LastCursorChangeAt(10), TimeUs(-1));
    CT_CHECK_EQ(log.CursorSamplesBetween(1'000'000, 3'000'000).size(), size_t(2));
    CT_CHECK_EQ(log.CursorSamplesBetween(0, 10'000'000).size(), size_t(3));
    CT_CHECK_EQ(log.CursorSamplesBetween(5, 5).size(), size_t(0));
    CT_CHECK_EQ(log.LastTimeUs(), TimeUs(3'000'000));
    CT_CHECK_EQ(log.RegionSize().w, 1920);
}

CT_TEST(event_log_sorts_out_of_order_records) {
    EventWriter w;
    w.OpenMemory(Header1080());
    w.CursorMove(3000, 3, 3, 0, true);
    w.CursorMove(1000, 1, 1, 0, true);
    w.CursorMove(2000, 2, 2, 0, true);
    EventLog log = FromWriter(w);
    CT_REQUIRE(log.Cursor().size() == 3);
    CT_CHECK_EQ(log.Cursor()[0].t_us, TimeUs(1000));
    CT_CHECK_EQ(log.Cursor()[2].x, 3);
}

CT_TEST(event_log_mouse_queries) {
    EventWriter w;
    w.OpenMemory(Header1080());
    w.MouseButton(1000, Button::Left, true, 10, 10);
    w.MouseButton(1100, Button::Left, false, 10, 10);
    w.MouseButton(5000, Button::Right, true, 50, 50);
    w.MouseButton(5200, Button::Right, false, 50, 50);
    w.MouseWheel(6000, 120, 0, 1, 1);
    EventLog log = FromWriter(w);
    CT_CHECK_EQ(log.ButtonsBetween(0, 5000).size(), size_t(2));
    CT_CHECK_EQ(log.ButtonsBetween(0, 5001).size(), size_t(3));
    const ButtonEvent* d = log.LastMouseDown(5300, 1000);
    CT_REQUIRE(d != nullptr);
    CT_CHECK(d->button == Button::Right);
    CT_CHECK(log.LastMouseDown(5300, 100) == nullptr);   // too old
    CT_CHECK(log.LastMouseDown(500, 10'000) == nullptr);  // none yet
    std::vector<ButtonEvent> downs;
    log.MouseDownsBetween(0, 10'000, downs);
    CT_CHECK_EQ(downs.size(), size_t(2));
    downs.clear();
    const Button left = Button::Left;
    log.MouseDownsBetween(0, 10'000, downs, &left);
    CT_CHECK_EQ(downs.size(), size_t(1));
    CT_CHECK_EQ(log.WheelsBetween(0, 10'000).size(), size_t(1));
    CT_CHECK_EQ(log.WheelsBetween(0, 10'000)[0].delta_v, int16_t(120));
}

CT_TEST(event_log_strokes_and_key_state) {
    EventWriter w;
    w.OpenMemory(Header1080());
    w.Key(1000, MakeKey(0x11, true, kModCtrl, "Ctrl"));
    w.Key(1100, MakeKey(0x10, true, kModCtrl | kModShift, "Shift"));
    w.Key(1200, MakeKey(0x53, true, kModCtrl | kModShift, "S"));
    w.Key(1250, MakeKey(0x53, true, kModCtrl | kModShift, "S"));   // auto-repeat: not a new stroke
    w.Key(1300, MakeKey(0x53, false, kModCtrl | kModShift, "S"));
    w.Key(1400, MakeKey(0x10, false, kModCtrl, "Shift"));
    w.Key(1500, MakeKey(0x11, false, 0, "Ctrl"));
    w.Key(3'000'000, MakeKey(0x0D, true, 0, "Enter"));
    w.Key(3'000'100, MakeKey(0x0D, false, 0, "Enter"));
    KeyPayload ch = MakeKey(0x41, true, 0, "");
    ch.utf32_char = 0xE9;   // no label, a character
    w.Key(3'500'000, ch);
    EventLog log = FromWriter(w);
    CT_CHECK_EQ(log.Keys().size(), size_t(10));
    CT_REQUIRE(log.Strokes().size() == 3);
    CT_CHECK_EQ(log.Strokes()[0].text, std::string("Ctrl+Shift+S"));
    CT_CHECK_EQ(log.Strokes()[1].text, std::string("Enter"));
    CT_CHECK_EQ(log.Strokes()[2].text, std::string("\xC3\xA9"));
    CT_CHECK(log.IsKeyDown(0x11, 1250));
    CT_CHECK(!log.IsKeyDown(0x11, 1500));
    CT_CHECK(!log.IsKeyDown(0x99, 1500));
    CT_CHECK_EQ(log.StrokesVisibleAt(1200 + 1'199'999, 1'200'000).size(), size_t(1));
    CT_CHECK_EQ(log.StrokesVisibleAt(1200 + 1'200'000, 1'200'000).size(), size_t(0));
    CT_CHECK_EQ(log.StrokesVisibleAt(1199, 1'200'000).size(), size_t(0));
    CT_CHECK_EQ(log.StrokesBetween(0, 4'000'000).size(), size_t(3));
    CT_CHECK_EQ(log.KeysBetween(1000, 1300).size(), size_t(4));
    CT_CHECK_EQ(EventLog::StrokeText(kModWin | kModAlt, "Tab"), std::string("Alt+Win+Tab"));
}

CT_TEST(event_log_typing_bursts_derived_from_keys) {
    EventWriter w;
    w.OpenMemory(Header1080());
    for (int i = 0; i < 5; ++i) {
        w.Key(1'000'000 + i * 200'000, MakeKey(0x41, true, 0, "A"));
        w.Key(1'000'000 + i * 200'000 + 50'000, MakeKey(0x41, false, 0, "A"));
    }
    w.Key(4'000'000, MakeKey(0x42, true, 0, "B"));   // 2.2 s later: new burst
    EventLog log = FromWriter(w);
    CT_REQUIRE(log.TypingBursts().size() == 2);
    CT_CHECK_EQ(log.TypingBursts()[0].start_us, TimeUs(1'000'000));
    CT_CHECK_EQ(log.TypingBursts()[0].end_us, TimeUs(1'800'000));
    CT_CHECK(log.IsTyping(1'500'000));
    CT_CHECK(!log.IsTyping(2'000'000));
    CT_CHECK(log.IsTyping(4'000'000));
}

CT_TEST(event_log_typing_bursts_from_records) {
    EventWriter w;
    w.OpenMemory(Header1080());
    w.Typing(1000, true);
    w.Key(1000, MakeKey(0x41, true, 0, "A"));
    w.Typing(5000, false);
    w.Typing(9000, true);   // still open at end of file
    w.Key(9500, MakeKey(0x41, true, 0, "A"));
    EventLog log = FromWriter(w);
    CT_REQUIRE(log.TypingBursts().size() == 2);
    CT_CHECK_EQ(log.TypingBursts()[0].end_us, TimeUs(5000));
    CT_CHECK_EQ(log.TypingBursts()[1].start_us, TimeUs(9000));
    CT_CHECK_EQ(log.TypingBursts()[1].end_us, TimeUs(9500));
}

CT_TEST(event_log_shapes_focus_markers_pauses) {
    EventWriter w;
    w.OpenMemory(Header1080());
    std::vector<uint8_t> px(16 * 16 * 4, 0);
    for (size_t i = 0; i < px.size(); i += 4) px[i + 3] = (i / 4) % 3 == 0 ? 255 : 0;
    const uint32_t id = w.CursorShape(0, 0, 16, 16, 2, 3, px.data());
    w.CursorMove(10, 1, 1, id, true);
    WindowFocusPayload f;
    f.w = 800;
    f.h = 600;
    CopyLabel(f.title, sizeof(f.title), "Editor");
    w.WindowFocus(100, f);
    w.Marker(200, 0, "here");
    w.Pause(300, true);
    w.Pause(400, false);
    const uint8_t junk[8] = {};
    w.WriteRecord(250, 450, junk, sizeof(junk));
    EventLog log = FromWriter(w);
    const CursorShapeData* s = log.FindShape(id);
    CT_REQUIRE(s != nullptr);
    CT_CHECK_EQ(s->w, uint16_t(16));
    CT_CHECK_EQ(s->hot_y, int16_t(3));
    CT_CHECK(s->bgra == px);
    CT_CHECK(log.FindShape(id + 1) == nullptr);
    CT_CHECK(log.FocusAt(50) == nullptr);
    CT_REQUIRE(log.FocusAt(150) != nullptr);
    CT_CHECK_EQ(log.FocusAt(150)->title, std::string("Editor"));
    CT_CHECK_EQ(log.FocusAt(150)->rect.w, 800);
    CT_REQUIRE(log.Markers().size() == 1);
    CT_CHECK_EQ(log.Markers()[0].label, std::string("here"));
    CT_CHECK_EQ(log.Pauses().size(), size_t(2));
    CT_CHECK_EQ(log.UnknownRecords(), uint64_t(1));
}

CT_TEST(event_log_load_file_and_errors) {
    const std::string path = fs::TempPath(".ctev");
    {
        EventWriter w;
        Err err;
        CT_REQUIRE(w.Open(path, Header1080(), &err));
        for (int i = 0; i < 1200; ++i) w.CursorMove(i * 8333, i, 2 * i, 1, true);   // 10 s at 120 Hz
        CT_CHECK(w.Close());
    }
    EventLog log;
    Err err;
    CT_REQUIRE(log.Load(path, &err));
    CT_CHECK_EQ(log.Cursor().size(), size_t(1200));
    CT_CHECK(!log.Truncated());
    const CursorState s = log.CursorAt(8333 * 600 + 4166);
    CT_CHECK_NEAR(s.x, 600.5, 0.01);
    fs::Delete(path);
    CT_CHECK(!log.Load(path, &err));
    CT_CHECK(!err.Ok());
    CT_CHECK(log.Cursor().empty());
}

CT_BENCH(event_log_cursor_at, 1'000'000) {
    EventWriter w;
    w.OpenMemory(Header1080());
    for (int i = 0; i < 120 * 60 * 10; ++i) w.CursorMove(i * 8333, i & 1023, i & 511, 1, true);   // 10 min
    EventLog log;
    log.LoadFromMemory(w.Bytes(), nullptr);
    float acc = 0;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) acc += log.CursorAt((i * 7919) % 600'000'000).x;
    if (acc < 0) CT_FAIL("unreachable");
}
