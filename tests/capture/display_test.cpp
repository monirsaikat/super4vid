// ct/capture/display.h
#include <ct/capture/display.h>
#include <ct/core/strings.h>
#include <ct/test/ct_test.h>

#include <cstdio>

using namespace ct;
using namespace ct::capture;

namespace {
DisplayInfo FakeDisplay(int x, int y, int w, int h) {
    DisplayInfo d;
    d.rect = RectI{x, y, w, h};
    d.work_rect = d.rect;
    return d;
}
} // namespace

CT_TEST(clamp_region_even_and_inside) {
    const DisplayInfo d = FakeDisplay(-1920, 0, 1920, 1080);
    RectI r = ClampRegionToDisplay(d, RectI{-1900, 10, 801, 601});
    CT_CHECK_EQ(r.x, -1900);
    CT_CHECK_EQ(r.y, 10);
    CT_CHECK_EQ(r.w, 800);
    CT_CHECK_EQ(r.h, 600);
    // Partly outside: intersected, then even.
    r = ClampRegionToDisplay(d, RectI{-101, 1000, 500, 500});
    CT_CHECK_EQ(r.x, -101);
    CT_CHECK_EQ(r.w, 100);
    CT_CHECK_EQ(r.y, 1000);
    CT_CHECK_EQ(r.h, 80);
    CT_CHECK(r.Right() <= d.rect.Right());
    CT_CHECK(r.Bottom() <= d.rect.Bottom());
    // Whole display for empty or disjoint regions.
    r = ClampRegionToDisplay(d, RectI{});
    CT_CHECK(r.x == -1920 && r.y == 0 && r.w == 1920 && r.h == 1080);
    r = ClampRegionToDisplay(d, RectI{100, 100, 50, 50});
    CT_CHECK(r.x == -1920 && r.w == 1920);
}

CT_TEST(clamp_region_minimum_64) {
    const DisplayInfo d = FakeDisplay(0, 0, 2560, 1440);
    RectI r = ClampRegionToDisplay(d, RectI{100, 100, 10, 3});
    CT_CHECK_EQ(r.w, 64);
    CT_CHECK_EQ(r.h, 64);
    CT_CHECK_EQ(r.x, 100);
    // Near the bottom-right corner the grown region is shifted back inside.
    r = ClampRegionToDisplay(d, RectI{2550, 1430, 10, 10});
    CT_CHECK_EQ(r.w, 64);
    CT_CHECK_EQ(r.h, 64);
    CT_CHECK_EQ(r.Right(), 2560);
    CT_CHECK_EQ(r.Bottom(), 1440);
    // Odd display width: never larger than the display.
    const DisplayInfo odd = FakeDisplay(0, 0, 1365, 767);
    r = ClampRegionToDisplay(odd, RectI{});
    CT_CHECK_EQ(r.w, 1364);
    CT_CHECK_EQ(r.h, 766);
    for (int w = 1; w < 300; w += 7) {
        r = ClampRegionToDisplay(d, RectI{5, 7, w, w + 1});
        CT_CHECK(r.w % 2 == 0 && r.h % 2 == 0 && r.w >= 64 && r.h >= 64);
    }
}

CT_TEST(display_index_at) {
    std::vector<DisplayInfo> v = {FakeDisplay(0, 0, 1920, 1080), FakeDisplay(1920, -200, 3840, 2160)};
    v[0].index = 0;
    v[1].index = 1;
    CT_CHECK_EQ(DisplayIndexAt(v, PointI{0, 0}), 0);
    CT_CHECK_EQ(DisplayIndexAt(v, PointI{1919, 1079}), 0);
    CT_CHECK_EQ(DisplayIndexAt(v, PointI{1920, 0}), 1);
    CT_CHECK_EQ(DisplayIndexAt(v, PointI{1920, -200}), 1);
    CT_CHECK_EQ(DisplayIndexAt(v, PointI{100, -1}), -1);
}

CT_TEST_DEVICE(enumerate_displays_sane) {
    const std::vector<DisplayInfo> displays = EnumerateDisplays(nullptr);
    CT_REQUIRE(!displays.empty());
    CT_CHECK(displays[0].primary);
    const RectI vs = VirtualScreenRect();
    int primaries = 0;
    for (const DisplayInfo& d : displays) {
        std::printf("  [%d] %ls \"%s\" %dx%d at (%d,%d) dpi %u %u Hz%s%s rot %s white %.0f nits luid %08lx:%08lx out %u\n",
                    d.index, d.device_name.c_str(), d.friendly_name.c_str(), d.rect.w, d.rect.h, d.rect.x, d.rect.y,
                    d.dpi, d.refresh_hz, d.primary ? " primary" : "", d.hdr ? " HDR" : "", RotationName(d.rotation),
                    d.sdr_white_nits, static_cast<unsigned long>(d.adapter_luid.HighPart), d.adapter_luid.LowPart,
                    d.output_index);
        CT_CHECK(d.dpi >= 96 && d.dpi <= 96 * 5);
        CT_CHECK(d.refresh_hz >= 24 && d.refresh_hz <= 500);
        CT_CHECK(d.rect.w >= 640 && d.rect.h >= 480);
        CT_CHECK(d.hmonitor != nullptr);
        CT_CHECK(!d.device_name.empty());
        CT_CHECK(d.adapter_luid.LowPart != 0 || d.adapter_luid.HighPart != 0);
        CT_CHECK(d.sdr_white_nits >= 80.f && d.sdr_white_nits <= 500.f);
        CT_CHECK(d.rect.x >= vs.x && d.rect.Right() <= vs.Right());
        CT_CHECK(d.work_rect.w <= d.rect.w);
        if (d.primary) {
            ++primaries;
            CT_CHECK_EQ(d.rect.x, 0);
            CT_CHECK_EQ(d.rect.y, 0);
        }
        CT_CHECK_EQ(DisplayIndexAt(displays, PointI{d.rect.x + d.rect.w / 2, d.rect.y + d.rect.h / 2}), d.index);
    }
    CT_CHECK_EQ(primaries, 1);
}
