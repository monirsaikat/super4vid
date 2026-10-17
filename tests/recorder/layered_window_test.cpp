// platform::LayeredWindow (P05 5.5): create, full and dirty-rect presents, exclusion from capture, resize.
#include <ct/platform/layered_window.h>
#include <ct/test/ct_test.h>

#include <cstdio>

using namespace ct;
using namespace ct::platform;

CT_TEST_DEVICE(layered_window_draw_present_exclude) {
    LayeredWindow w;
    CT_REQUIRE(w.Create(RectI{40, 40, 320, 200}, true, true, true));
    ID2D1RenderTarget* rt = w.BeginDraw();
    CT_REQUIRE(rt != nullptr);
    rt->Clear(D2D1::ColorF(1.f, 0.3f, 0.3f, 0.5f));
    w.EndDrawAndPresent();
    w.Show(true);
    CT_CHECK(w.IsVisible());
    // Dirty-rect redraw after the first full present.
    const RectI dirty{10, 10, 50, 50};
    rt = w.BeginDraw(&dirty);
    CT_REQUIRE(rt != nullptr);
    rt->Clear(D2D1::ColorF(0.f, 0.f, 1.f, 1.f));
    w.EndDrawAndPresent(200);
    w.Present(255);
    std::printf("  last draw %.2f ms, excluded from capture: %s\n", w.LastDrawMs(), w.ExcludedFromCapture() ? "yes" : "no");
    DWORD affinity = 0;
    CT_CHECK(GetWindowDisplayAffinity(w.Hwnd(), &affinity));
    if (w.ExcludedFromCapture()) CT_CHECK_EQ(affinity, static_cast<DWORD>(WDA_EXCLUDEFROMCAPTURE));
    const LONG_PTR ex = GetWindowLongPtrW(w.Hwnd(), GWL_EXSTYLE);
    CT_CHECK((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT) && (ex & WS_EX_TOPMOST) && (ex & WS_EX_NOACTIVATE));
    w.Move(RectI{60, 60, 400, 220});   // resize: new surface, full present next
    rt = w.BeginDraw(&dirty);
    CT_REQUIRE(rt != nullptr);
    w.EndDrawAndPresent();
    RECT rc{};
    GetWindowRect(w.Hwnd(), &rc);
    CT_CHECK_EQ(rc.right - rc.left, 400);
    CT_CHECK_EQ(rc.left, 60);
    w.Destroy();
    CT_CHECK(w.Hwnd() == nullptr);
}
