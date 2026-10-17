// platform::HotkeyManager and platform::TrayIcon (P05 5.4).
#include <ct/platform/hotkeys.h>
#include <ct/platform/tray_icon.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::platform;

namespace {

HWND MessageWindow() {
    return CreateWindowExW(0, L"STATIC", L"ct-test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
}

} // namespace

CT_TEST_DEVICE(hotkeys_register_and_conflict) {
    HWND a = MessageWindow(), b = MessageWindow();
    CT_REQUIRE(a && b);
    HotkeyManager ma, mb;
    std::string error;
    // An unlikely combination so the test does not collide with the user's own hotkeys.
    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    CT_REQUIRE(ma.Register(a, 1, mods, VK_F22, &error));
    CT_CHECK(ma.IsRegistered(a, 1));
    CT_CHECK(!mb.Register(b, 7, mods, VK_F22, &error));
    std::printf("  conflict message: %s\n", error.c_str());
    CT_CHECK_EQ(error, std::string("Ctrl+Alt+Shift+F22 is already used by another app"));
    CT_CHECK(ma.Register(a, 1, mods, VK_F21, &error));   // re-register replaces the binding
    CT_CHECK_EQ(ma.Count(), static_cast<size_t>(1));
    CT_CHECK(mb.Register(b, 7, mods, VK_F22, &error));   // F22 is free again
    CT_CHECK(!ma.Register(a, 2, 0, 0, &error));
    ma.UnregisterAll();
    mb.UnregisterAll();
    CT_CHECK_EQ(ma.Count(), static_cast<size_t>(0));
    CT_CHECK_EQ(HotkeyManager::Describe(MOD_CONTROL | MOD_SHIFT, VK_F9), std::string("Ctrl+Shift+F9"));
    DestroyWindow(a);
    DestroyWindow(b);
}

CT_TEST(tray_recording_icon_pixels) {
    HICON base = LoadIconW(nullptr, IDI_APPLICATION);
    HICON icon = TrayIcon::CreateRecordingIcon(base, 32);
    CT_REQUIRE(icon != nullptr);
    ICONINFO ii{};
    CT_REQUIRE(GetIconInfo(icon, &ii));
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 32;
    bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint32_t> px(32 * 32);
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, ii.hbmColor, 0, 32, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    DestroyIcon(icon);
    CT_REQUIRE(lines == 32);
    const uint32_t dot = px[22 * 32 + 22];   // dot centre (0.70 * 32)
    std::printf("  dot pixel %08X\n", dot);
    CT_CHECK_EQ(dot >> 24, 0xFFu);
    CT_CHECK_EQ((dot >> 16) & 0xFF, 0xFFu);   // #FF4D4F
    CT_CHECK_NEAR((dot >> 8) & 0xFF, 0x4D, 2);
    CT_CHECK_NEAR(dot & 0xFF, 0x4F, 2);
    // The gap ring is transparent, the base icon is still visible in the top-left quarter.
    uint32_t max_alpha_top_left = 0;
    for (int y = 0; y < 14; ++y)
        for (int x = 0; x < 14; ++x) max_alpha_top_left = std::max(max_alpha_top_left, px[y * 32 + x] >> 24);
    CT_CHECK(max_alpha_top_left > 0);
    CT_CHECK(TrayIcon::CreateRecordingIcon(nullptr, 1000) == nullptr);
}

CT_TEST_DEVICE(tray_icon_add_modify_remove) {
    HWND w = CreateWindowExW(0, L"STATIC", L"ct-tray-test", 0, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CT_REQUIRE(w);
    TrayIcon tray;
    HICON base = LoadIconW(nullptr, IDI_APPLICATION);
    if (!tray.Add(w, base, L"Camtesia test", WM_APP + 1)) {
        DestroyWindow(w);
        CT_SKIP("no notification area (Explorer not running?)");
    }
    HICON rec = TrayIcon::CreateRecordingIcon(base);
    tray.SetIcon(rec);
    tray.SetTip(L"Recording 00:00:03");
    tray.SetIcon(base);
    tray.Remove();
    CT_CHECK(!tray.IsAdded());
    DestroyIcon(rec);
    DestroyWindow(w);
    const TrayIcon::Event e = TrayIcon::Decode(MAKEWPARAM(120, 900), MAKELPARAM(WM_CONTEXTMENU, 1));
    CT_CHECK_EQ(e.what, static_cast<UINT>(WM_CONTEXTMENU));
    CT_CHECK_EQ(e.anchor.x, 120);
    CT_CHECK_EQ(e.anchor.y, 900);
}
