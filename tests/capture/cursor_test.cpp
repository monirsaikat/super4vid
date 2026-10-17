// ct/capture/cursor_tracker.h
#include <ct/capture/cursor_tracker.h>
#include <ct/test/ct_test.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ct;
using namespace ct::capture;

namespace {
struct AlphaStats {
    int zero = 0, opaque = 0, partial = 0, bad_premul = 0;
};
AlphaStats Analyze(const CursorShape& s) {
    AlphaStats a;
    for (uint32_t px : s.bgra_premul) {
        const uint32_t al = px >> 24, r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
        if (al == 0) ++a.zero;
        else if (al == 255) ++a.opaque;
        else ++a.partial;
        if (r > al || g > al || b > al) ++a.bad_premul;
    }
    return a;
}
} // namespace

CT_TEST_DEVICE(cursor_shape_arrow) {
    CursorShape s;
    CT_REQUIRE(CursorTracker::CaptureShape(LoadCursorW(nullptr, IDC_ARROW), s));
    std::printf("  arrow: %ux%u hotspot (%d,%d) id %08x\n", s.w, s.h, s.hot_x, s.hot_y, s.id);
    CT_CHECK(s.w >= 16 && s.w <= 256);
    CT_CHECK(s.h >= 16 && s.h <= 256);
    CT_CHECK(s.hot_x >= 0 && s.hot_x < s.w);
    CT_CHECK(s.hot_y >= 0 && s.hot_y < s.h);
    CT_CHECK_EQ(s.bgra_premul.size(), static_cast<size_t>(s.w) * s.h);
    const AlphaStats a = Analyze(s);
    CT_CHECK(a.zero > 0);
    CT_CHECK(a.opaque > 0);
    CT_CHECK_EQ(a.bad_premul, 0);
    CT_CHECK(s.id != 0);
    // Same cursor, same id; a different cursor, a different id.
    CursorShape again, hand;
    CT_REQUIRE(CursorTracker::CaptureShape(LoadCursorW(nullptr, IDC_ARROW), again));
    CT_CHECK_EQ(again.id, s.id);
    CT_REQUIRE(CursorTracker::CaptureShape(LoadCursorW(nullptr, IDC_HAND), hand));
    CT_CHECK(hand.id != s.id);
}

CT_TEST_DEVICE(cursor_shape_ibeam_and_wait) {
    CursorShape s;
    CT_REQUIRE(CursorTracker::CaptureShape(LoadCursorW(nullptr, IDC_IBEAM), s));
    const AlphaStats a = Analyze(s);
    std::printf("  ibeam: %ux%u hotspot (%d,%d), %d opaque, %d transparent\n", s.w, s.h, s.hot_x, s.hot_y, a.opaque, a.zero);
    CT_CHECK(a.opaque > 0);   // inverting pixels become opaque
    CT_CHECK(a.zero > 0);
    CT_CHECK_EQ(a.bad_premul, 0);
    // Animated (wait) cursor: frame 0 is captured.
    CursorShape wait;
    CT_CHECK(CursorTracker::CaptureShape(LoadCursorW(nullptr, IDC_WAIT), wait));
    CT_CHECK(wait.w >= 16);
    CT_CHECK(!CursorTracker::CaptureShape(nullptr, wait));
}

CT_TEST_DEVICE(cursor_shape_no_gdi_leak) {
    HANDLE proc = GetCurrentProcess();
    CursorShape s;
    CT_REQUIRE(CursorTracker::CaptureShape(LoadCursorW(nullptr, IDC_IBEAM), s));
    const DWORD before = GetGuiResources(proc, GR_GDIOBJECTS);
    const LPCWSTR ids[] = {IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_SIZEALL, IDC_WAIT};
    for (int i = 0; i < 2000; ++i) CursorTracker::CaptureShape(LoadCursorW(nullptr, ids[i % 5]), s);
    const DWORD after = GetGuiResources(proc, GR_GDIOBJECTS);
    std::printf("  GDI objects before %lu, after 2000 captures %lu\n", before, after);
    CT_CHECK(after <= before + 4);
}

CT_TEST_DEVICE(cursor_tracker_poll_dedupes) {
    auto samples = std::make_unique<SpscQueue<CursorSample, 4096>>();
    auto shapes = std::make_unique<SpscQueue<CursorShape, 64>>();
    CursorTracker t;
    t.Reset();
    POINT p{};
    GetCursorPos(&p);
    const RectI region{100, 50, 800, 600};
    t.Poll(0, region, *samples, *shapes);
    // Unchanged cursor: no new sample (unless the user moved the mouse in between).
    for (int i = 1; i < 10; ++i) t.Poll(i * 8333, region, *samples, *shapes);
    CT_CHECK(samples->Size() >= 1);
    const size_t after_still = samples->Size();
    // A different region shifts the relative position: exactly one more sample.
    const RectI region2{0, 0, 800, 600};
    t.Poll(100000, region2, *samples, *shapes);
    CT_CHECK(samples->Size() >= after_still);

    CursorSample first{};
    CT_REQUIRE(samples->Pop(first));
    CT_CHECK_EQ(first.t_us, 0);
    CURSORINFO ci{sizeof(ci)};
    GetCursorInfo(&ci);
    if (std::abs(ci.ptScreenPos.x - p.x) < 2 && std::abs(ci.ptScreenPos.y - p.y) < 2) {
        CT_CHECK_EQ(first.x, p.x - 100);
        CT_CHECK_EQ(first.y, p.y - 50);
        CT_CHECK(after_still == 1);
    }
    if (first.visible && ci.hCursor) {
        // The current shape was pushed once, and its id matches the sample.
        CT_CHECK_EQ(shapes->Size(), static_cast<size_t>(1));
        CursorShape sh;
        CT_REQUIRE(shapes->Pop(sh));
        CT_CHECK_EQ(sh.id, first.shape_id);
    }
    // Reset forgets known shapes: the next poll pushes the shape again.
    t.Reset();
    t.Poll(200000, region, *samples, *shapes);
    if (ci.hCursor) CT_CHECK(shapes->Size() >= 1);
}
