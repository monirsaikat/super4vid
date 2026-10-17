#include <ct/events/event_log.h>
#include <ct/recorder/typing.h>
#include <ct/test/ct_test.h>

#include <vector>

using namespace ct;
using namespace ct::recorder;

namespace {
struct Collector {
    TypingDetector det;
    std::vector<TypingDetector::Edge> edges;
    void Key(TimeUs t) {
        TypingDetector::Edge e[2];
        const int n = det.OnKeyDown(t, e);
        for (int i = 0; i < n; ++i) edges.push_back(e[i]);
    }
    void Tick(TimeUs t) {
        TypingDetector::Edge e[1];
        if (det.Tick(t, e)) edges.push_back(e[0]);
    }
    void Finish() {
        TypingDetector::Edge e[1];
        if (det.Finish(e)) edges.push_back(e[0]);
    }
};
} // namespace

CT_TEST(typing_bursts_from_key_downs) {
    Collector c;
    c.Key(100'000);
    c.Key(400'000);
    c.Key(1'300'000);   // 0.9 s gap: same burst
    c.Tick(2'000'000);  // 0.7 s since the last key: still open
    CT_CHECK(c.det.InBurst());
    c.Tick(2'300'000);  // 1.0 s: closes at the last key
    CT_CHECK(!c.det.InBurst());
    c.Key(5'000'000);   // new burst
    c.Key(7'000'000);   // 2 s gap without a tick: ended + started in one call
    c.Finish();
    c.Finish();         // idempotent

    const std::vector<TypingDetector::Edge> want = {
        {100'000, true}, {1'300'000, false}, {5'000'000, true}, {5'000'000, false}, {7'000'000, true}, {7'000'000, false}};
    CT_REQUIRE(c.edges.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        CT_CHECK_EQ(c.edges[i].t_us, want[i].t_us);
        CT_CHECK_EQ(c.edges[i].started, want[i].started);
    }
}

CT_TEST(typing_records_match_event_log_derivation) {
    // TYPING written by the detector must give the same bursts EventLog derives from key-downs alone.
    const TimeUs keys[] = {10'000, 200'000, 900'000, 2'500'000, 2'600'000, 4'000'000, 4'999'999, 7'000'000};
    events::EventHeader h;
    h.region_w = 100;
    h.region_h = 100;
    h.flags = events::kFlagKeystrokes | events::kFlagAllKeys;
    events::EventWriter with, without;
    with.OpenMemory(h);
    without.OpenMemory(h);
    Collector c;
    for (TimeUs t : keys) {
        c.Key(t);
        events::KeyPayload k;
        k.vk = 'A';
        k.down = 1;
        k.label[0] = 'A';
        with.Key(t, k);
        without.Key(t, k);
    }
    c.Finish();
    for (const auto& e : c.edges) with.Typing(e.t_us, e.started);

    events::EventLog a, b;
    Err err;
    CT_REQUIRE(a.LoadFromMemory(with.Bytes(), &err));
    CT_REQUIRE(b.LoadFromMemory(without.Bytes(), &err));
    CT_REQUIRE(a.TypingBursts().size() == b.TypingBursts().size());
    CT_CHECK_EQ(a.TypingBursts().size(), static_cast<size_t>(4));
    for (size_t i = 0; i < a.TypingBursts().size(); ++i) {
        CT_CHECK_EQ(a.TypingBursts()[i].start_us, b.TypingBursts()[i].start_us);
        CT_CHECK_EQ(a.TypingBursts()[i].end_us, b.TypingBursts()[i].end_us);
    }
}
