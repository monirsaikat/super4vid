// platform::Nv12Blit lives in ct_platform, which has no test exe of its own; its tests run here with the decoder tests.
#include <ct/platform/nv12_blit.h>

#include "../common/rgba_readback.h"
#include "../common/test_device.h"

#include <ct/test/ct_test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ct;
using Microsoft::WRL::ComPtr;

namespace {

struct Rgb { float r, g, b; };

// The forward BT.709 limited-range transform used by the capture converter (nv12_common.hlsli).
void RgbToYuv(const Rgb& c, uint8_t& y, uint8_t& u, uint8_t& v) {
    auto q = [](float x) { return static_cast<uint8_t>(std::lround(std::fmin(std::fmax(x, 0.0f), 1.0f) * 255.0f)); };
    y = q(0.1826f * c.r + 0.6142f * c.g + 0.0620f * c.b + 16.0f / 255.0f);
    u = q(-0.1006f * c.r - 0.3386f * c.g + 0.4392f * c.b + 128.0f / 255.0f);
    v = q(0.4392f * c.r - 0.3989f * c.g - 0.0403f * c.b + 128.0f / 255.0f);
}

// An NV12 texture of w x h made of vertical bars of `colors` (each bar w / n wide, chroma-aligned).
HRESULT MakeBars(platform::D3DDevice& dev, int w, int h, const std::vector<Rgb>& colors, ComPtr<ID3D11Texture2D>& tex) {
    std::vector<uint8_t> data(static_cast<size_t>(w) * h * 3 / 2);
    const int bar = w / static_cast<int>(colors.size());
    for (int x = 0; x < w; x += 2) {
        uint8_t y = 0, u = 0, v = 0;
        RgbToYuv(colors[std::min<size_t>(x / bar, colors.size() - 1)], y, u, v);
        for (int r = 0; r < h; ++r) {
            data[static_cast<size_t>(r) * w + x] = y;
            data[static_cast<size_t>(r) * w + x + 1] = y;
        }
        for (int r = 0; r < h / 2; ++r) {
            data[static_cast<size_t>(w) * h + static_cast<size_t>(r) * w + x] = u;
            data[static_cast<size_t>(w) * h + static_cast<size_t>(r) * w + x + 1] = v;
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data.data(), static_cast<UINT>(w), 0};
    return dev.Device()->CreateTexture2D(&d, &init, tex.GetAddressOf());
}

HRESULT MakeRt(platform::D3DDevice& dev, int w, int h, ComPtr<ID3D11Texture2D>& tex) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return dev.Device()->CreateTexture2D(&d, nullptr, tex.GetAddressOf());
}

const std::vector<Rgb> kColors = {{1, 1, 1}, {1, 1, 0}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {1, 0, 0}, {0, 0, 1}, {0, 0, 0},
                                  {0.5f, 0.5f, 0.5f}, {0.2f, 0.4f, 0.8f}};

} // namespace

CT_TEST_DEVICE(nv12_blit_matches_bt709_reference) {
    CT_REQUIRE_DEVICE(dev);
    platform::Nv12Blit blit;
    CT_REQUIRE(SUCCEEDED(blit.Init(*dev)));
    ComPtr<ID3D11Texture2D> nv12, rt;
    CT_REQUIRE(SUCCEEDED(MakeBars(*dev, 640, 64, kColors, nv12)));
    CT_REQUIRE(SUCCEEDED(MakeRt(*dev, 640, 64, rt)));
    {
        platform::D3DDevice::Lock lock(*dev);
        CT_CHECK_HR(blit.ToRgba(dev->Context(), nv12.Get(), rt.Get()));
    }
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    CT_REQUIRE(test::ReadbackRgba8(*dev, rt.Get(), px, w, h));
    int max_err = 0;
    for (size_t i = 0; i < kColors.size(); ++i) {
        const int x = static_cast<int>(i) * 64 + 32, y = 32;   // middle of each bar
        const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        const int ref[3] = {static_cast<int>(std::lround(kColors[i].r * 255)), static_cast<int>(std::lround(kColors[i].g * 255)),
                            static_cast<int>(std::lround(kColors[i].b * 255))};
        for (int c = 0; c < 3; ++c) max_err = std::max(max_err, std::abs(p[c] - ref[c]));
        CT_CHECK_EQ(static_cast<int>(p[3]), 255);
    }
    std::printf("    max |dRGB| %d\n", max_err);
    CT_CHECK(max_err <= 3);   // 8-bit YUV quantization of the reference: <= 2-3 steps
}

CT_TEST_DEVICE(nv12_blit_box_downscale_keeps_flat_colour) {
    CT_REQUIRE_DEVICE(dev);
    platform::Nv12Blit blit;
    CT_REQUIRE(SUCCEEDED(blit.Init(*dev)));
    ComPtr<ID3D11Texture2D> nv12, rt;
    CT_REQUIRE(SUCCEEDED(MakeBars(*dev, 1920, 1080, {{0.2f, 0.4f, 0.8f}}, nv12)));
    CT_REQUIRE(SUCCEEDED(MakeRt(*dev, 160, 90, rt)));
    ComPtr<ID3D11ShaderResourceView> y, uv;
    CT_REQUIRE(SUCCEEDED(platform::Nv12Blit::CreatePlaneViews(dev->Device(), nv12.Get(), y.GetAddressOf(), uv.GetAddressOf())));
    ComPtr<ID3D11RenderTargetView> rtv;
    CT_REQUIRE(SUCCEEDED(dev->Device()->CreateRenderTargetView(rt.Get(), nullptr, rtv.GetAddressOf())));
    {
        platform::D3DDevice::Lock lock(*dev);
        D3D11_VIEWPORT vp{0, 0, 160, 90, 0, 1};
        CT_CHECK_HR(blit.Draw(dev->Context(), y.Get(), uv.Get(), {1920, 1080}, rtv.Get(), vp, true));
    }
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    CT_REQUIRE(test::ReadbackRgba8(*dev, rt.Get(), px, w, h));
    const int ref[3] = {51, 102, 204};
    int max_err = 0;
    for (size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c) max_err = std::max(max_err, std::abs(px[i + c] - ref[c]));
    CT_CHECK(max_err <= 3);
}
