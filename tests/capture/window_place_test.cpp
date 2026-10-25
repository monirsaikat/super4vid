// ct::platform::PlaceWindow: a new window never ends up larger than the monitor work area or off its edges
// (a laptop screen, or a scaled one where the editor's 1600x900 DIP default is 2400x1350 px).
#include <ct/platform/window.h>
#include <ct/test/ct_test.h>

using ct::PointI;
using ct::SizeI;
using ct::platform::PlaceWindow;
using ct::platform::WindowPlacement;

namespace {
constexpr SizeI kMin{656, 439};   // 640x400 DIP client + frame, the editor's minimum
}

CT_TEST(place_window_centres_a_window_that_fits) {
    const WindowPlacement p = PlaceWindow(SizeI{1616, 939}, SizeI{2560, 1392}, kMin, PointI{0, 48});
    CT_CHECK_EQ(p.w, 1616);
    CT_CHECK_EQ(p.h, 939);
    CT_CHECK_EQ(p.x, (2560 - 1616) / 2);
    CT_CHECK_EQ(p.y, 48 + (1392 - 939) / 2);
}

CT_TEST(place_window_fits_a_laptop_screen) {
    // 1366x768 laptop with a 40 px taskbar: the 1616x939 default must shrink to the work area, not overflow it.
    const WindowPlacement p = PlaceWindow(SizeI{1616, 939}, SizeI{1366, 728}, kMin, PointI{0, 0});
    CT_CHECK_EQ(p.w, 1366);
    CT_CHECK_EQ(p.h, 728);
    CT_CHECK_EQ(p.x, 0);
    CT_CHECK_EQ(p.y, 0);
}

CT_TEST(place_window_fits_a_scaled_screen) {
    // 1920x1080 at 150 %: the editor asks for 2400x1350 px, the work area is 1920x1040.
    const WindowPlacement p = PlaceWindow(SizeI{2424, 1409}, SizeI{1920, 1040}, SizeI{984, 659}, PointI{0, 0});
    CT_CHECK_EQ(p.w, 1920);
    CT_CHECK_EQ(p.h, 1040);
    CT_CHECK_EQ(p.x, 0);
    CT_CHECK_EQ(p.y, 0);
}

CT_TEST(place_window_keeps_the_minimum_and_stays_inside) {
    // A work area smaller than the minimum usable frame: the window keeps the minimum and starts at the work origin
    // (clipped at the bottom right) instead of being centred half off-screen.
    const WindowPlacement p = PlaceWindow(SizeI{1616, 939}, SizeI{480, 320}, kMin, PointI{100, 60});
    CT_CHECK_EQ(p.w, kMin.w);
    CT_CHECK_EQ(p.h, kMin.h);
    CT_CHECK_EQ(p.x, 100);
    CT_CHECK_EQ(p.y, 60);
}

CT_TEST(place_window_uses_the_work_origin_of_a_second_monitor) {
    const WindowPlacement p = PlaceWindow(SizeI{800, 600}, SizeI{1920, 1080}, kMin, PointI{-1920, 0});
    CT_CHECK_EQ(p.x, -1920 + (1920 - 800) / 2);
    CT_CHECK_EQ(p.y, (1080 - 600) / 2);
}

CT_TEST(place_window_tolerates_an_unknown_work_area) {
    // GetMonitorInfo failed: keep the requested size and do not move the window off the origin.
    const WindowPlacement p = PlaceWindow(SizeI{1616, 939}, SizeI{0, 0}, kMin, PointI{0, 0});
    CT_CHECK_EQ(p.w, 1616);
    CT_CHECK_EQ(p.h, 939);
    CT_CHECK_EQ(p.x, 0);
    CT_CHECK_EQ(p.y, 0);
}
