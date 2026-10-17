#include <ct/playback/cache_budget.h>
#include <ct/playback/frame_cache.h>

#include <ct/test/ct_test.h>

using namespace ct;
using namespace ct::playback;

namespace {

FrameCache::Entry E(TimeUs pts, TimeUs dur, uint32_t slot, uint64_t used, size_t bytes = 100) {
    FrameCache::Entry e;
    e.pts_us = pts;
    e.duration_us = dur;
    e.slot = slot;
    e.last_used = used;
    e.bytes = bytes;
    return e;
}

} // namespace

CT_TEST(frame_cache_find_with_gaps) {
    FrameCache c;
    bool exact = true;
    CT_CHECK_EQ(c.Find(0, exact), -1);
    CT_CHECK(!exact);
    // 60 fps frames 0, 1, 2 and 5 (rounded durations leave 1 us holes), inserted out of order.
    c.Insert(E(33333, 16666, 2, 1));
    c.Insert(E(0, 16666, 0, 1));
    c.Insert(E(83333, 16666, 5, 1));
    c.Insert(E(16666, 16666, 1, 1));
    CT_CHECK_EQ(c.Count(), 4u);
    CT_CHECK_EQ(c.At(0).pts_us, 0);
    CT_CHECK_EQ(c.At(3).pts_us, 83333);
    CT_CHECK_EQ(c.Find(0, exact), 0);
    CT_CHECK(exact);
    CT_CHECK_EQ(c.Find(16665, exact), 0);
    CT_CHECK(exact);
    CT_CHECK_EQ(c.Find(33332, exact), 1);   // inside the 1 us hole between frames 1 and 2: still frame 1
    CT_CHECK(exact);
    CT_CHECK_EQ(c.Find(40000, exact), 2);
    CT_CHECK(exact);
    CT_CHECK_EQ(c.Find(60000, exact), 2);   // frames 3 and 4 missing: nearest earlier, not exact
    CT_CHECK(!exact);
    CT_CHECK_EQ(c.Find(90000, exact), 3);
    CT_CHECK(exact);
    CT_CHECK_EQ(c.Find(100000, exact), 3);  // after the last frame
    CT_CHECK(!exact);
    CT_CHECK_EQ(c.Find(-5, exact), -1);
    CT_CHECK_EQ(c.FindPts(33333), 2 - 1 + 1);
    CT_CHECK_EQ(c.FindPts(33334), -1);
}

CT_TEST(frame_cache_insert_replace_and_bytes) {
    FrameCache c;
    c.Insert(E(0, 10, 0, 1, 100));
    c.Insert(E(10, 10, 1, 1, 100));
    CT_CHECK_EQ(c.Bytes(), 200u);
    bool replaced = false;
    uint32_t old_slot = 99;
    c.Insert(E(10, 10, 7, 2, 50), &replaced, &old_slot);
    CT_CHECK(replaced);
    CT_CHECK_EQ(old_slot, 1u);
    CT_CHECK_EQ(c.Count(), 2u);
    CT_CHECK_EQ(c.Bytes(), 150u);
    const FrameCache::Entry e = c.Erase(0);
    CT_CHECK_EQ(e.slot, 0u);
    CT_CHECK_EQ(c.Bytes(), 50u);
}

CT_TEST(frame_cache_eviction_order) {
    FrameCache c;
    // Frames every 100 us; window [1000, 2000), playhead 1500, forward.
    c.Insert(E(100, 100, 1, 5));    // outside, used at 5
    c.Insert(E(300, 100, 3, 2));    // outside, used at 2  <- first victim (outside, LRU)
    c.Insert(E(1100, 100, 11, 1));  // inside, behind the playhead, LRU among those
    c.Insert(E(1300, 100, 13, 0));  // inside, behind, but pinned
    c.Insert(E(1600, 100, 16, 0));  // inside, ahead: never
    c.Insert(E(2500, 100, 25, 9));  // outside, recently used
    const uint32_t pinned[1] = {13};
    int v = c.PickVictim(1000, 2000, 1500, true, pinned, 1);
    CT_CHECK_EQ(c.At(v).slot, 3u);
    c.Erase(v);
    v = c.PickVictim(1000, 2000, 1500, true, pinned, 1);
    CT_CHECK_EQ(c.At(v).slot, 1u);
    c.Erase(v);
    v = c.PickVictim(1000, 2000, 1500, true, pinned, 1);
    CT_CHECK_EQ(c.At(v).slot, 25u);
    c.Erase(v);
    v = c.PickVictim(1000, 2000, 1500, true, pinned, 1);
    CT_CHECK_EQ(c.At(v).slot, 11u);   // outside exhausted: behind the playhead
    c.Erase(v);
    CT_CHECK_EQ(c.PickVictim(1000, 2000, 1500, true, pinned, 1), -1);   // pinned + ahead remain
    // Backward playback: frames after the playhead are "behind".
    CT_CHECK_EQ(c.At(c.PickVictim(1000, 2000, 1500, false, nullptr, 0)).slot, 16u);
    // Touch changes LRU order.
    FrameCache d;
    d.Insert(E(0, 10, 0, 1));
    d.Insert(E(10, 10, 1, 2));
    d.Touch(0, 10);
    CT_CHECK_EQ(d.At(d.PickVictim(100, 200, 150, true, nullptr, 0)).slot, 1u);
}

CT_TEST(frame_cache_first_gap) {
    FrameCache c;
    CT_CHECK_EQ(c.FirstGap(500, 1000), 500);
    c.Insert(E(0, 16666, 0, 0));
    c.Insert(E(16667, 16666, 1, 0));   // 1 us hole: contiguous
    c.Insert(E(33333, 16666, 2, 0));
    c.Insert(E(66666, 16666, 4, 0));   // frame 3 missing
    CT_CHECK_EQ(c.FirstGap(0, 1000), 49999);
    CT_CHECK_EQ(c.FirstGap(20000, 1000), 49999);
    CT_CHECK_EQ(c.FirstGap(55000, 1000), 55000);   // not covered
    CT_CHECK_EQ(c.FirstGap(70000, 1000), 83332);
}

CT_TEST(cache_budget_accounting) {
    const size_t old = CacheBudget::Bytes();
    const size_t used = CacheBudget::Used();
    CacheBudget::SetBytes(1000);
    CT_CHECK_EQ(CacheBudget::Bytes(), 1000u);
    CacheBudget::Add(600);
    CT_CHECK_EQ(CacheBudget::Used(), used + 600);
    CT_CHECK(CacheBudget::Fits(1000 - used - 600));
    CT_CHECK(!CacheBudget::Fits(1000 - used - 600 + 1));
    CacheBudget::Add(-600);
    CT_CHECK_EQ(CacheBudget::Used(), used);
    CacheBudget::SetBytes(old);
}
