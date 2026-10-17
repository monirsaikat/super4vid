// ct/capture/desktop_duplicator.h
#include "test_util.h"

#include <ct/capture/desktop_duplicator.h>
#include <ct/capture/display.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <dwmapi.h>

#include <cstdio>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;
using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D11Texture2D> MakeTarget(D3DDevice& dev, SizeI size, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(size.w);
    d.Height = static_cast<UINT>(size.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    dev.Device()->CreateTexture2D(&d, nullptr, t.GetAddressOf());
    return t;
}

// Acquires until the pixel at (px, py) of the region is pure red (BGRA 0,0,255) or the deadline passes.
bool WaitForRed(D3DDevice& dev, DesktopDuplicator& dup, ID3D11Texture2D* dst, test_util::SolidWindow& win, int px, int py,
                double seconds, int& s_ok_count, double& first_ok_ms) {
    Stopwatch sw;
    sw.Start();
    while (sw.ElapsedMs() < seconds * 1000.0) {
        win.Pump();
        int64_t present = 0;
        HRESULT hr;
        {
            D3DDevice::Lock lock(dev);
            hr = dup.AcquireInto(dst, 50, present);
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            D3DDevice::Lock lock(dev);
            dup.Reinit();
            continue;
        }
        if (FAILED(hr)) return false;
        if (hr != S_OK) continue;
        if (s_ok_count++ == 0) first_ok_ms = sw.ElapsedMs();
        std::vector<uint8_t> px_data;
        UINT w = 0, h = 0;
        if (!test_util::ReadbackTexture(dev, dst, px_data, w, h)) return false;
        const uint8_t* p = &px_data[(static_cast<size_t>(py) * w + px) * 4];
        if (p[0] == 0 && p[1] == 0 && p[2] == 255) return true;
    }
    return false;
}

} // namespace

CT_TEST_DEVICE(duplicator_first_frame_and_content) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    CT_REQUIRE(!displays.empty());
    const DisplayInfo& d = displays[0];

    test_util::SolidWindow win(d.rect.x + 300, d.rect.y + 300, 256, 256, RGB(255, 0, 0));
    CT_REQUIRE(win.Hwnd() != nullptr);
    DwmFlush();

    DesktopDuplicator::Config cfg;
    cfg.display = d;
    cfg.region = ClampRegionToDisplay(d, RectI{d.rect.x + 200, d.rect.y + 200, 640, 480});
    DesktopDuplicator dup;
    CT_REQUIRE(SUCCEEDED(dup.Init(dev, cfg)));
    CT_CHECK(!dup.IsCrossAdapter());
    CT_CHECK_EQ(dup.Size().w, 640);
    CT_CHECK_EQ(dup.Size().h, 480);
    CT_CHECK(dup.Format() == DXGI_FORMAT_B8G8R8A8_UNORM || dup.Format() == DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (dup.Format() != DXGI_FORMAT_B8G8R8A8_UNORM) CT_SKIP("HDR display: content check expects BGRA8");
    if (dup.DisplayRotation() != Rotation::Identity) CT_SKIP("rotated display: content check expects identity");

    ComPtr<ID3D11Texture2D> dst = MakeTarget(dev, dup.Size(), dup.Format());
    CT_REQUIRE(dst != nullptr);
    int oks = 0;
    double first_ms = -1;
    // The window centre is at region-local (100 + 128, 100 + 128).
    const bool red = WaitForRed(dev, dup, dst.Get(), win, 228, 228, 2.0, oks, first_ms);
    std::printf("  first S_OK after %.1f ms, %d frames until red\n", first_ms, oks);
    CT_CHECK(oks >= 1);
    CT_CHECK(first_ms >= 0 && first_ms < 2000.0);
    CT_CHECK(red);

    // Reinit while running (what ACCESS_LOST recovery does) keeps working; the new duplication delivers a frame.
    {
        D3DDevice::Lock lock(dev);
        CT_CHECK_HR(dup.Reinit());
    }
    win.SetColor(RGB(0, 0, 255));
    win.SetColor(RGB(255, 0, 0));
    oks = 0;
    CT_CHECK(WaitForRed(dev, dup, dst.Get(), win, 228, 228, 2.0, oks, first_ms));
    CT_CHECK(oks >= 1);

    // After Shutdown the output is free again for a new duplication in this process.
    dup.Shutdown();
    DesktopDuplicator dup2;
    CT_CHECK_HR(dup2.Init(dev, cfg));
}

CT_TEST_DEVICE(duplicator_rejects_odd_region) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    CT_REQUIRE(!displays.empty());
    DesktopDuplicator::Config cfg;
    cfg.display = displays[0];
    cfg.region = RectI{displays[0].rect.x, displays[0].rect.y, 641, 480};
    DesktopDuplicator dup;
    CT_CHECK(dup.Init(dev, cfg) == E_INVALIDARG);
}

CT_TEST_DEVICE(duplicator_letterboxes_region_outside_desktop) {
    // A region that sticks out of the desktop (what a mode change to a smaller resolution produces): the visible part
    // keeps its place, the rest is black.
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    CT_REQUIRE(!displays.empty());
    const DisplayInfo& d = displays[0];
    test_util::SolidWindow win(d.rect.Right() - 200, d.rect.y + 100, 200, 200, RGB(255, 0, 0));
    DesktopDuplicator::Config cfg;
    cfg.display = d;
    cfg.region = RectI{d.rect.Right() - 320, d.rect.y, 640, 480};
    DesktopDuplicator dup;
    CT_REQUIRE(SUCCEEDED(dup.Init(dev, cfg)));
    if (dup.Format() != DXGI_FORMAT_B8G8R8A8_UNORM || dup.DisplayRotation() != Rotation::Identity)
        CT_SKIP("needs an SDR, unrotated primary display");
    ComPtr<ID3D11Texture2D> dst = MakeTarget(dev, dup.Size(), dup.Format());
    int oks = 0;
    double first_ms = -1;
    // Window centre: x = 320 - 100, y = 200.
    CT_REQUIRE(WaitForRed(dev, dup, dst.Get(), win, 220, 200, 2.0, oks, first_ms));
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    CT_REQUIRE(test_util::ReadbackTexture(dev, dst.Get(), px, w, h));
    int non_black = 0;
    for (UINT y = 0; y < h; y += 7)
        for (UINT x = 320; x < w; x += 5) {
            const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            if (p[0] | p[1] | p[2]) ++non_black;
        }
    CT_CHECK_EQ(non_black, 0);
}
