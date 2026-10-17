// Review fix: damaged records must fail EventLog loads cleanly, never allocate from file-controlled sizes.
#include <ct/events/event_log.h>
#include <ct/test/ct_test.h>

#include <string>
#include <vector>

using namespace ct;
using namespace ct::events;

namespace {

EventHeader Header() {
    EventHeader h;
    h.region_w = 1920;
    h.region_h = 1080;
    return h;
}

std::string ShapeRecordFile(uint16_t w, uint16_t h, uint8_t compression, uint32_t data_size, size_t actual_data) {
    EventWriter wr;
    wr.OpenMemory(Header());
    CursorShapePayload p;
    p.shape_id = 42;
    p.w = w;
    p.h = h;
    p.compression = compression;
    p.data_size = data_size;
    std::vector<uint8_t> data(actual_data, 1);   // run length 1 everywhere: valid runs
    wr.WriteRecord(uint8_t(RecordType::CursorShape), 0, &p, sizeof(p), data.data(), data.size());
    return wr.Bytes();
}

} // namespace

CT_TEST(event_log_rejects_malformed_shapes_without_allocating) {
    struct Case { uint16_t w, h; uint8_t comp; uint32_t size; size_t actual; };
    const Case cases[] = {
        {65535, 65535, 1, 5, 5},       // RLE claiming 4 G pixels from one run (would reserve ~17 GB)
        {65535, 65535, 0, 16, 16},     // raw, size mismatch
        {2000, 10, 0, 80000, 80000},   // larger than kMaxShapeSize
        {0, 16, 1, 5, 5},              // empty
        {16, 16, 1, 10, 10},           // RLE that cannot reach 256 pixels (2 runs of 1)
        {16, 16, 0, 1000, 1000},       // raw size != 16*16*4
        {16, 16, 1, 5000, 100},        // data_size larger than the record
        {16, 16, 7, 1024, 1024},       // unknown compression
    };
    for (const Case& c : cases) {
        EventLog log;
        Err err;
        CT_CHECK(!log.LoadFromMemory(ShapeRecordFile(c.w, c.h, c.comp, c.size, c.actual), &err));
        CT_CHECK(!err.Ok());
        CT_CHECK(log.Shapes().empty());
    }
    // A well-formed shape next to them still loads.
    std::vector<uint8_t> px(8 * 8 * 4, 7);
    EventWriter ok;
    ok.OpenMemory(Header());
    const uint32_t id = ok.CursorShape(0, 0, 8, 8, 0, 0, px.data());
    EventLog good;
    CT_REQUIRE(good.LoadFromMemory(ok.Bytes(), nullptr));
    CT_CHECK(good.FindShape(id) != nullptr);

    // Direct decoder: a huge count with tiny data fails before allocating.
    const uint8_t one_run[5] = {255, 1, 2, 3, 4};
    std::vector<uint8_t> out;
    CT_CHECK(!RleDecode(one_run, sizeof(one_run), size_t(65535) * 65535, out));
    CT_CHECK(out.capacity() < 1024 * 1024);
    CT_CHECK(!RleDecode(one_run, sizeof(one_run), 0, out));
    CT_CHECK(RleDecode(one_run, sizeof(one_run), 255, out));
    CT_CHECK_EQ(out.size(), size_t(255 * 4));
    CT_CHECK_EQ(out[1016], uint8_t(1));   // first byte of the last pixel
    CT_CHECK_EQ(out[1019], uint8_t(4));

    // The writer refuses shapes it could not read back.
    EventWriter wr;
    wr.OpenMemory(Header());
    std::vector<uint8_t> big(size_t(1025) * 4, 0);
    CT_CHECK_EQ(wr.CursorShape(0, 0, 1025, 1, 0, 0, big.data()), uint32_t(0));
    CT_CHECK_EQ(wr.CursorShape(0, 0, 0, 1, 0, 0, big.data()), uint32_t(0));
    CT_CHECK_EQ(wr.RecordCount(), uint64_t(0));
}

CT_TEST(event_log_rejects_short_known_records) {
    EventWriter wr;
    wr.OpenMemory(Header());
    wr.CursorMove(1, 1, 1, 0, true);
    const uint8_t short_key[8] = {};
    wr.WriteRecord(uint8_t(RecordType::Key), 2, short_key, sizeof(short_key));   // KEY needs 44 bytes
    EventLog log;
    Err err;
    CT_CHECK(!log.LoadFromMemory(wr.Bytes(), &err));
    CT_CHECK(!err.Ok());
    CT_CHECK(log.Cursor().empty());
}
