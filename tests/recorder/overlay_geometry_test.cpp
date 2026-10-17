#include <ct/recorder/overlay_geometry.h>
#include <ct/test/ct_test.h>

#include <vector>

using namespace ct;
using namespace ct::recorder::overlay;

namespace {
bool Eq(const RectI& a, const RectI& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
#define CHECK_RECT(a, ...)                                                                                     \
    do {                                                                                                       \
        const RectI ct_got_ = (a);                                                                             \
        const RectI ct_want_ = __VA_ARGS__;                                                                    \
        if (!Eq(ct_got_, ct_want_)) {                                                                          \
            std::printf("  got {%d,%d,%d,%d} want {%d,%d,%d,%d}\n", ct_got_.x, ct_got_.y, ct_got_.w, ct_got_.h, \
                        ct_want_.x, ct_want_.y, ct_want_.w, ct_want_.h);                                       \
            CT_FAIL(#a);                                                                                       \
        }                                                                                                      \
    } while (0)
} // namespace

CT_TEST(overlay_even_rounding_and_drag_rect) {
    CHECK_RECT(MakeEven(RectI{3, 4, 101, 77}), RectI{3, 4, 100, 76});
    CHECK_RECT(MakeEven(RectI{0, 0, -5, 1}), RectI{0, 0, 0, 0});
    // Inclusive drag points: (10,10)->(19,19) covers 10x10 pixels.
    CHECK_RECT(RectFromDrag(PointI{10, 10}, PointI{19, 19}), RectI{10, 10, 10, 10});
    CHECK_RECT(RectFromDrag(PointI{19, 19}, PointI{10, 10}), RectI{10, 10, 10, 10});   // any direction
    CHECK_RECT(RectFromDrag(PointI{10, 10}, PointI{20, 14}), RectI{10, 10, 10, 4});    // 11x5 rounds down
    CHECK_RECT(RectFromDrag(PointI{5, 5}, PointI{5, 5}), RectI{5, 5, 0, 0});
}

CT_TEST(overlay_handle_hit_testing) {
    const RectI sel{100, 100, 200, 100};   // right 300, bottom 200
    CT_CHECK_EQ(HitTest(sel, PointI{100, 100}), Handle::TopLeft);
    CT_CHECK_EQ(HitTest(sel, PointI{95, 96}), Handle::TopLeft);       // slop outside the corner
    CT_CHECK_EQ(HitTest(sel, PointI{300, 100}), Handle::TopRight);
    CT_CHECK_EQ(HitTest(sel, PointI{300, 200}), Handle::BottomRight);
    CT_CHECK_EQ(HitTest(sel, PointI{100, 199}), Handle::BottomLeft);
    CT_CHECK_EQ(HitTest(sel, PointI{200, 101}), Handle::Top);
    CT_CHECK_EQ(HitTest(sel, PointI{299, 150}), Handle::Right);
    CT_CHECK_EQ(HitTest(sel, PointI{200, 202}), Handle::Bottom);
    CT_CHECK_EQ(HitTest(sel, PointI{98, 150}), Handle::Left);
    CT_CHECK_EQ(HitTest(sel, PointI{150, 130}), Handle::Inside);
    CT_CHECK_EQ(HitTest(sel, PointI{50, 50}), Handle::None);
    CT_CHECK_EQ(HitTest(sel, PointI{150, 300}), Handle::None);
    CT_CHECK_EQ(HitTest(RectI{}, PointI{0, 0}), Handle::None);
    // A tiny selection: corners win over edges and inside.
    const RectI tiny{0, 0, 12, 12};
    CT_CHECK_EQ(HitTest(tiny, PointI{6, 0}), Handle::TopLeft);
    CHECK_RECT(HandleRect(sel, Handle::Right, 10), RectI{295, 145, 10, 10});
}

CT_TEST(overlay_drag_resize_move_nudge) {
    const RectI bounds{0, 0, 1920, 1080};
    const RectI sel{100, 100, 400, 300};
    // Right edge +51 -> width 451 -> 450 (even), left edge fixed.
    CHECK_RECT(ApplyDrag(sel, Handle::Right, PointI{51, 0}, bounds), RectI{100, 100, 450, 300});
    // Left edge -51: the left edge absorbs the rounding, the right edge (500) stays.
    CHECK_RECT(ApplyDrag(sel, Handle::Left, PointI{-51, 0}, bounds), RectI{50, 100, 450, 300});
    // Top-left beyond the bounds clamps to the screen.
    CHECK_RECT(ApplyDrag(sel, Handle::TopLeft, PointI{-500, -500}, bounds), RectI{0, 0, 500, 400});
    // Dragging the bottom edge above the top keeps the minimum size.
    CHECK_RECT(ApplyDrag(sel, Handle::Bottom, PointI{0, -1000}, bounds), RectI{100, 100, 400, kMinRegion});
    // Moving clamps the whole rect inside the bounds.
    CHECK_RECT(ApplyDrag(sel, Handle::Inside, PointI{5000, -5000}, bounds), RectI{1520, 0, 400, 300});
    CHECK_RECT(ApplyDrag(sel, Handle::None, PointI{5, 5}, bounds), sel);
    CHECK_RECT(Nudge(sel, 1, 0, false, bounds), RectI{101, 100, 400, 300});
    CHECK_RECT(Nudge(sel, 0, -10, false, bounds), RectI{100, 90, 400, 300});
    CHECK_RECT(Nudge(RectI{0, 0, 100, 100}, -1, -1, false, bounds), RectI{0, 0, 100, 100});
    CHECK_RECT(Nudge(sel, 2, 2, true, bounds), RectI{100, 100, 402, 302});
    CHECK_RECT(Nudge(sel, 1, 0, true, bounds), RectI{100, 100, 400, 300});   // odd growth rounds back
}

CT_TEST(overlay_window_snapping) {
    std::vector<SnapWindow> z(6);
    z[0].rect = {0, 0, 1920, 1080};
    z[0].tool = true;                         // e.g. a transparent tool overlay on top: skipped
    z[1].rect = {100, 100, 300, 200};
    z[1].cloaked = true;                      // on another virtual desktop
    z[2].rect = {150, 150, 400, 300};
    z[2].own_process = true;                  // our own bar / overlay
    z[3].rect = {200, 200, 501, 401};         // the real target (odd size)
    z[4].rect = {0, 0, 1920, 1080};           // desktop behind it
    z[5].rect = {300, 300, 0, 0};             // zero size
    CT_CHECK_EQ(PickSnapWindow(z, PointI{250, 250}), 3);
    CT_CHECK_EQ(PickSnapWindow(z, PointI{120, 120}), 4);   // only the desktop contains it among snappable ones
    z[4].visible = false;
    CT_CHECK_EQ(PickSnapWindow(z, PointI{120, 120}), -1);
    z[4].visible = true;
    z[4].minimized = true;
    CT_CHECK_EQ(PickSnapWindow(z, PointI{120, 120}), -1);
    // Snap rect: intersected with the display under the cursor, even size.
    CHECK_RECT(SnapRectAt(z, PointI{250, 250}, RectI{0, 0, 1920, 1080}), RectI{200, 200, 500, 400});
    CHECK_RECT(SnapRectAt(z, PointI{250, 250}, RectI{0, 0, 400, 1080}), RectI{200, 200, 200, 400});
    CHECK_RECT(SnapRectAt(z, PointI{10, 10}, RectI{0, 0, 1920, 1080}), RectI{});
}

CT_TEST(overlay_finalize_region_monitor) {
    const std::vector<RectI> displays = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
    FinalRegion f = Finalize(RectI{100, 100, 801, 601}, displays);
    CT_CHECK_EQ(f.display, 0);
    CHECK_RECT(f.region, RectI{100, 100, 800, 600});
    // Spanning two monitors: the larger overlap wins and the region is clamped to it.
    f = Finalize(RectI{1800, 100, 400, 300}, displays);   // 120 px on 0, 280 px on 1
    CT_CHECK_EQ(f.display, 1);
    CHECK_RECT(f.region, RectI{1920, 100, 280, 300});
    // Tiny regions grow to 64x64 inside the display.
    f = Finalize(RectI{1900, 1070, 10, 10}, displays);
    CT_CHECK_EQ(f.display, 0);
    CHECK_RECT(f.region, RectI{1856, 1016, 64, 64});
    // Equal overlap: the display containing the centre wins.
    f = Finalize(RectI{1820, 0, 200, 100}, displays);
    CT_CHECK_EQ(f.display, 1);
    f = Finalize(RectI{5000, 5000, 100, 100}, displays);
    CT_CHECK_EQ(f.display, -1);
    f = Finalize(RectI{}, displays);
    CT_CHECK_EQ(f.display, -1);
    // Monitor presets cycle.
    CT_CHECK_EQ(NextMonitor(-1, 2), 0);
    CT_CHECK_EQ(NextMonitor(0, 2), 1);
    CT_CHECK_EQ(NextMonitor(1, 2), 0);
    CT_CHECK_EQ(NextMonitor(0, 0), -1);
}

CT_TEST(overlay_labels_frame_and_countdown_geometry) {
    CT_CHECK_EQ(SizeLabel(1920, 1080), std::string("1920 \xC3\x97 1080"));
    const RectI bounds{0, 0, 1920, 1080};
    // Below the bottom-right corner, right-aligned.
    CHECK_RECT(LabelRect(RectI{100, 100, 400, 300}, SizeI{120, 26}, bounds), RectI{380, 408, 120, 26});
    // No room below: above the top edge.
    CHECK_RECT(LabelRect(RectI{100, 700, 400, 370}, SizeI{120, 26}, bounds), RectI{380, 666, 120, 26});
    // Full screen: inside the bottom-right corner.
    CHECK_RECT(LabelRect(bounds, SizeI{120, 26}, bounds), RectI{1800, 1046, 120, 26});
    // Clamped at the left edge.
    CHECK_RECT(LabelRect(RectI{0, 100, 40, 40}, SizeI{120, 26}, bounds), RectI{0, 148, 120, 26});

    const std::vector<RectI> displays = {{0, 0, 1920, 1080}, {1920, 0, 1920, 1080}};
    FrameEdges f = FrameEdgesFor(RectI{100, 100, 400, 300}, displays, 2);
    CHECK_RECT(f.edge[0], RectI{98, 98, 404, 2});
    CHECK_RECT(f.edge[1], RectI{98, 400, 404, 2});
    CHECK_RECT(f.edge[2], RectI{98, 100, 2, 300});
    CHECK_RECT(f.edge[3], RectI{500, 100, 2, 300});
    // Full primary monitor: top, bottom and left would be off-screen and move inside; the right edge is on monitor 1.
    f = FrameEdgesFor(RectI{0, 0, 1920, 1080}, displays, 2);
    CHECK_RECT(f.edge[0], RectI{0, 0, 1920, 2});
    CHECK_RECT(f.edge[1], RectI{0, 1078, 1920, 2});
    CHECK_RECT(f.edge[2], RectI{0, 0, 2, 1080});
    CHECK_RECT(f.edge[3], RectI{1920, 0, 2, 1080});

    CHECK_RECT(CenteredSquare(RectI{0, 0, 1920, 1080}, 160), RectI{880, 460, 160, 160});
    CHECK_RECT(CenteredSquare(RectI{10, 10, 100, 64}, 160), RectI{28, 10, 64, 64});
    CHECK_RECT(Union(RectI{0, 0, 10, 10}, RectI{20, 5, 10, 10}), RectI{0, 0, 30, 15});
    CHECK_RECT(Union(RectI{}, RectI{1, 2, 3, 4}), RectI{1, 2, 3, 4});
}
