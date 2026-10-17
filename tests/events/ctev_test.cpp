// ct/events/ctev.h: binary layout, writer buffering/flush, reader tolerance (unknown types, torn tail), RLE.
#include <ct/core/fs.h>
#include <ct/events/ctev.h>
#include <ct/test/ct_test.h>

#include <cstring>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::events;

namespace {

EventHeader MakeHeader() {
    EventHeader h;
    h.epoch_qpc = 123456789012;
    h.qpc_frequency = 10'000'000;
    h.region_x = 100;
    h.region_y = 50;
    h.region_w = 1920;
    h.region_h = 1080;
    h.flags = kFlagKeystrokes;
    return h;
}

std::vector<uint8_t> Checker(int w, int h) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int i = 0; i < w * h; ++i) {
        const uint8_t v = static_cast<uint8_t>((i * 37) & 0xFF);
        px[size_t(i) * 4 + 0] = v;
        px[size_t(i) * 4 + 1] = static_cast<uint8_t>(255 - v);
        px[size_t(i) * 4 + 2] = static_cast<uint8_t>(i & 0xFF);
        px[size_t(i) * 4 + 3] = 255;
    }
    return px;
}

std::vector<uint8_t> Solid(int w, int h, uint32_t pixel) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int i = 0; i < w * h; ++i) std::memcpy(&px[size_t(i) * 4], &pixel, 4);
    return px;
}

} // namespace

CT_TEST(ctev_layout_sizes) {
    CT_CHECK_EQ(sizeof(EventHeader), size_t(44));
    CT_CHECK_EQ(sizeof(RecordHeader), size_t(16));
    EventWriter w;
    w.OpenMemory(MakeHeader());
    CT_CHECK_EQ(w.Bytes().size(), size_t(44));
    w.CursorMove(1, 2, 3, 4, true);
    w.MouseButton(2, Button::Left, true, 5, 6);
    w.MouseWheel(3, 120, 0, 7, 8);
    w.Key(4, KeyPayload{});
    w.Marker(5, 0, "intro");
    w.Pause(6, true);
    w.Typing(7, true);
    w.WindowFocus(8, WindowFocusPayload{});
    // 44 + 32 + 28 + 28 + 60 + 84 + 20 + 20 + 360
    CT_CHECK_EQ(w.Bytes().size(), size_t(44 + 32 + 28 + 28 + 60 + 84 + 20 + 20 + 360));
    CT_CHECK_EQ(w.RecordCount(), uint64_t(8));
    CT_CHECK(std::memcmp(w.Bytes().data(), "CTEV", 4) == 0);
    uint16_t version = 0, header_size = 0;
    std::memcpy(&version, w.Bytes().data() + 4, 2);
    std::memcpy(&header_size, w.Bytes().data() + 6, 2);
    CT_CHECK_EQ(version, uint16_t(1));
    CT_CHECK_EQ(header_size, uint16_t(44));
}

CT_TEST(ctev_records_padded_to_4) {
    EventWriter w;
    w.OpenMemory(MakeHeader());
    const char odd[5] = {1, 2, 3, 4, 5};
    w.WriteRecord(200, 10, odd, sizeof(odd));
    CT_CHECK_EQ(w.Bytes().size(), size_t(44 + 16 + 8));
    uint32_t size = 0;
    std::memcpy(&size, w.Bytes().data() + 44, 4);
    CT_CHECK_EQ(size, uint32_t(24));
    CT_CHECK_EQ(w.Bytes()[44 + 16 + 5], '\0');
}

CT_TEST(ctev_file_roundtrip_all_types) {
    const std::string path = fs::TempPath(".ctev");
    {
        EventWriter w;
        Err err;
        CT_REQUIRE(w.Open(path, MakeHeader(), &err));
        w.CursorMove(1000, 10, 20, 0xABCD, true);
        w.MouseButton(2000, Button::Right, true, 30, 40);
        w.MouseWheel(3000, -120, 60, 50, 60);
        KeyPayload k;
        k.vk = 0x41;
        k.scancode = 30;
        k.down = 1;
        k.modifiers = kModCtrl | kModShift;
        k.utf32_char = 'A';
        CopyLabel(k.label, sizeof(k.label), "A");
        w.Key(4000, k);
        WindowFocusPayload f;
        f.hwnd = 0x123456;
        f.x = 1;
        f.y = 2;
        f.w = 300;
        f.h = 400;
        CopyLabel(f.title, sizeof(f.title), "Notepad - \xC3\xA9t\xC3\xA9");
        CopyLabel(f.exe, sizeof(f.exe), "notepad.exe");
        w.WindowFocus(5000, f);
        w.Marker(6000, 0, "chapter 1");
        w.Pause(7000, true);
        w.Typing(8000, false);
        const std::vector<uint8_t> px = Checker(4, 3);
        w.CursorShape(500, 0, 4, 3, 1, 2, px.data());
        CT_CHECK(w.Close());
    }
    EventReader r;
    Err err;
    CT_REQUIRE(r.Open(path, &err));
    CT_CHECK_EQ(r.Header().region_w, 1920);
    CT_CHECK_EQ(r.Header().epoch_qpc, int64_t(123456789012));
    CT_CHECK_EQ(r.Header().flags, kFlagKeystrokes);
    RecordView v;
    std::vector<uint8_t> types;
    std::vector<TimeUs> times;
    while (r.Next(v)) {
        types.push_back(v.type);
        times.push_back(v.t_us);
        if (v.type == uint8_t(RecordType::Key)) {
            KeyPayload kp;
            CT_REQUIRE(v.payload_size >= sizeof(kp));
            std::memcpy(&kp, v.payload, sizeof(kp));
            CT_CHECK_EQ(kp.vk, uint16_t(0x41));
            CT_CHECK_EQ(kp.modifiers, uint8_t(kModCtrl | kModShift));
            CT_CHECK_EQ(std::string(kp.label), std::string("A"));
        }
        if (v.type == uint8_t(RecordType::WindowFocus)) {
            WindowFocusPayload fp;
            std::memcpy(&fp, v.payload, sizeof(fp));
            CT_CHECK_EQ(std::string(fp.title), std::string("Notepad - \xC3\xA9t\xC3\xA9"));
            CT_CHECK_EQ(fp.hwnd, uint64_t(0x123456));
        }
    }
    CT_CHECK(!r.Truncated());
    CT_REQUIRE(types.size() == 9);
    CT_CHECK_EQ(types[0], uint8_t(RecordType::CursorMove));
    CT_CHECK_EQ(types[8], uint8_t(RecordType::CursorShape));
    CT_CHECK_EQ(times[3], TimeUs(4000));
    fs::Delete(path);
}

CT_TEST(ctev_flush_makes_data_durable_while_open) {
    const std::string path = fs::TempPath(".ctev");
    EventWriter w;
    Err err;
    CT_REQUIRE(w.Open(path, MakeHeader(), &err, 1 << 20));
    CT_CHECK_EQ(fs::FileSize(path), uint64_t(44));   // header is flushed at Open
    for (int i = 0; i < 100; ++i) w.CursorMove(i * 8333, i, i, 1, true);
    CT_CHECK_EQ(w.BufferedBytes(), size_t(100 * 32));
    CT_CHECK_EQ(fs::FileSize(path), uint64_t(44));   // still buffered
    CT_CHECK(w.Flush());
    CT_CHECK_EQ(w.BufferedBytes(), size_t(0));
    CT_CHECK_EQ(fs::FileSize(path), uint64_t(44 + 100 * 32));
    CT_CHECK_EQ(w.BytesWritten(), uint64_t(44 + 100 * 32));
    // Simulated crash: read the file while the writer still has it open.
    std::string bytes;
    CT_REQUIRE(fs::ReadAll(path, bytes));
    EventReader r;
    CT_REQUIRE(r.Parse(bytes, &err));
    RecordView v;
    int n = 0;
    while (r.Next(v)) ++n;
    CT_CHECK_EQ(n, 100);
    CT_CHECK(w.Close());
    CT_CHECK(w.Close());   // idempotent
    fs::Delete(path);
}

CT_TEST(ctev_auto_flush_at_threshold) {
    const std::string path = fs::TempPath(".ctev");
    EventWriter w;
    Err err;
    CT_REQUIRE(w.Open(path, MakeHeader(), &err, 4096));
    for (int i = 0; i < 200; ++i) w.CursorMove(i, i, i, 1, true);   // 6400 bytes > 4096
    CT_CHECK(fs::FileSize(path) > 44);
    CT_CHECK(w.BufferedBytes() < 4096);
    CT_CHECK(w.Close());
    CT_CHECK_EQ(fs::FileSize(path), uint64_t(44 + 200 * 32));
    fs::Delete(path);
}

CT_TEST(ctev_reader_skips_unknown_and_stops_at_torn_tail) {
    EventWriter w;
    w.OpenMemory(MakeHeader());
    w.CursorMove(1, 1, 1, 0, true);
    const uint8_t future[40] = {9, 9, 9};
    w.WriteRecord(77, 2, future, sizeof(future));
    w.CursorMove(3, 3, 3, 0, true);
    std::string bytes = w.Bytes();
    bytes.resize(bytes.size() - 7);   // tear the last record
    EventReader r;
    Err err;
    CT_REQUIRE(r.Parse(bytes, &err));
    RecordView v;
    std::vector<uint8_t> types;
    while (r.Next(v)) types.push_back(v.type);
    CT_REQUIRE(types.size() == 2);
    CT_CHECK_EQ(types[1], uint8_t(77));
    CT_CHECK(r.Truncated());
}

CT_TEST(ctev_reader_rejects_bad_magic_and_accepts_40_byte_header) {
    EventReader r;
    Err err;
    CT_CHECK(!r.Parse("NOPE0000000000000000000000000000000000000000", &err));
    CT_CHECK(!err.Ok());
    CT_CHECK(!r.Parse("CT", nullptr));
    // A 40-byte header (docs/04 text) without the flags field.
    EventHeader h = MakeHeader();
    h.header_size = 40;
    std::string bytes(reinterpret_cast<const char*>(&h), 40);
    RecordHeader rh;
    rh.size = 32;
    rh.type = 1;
    rh.t_us = 99;
    CursorMovePayload p;
    p.x = 5;
    bytes.append(reinterpret_cast<const char*>(&rh), sizeof(rh));
    bytes.append(reinterpret_cast<const char*>(&p), sizeof(p));
    CT_REQUIRE(r.Parse(bytes, &err));
    CT_CHECK_EQ(r.Header().flags, uint32_t(0));
    CT_CHECK_EQ(r.Header().region_h, 1080);
    RecordView v;
    CT_REQUIRE(r.Next(v));
    CT_CHECK_EQ(v.t_us, TimeUs(99));
    CT_CHECK(!r.Next(v));
    CT_CHECK(!r.Truncated());
}

CT_TEST(ctev_rle_roundtrip_and_choice) {
    const std::vector<uint8_t> solid = Solid(32, 32, 0x80402010u);
    const std::vector<uint8_t> rle = RleEncode(solid.data(), solid.size());
    CT_CHECK_EQ(rle.size(), size_t(5 * 5));   // 1024 pixels = 4 runs of 255 + 1 of 4
    std::vector<uint8_t> back;
    CT_CHECK(RleDecode(rle.data(), rle.size(), 32 * 32, back));
    CT_CHECK(back == solid);
    CT_CHECK(!RleDecode(rle.data(), rle.size(), 32 * 32 + 1, back));
    CT_CHECK(!RleDecode(rle.data(), rle.size(), 100, back));

    EventWriter w;
    w.OpenMemory(MakeHeader());
    const uint32_t id1 = w.CursorShape(0, 0, 32, 32, 0, 0, solid.data());
    const size_t after_solid = w.Bytes().size();
    CT_CHECK_EQ(after_solid, size_t(44 + 16 + 20 + 28));   // RLE chosen: 25 bytes padded to 28
    CT_CHECK_EQ(w.CursorShape(1, 0, 32, 32, 0, 0, solid.data()), id1);   // written once
    CT_CHECK_EQ(w.Bytes().size(), after_solid);
    const std::vector<uint8_t> noisy = Checker(8, 8);
    const uint32_t id2 = w.CursorShape(2, 0, 8, 8, 3, 3, noisy.data());
    CT_CHECK(id2 != id1);
    CT_CHECK_EQ(w.Bytes().size(), after_solid + 16 + 20 + 256);   // raw chosen
    CT_CHECK(w.HasShape(id1) && w.HasShape(id2));
    // Hotspot is part of the id.
    CT_CHECK(ShapeId(solid.data(), solid.size(), 0, 0) != ShapeId(solid.data(), solid.size(), 1, 0));
}

CT_TEST(ctev_copy_label_truncates_on_code_point) {
    char buf[4];
    CopyLabel(buf, sizeof(buf), "a\xC3\xA9\xC3\xA9");   // a é é: 5 bytes, room for 3
    CT_CHECK_EQ(std::string(buf), std::string("a\xC3\xA9"));
    CopyLabel(buf, sizeof(buf), "ab\xC3\xA9");          // would cut é in half -> "ab"
    CT_CHECK_EQ(std::string(buf), std::string("ab"));
}

CT_BENCH(ctev_write_cursor_moves, 1'000'000) {
    EventWriter w;
    w.OpenMemory(MakeHeader());
    for (int64_t i = 0; i < CT_BENCH_N; ++i) w.CursorMove(i, int32_t(i & 1023), int32_t(i & 511), 1, true);
}
