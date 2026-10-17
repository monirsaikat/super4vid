// ct/capture/color_convert.h
#include "test_util.h"

#include <ct/capture/color_convert.h>
#include <ct/test/ct_test.h>

#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;
using Microsoft::WRL::ComPtr;

namespace {

const Rotation kRotations[] = {Rotation::Identity, Rotation::R90, Rotation::R180, Rotation::R270};

ComPtr<ID3D11Texture2D> MakeTexture(D3DDevice& dev, int w, int h, DXGI_FORMAT format, UINT bind, const void* data, UINT pitch) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.BindFlags = bind;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(dev.Device()->CreateTexture2D(&d, nullptr, t.GetAddressOf()))) return nullptr;
    if (data) {
        D3DDevice::Lock lock(dev);
        dev.Context()->UpdateSubresource(t.Get(), 0, nullptr, data, pitch, 0);
    }
    return t;
}

// smooth = false adds a sawtooth red channel with hard edges (worst case for chroma averaging).
std::vector<uint8_t> Gradient(int w, int h, bool smooth = false) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<uint8_t>(y * 255 / (h - 1));
            p[2] = smooth ? static_cast<uint8_t>((x + y) * 255 / (w + h - 2)) : static_cast<uint8_t>((x * 3 + y * 5) & 0xFF);
            p[3] = 255;
        }
    return px;
}

// The desktop-orientation image the converter must produce from a native-orientation source.
std::vector<uint8_t> RotateRef(const std::vector<uint8_t>& src, int sw, int sh, Rotation rot, int dw, int dh) {
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x) {
            float su, sv;
            RotateUvRef(rot, (x + 0.5f) / dw, (y + 0.5f) / dh, su, sv);
            const int sx = static_cast<int>(std::floor(su * sw)), sy = static_cast<int>(std::floor(sv * sh));
            std::memcpy(&out[(static_cast<size_t>(y) * dw + x) * 4], &src[(static_cast<size_t>(sy) * sw + sx) * 4], 4);
        }
    return out;
}

struct Diff {
    int max_y = 0, max_uv = 0, samples = 0;
};

Diff CompareNv12(const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& ref, int w, int h, int samples) {
    Diff d;
    uint32_t seed = 12345;
    auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int k = 0; k < samples; ++k) {
        const int x = static_cast<int>(next() % static_cast<uint32_t>(w)), y = static_cast<int>(next() % static_cast<uint32_t>(h));
        const size_t yi = static_cast<size_t>(y) * w + x;
        d.max_y = std::max(d.max_y, std::abs(gpu[yi] - ref[yi]));
        const size_t ci = static_cast<size_t>(w) * h + static_cast<size_t>(y / 2) * w + (x / 2) * 2;
        d.max_uv = std::max(d.max_uv, std::abs(gpu[ci] - ref[ci]));
        d.max_uv = std::max(d.max_uv, std::abs(gpu[ci + 1] - ref[ci + 1]));
        ++d.samples;
    }
    return d;
}

bool RunConvert(D3DDevice& dev, ColorConverter& conv, ID3D11Texture2D* src, int dw, int dh, std::vector<uint8_t>& nv12) {
    ComPtr<ID3D11Texture2D> dst = MakeTexture(dev, dw, dh, DXGI_FORMAT_NV12, conv.DstBindFlags(), nullptr, 0);
    if (!dst) return false;
    {
        D3DDevice::Lock lock(dev);
        if (FAILED(conv.Convert(dev.Context(), src, dst.Get()))) return false;
    }
    UINT w = 0, h = 0;
    return test_util::ReadbackTexture(dev, dst.Get(), nv12, w, h);
}

} // namespace

CT_TEST(rotation_uv_matrices_match_reference) {
    for (Rotation r : kRotations) {
        float m[16];
        RotationUvMatrix(r, m);
        for (float u = 0.f; u <= 1.f; u += 0.125f)
            for (float v = 0.f; v <= 1.f; v += 0.25f) {
                // Row vector [u v 0 1] * M (HLSL row_major mul(float4(uv,0,1), M)).
                const float su = u * m[0] + v * m[4] + 0.f * m[8] + m[12];
                const float sv = u * m[1] + v * m[5] + 0.f * m[9] + m[13];
                float ru, rv;
                RotateUvRef(r, u, v, ru, rv);
                CT_CHECK_NEAR(su, ru, 1e-6);
                CT_CHECK_NEAR(sv, rv, 1e-6);
            }
    }
    // The spec's formulas, spelled out once.
    float su, sv;
    RotateUvRef(Rotation::R90, 0.25f, 0.75f, su, sv);
    CT_CHECK(su == 0.75f && sv == 0.75f);
    RotateUvRef(Rotation::R180, 0.25f, 0.75f, su, sv);
    CT_CHECK(su == 0.75f && sv == 0.25f);
    RotateUvRef(Rotation::R270, 0.25f, 0.75f, su, sv);
    CT_CHECK(su == 0.25f && sv == 0.25f);
}

CT_TEST(rgb_to_nv12_reference_values) {
    // 2x2 blocks of white, black, red, mid grey.
    const uint8_t colors[4][4] = {{255, 255, 255, 255}, {0, 0, 0, 255}, {0, 0, 255, 255}, {128, 128, 128, 255}};
    const int expect[4][3] = {{235, 128, 128}, {16, 128, 128}, {63, 102, 240}, {126, 128, 128}};
    for (int c = 0; c < 4; ++c) {
        uint8_t px[16];
        for (int i = 0; i < 4; ++i) std::memcpy(px + i * 4, colors[c], 4);
        uint8_t y[4], uv[2];
        RgbToNv12Ref(px, 2, 2, y, uv);
        CT_CHECK_NEAR(y[0], expect[c][0], 1);
        CT_CHECK_NEAR(uv[0], expect[c][1], 1);
        CT_CHECK_NEAR(uv[1], expect[c][2], 1);
    }
}

CT_TEST_DEVICE(gpu_convert_matches_reference) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    if (!dev.SupportsNv12RenderTarget()) CT_SKIP("no NV12 render target support");
    const int sw = 640, sh = 360;
    const std::vector<uint8_t> src_px = Gradient(sw, sh);
    ComPtr<ID3D11Texture2D> src = MakeTexture(dev, sw, sh, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                              src_px.data(), sw * 4);
    CT_REQUIRE(src != nullptr);
    for (int compute = 1; compute >= 0; --compute)
    for (Rotation r : kRotations) {
        const bool swap = r == Rotation::R90 || r == Rotation::R270;
        const int dw = swap ? sh : sw, dh = swap ? sw : sh;
        ColorConverter conv;
        ColorConverter::Config cfg;
        cfg.src_size = SizeI{sw, sh};
        cfg.src_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        cfg.rotation = r;
        cfg.dst_size = SizeI{dw, dh};
        cfg.allow_compute = compute != 0;
        CT_REQUIRE(SUCCEEDED(conv.Init(dev, cfg)));
        CT_CHECK(!conv.UsingVideoProcessorFallback());
        if (!compute) CT_CHECK(!conv.UsingCompute());
        std::vector<uint8_t> gpu;
        CT_REQUIRE(RunConvert(dev, conv, src.Get(), dw, dh, gpu));
        const std::vector<uint8_t> rotated = RotateRef(src_px, sw, sh, r, dw, dh);
        std::vector<uint8_t> ref(static_cast<size_t>(dw) * dh * 3 / 2);
        RgbToNv12Ref(rotated.data(), dw, dh, ref.data(), ref.data() + static_cast<size_t>(dw) * dh);
        const Diff d = CompareNv12(gpu, ref, dw, dh, 10000);
        std::printf("  %s, rotation %s: max |dY| %d, max |dUV| %d over %d samples\n", conv.UsingCompute() ? "compute" : "pixel shaders",
                    RotationName(r), d.max_y, d.max_uv, d.samples);
        CT_CHECK(d.max_y <= 2);
        CT_CHECK(d.max_uv <= 2);
    }
}

CT_TEST_DEVICE(gpu_convert_video_processor_fallback) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    if (!dev.VideoDevice()) CT_SKIP("no video device");
    const int w = 640, h = 360;
    const std::vector<uint8_t> src_px = Gradient(w, h, true);
    ComPtr<ID3D11Texture2D> src = MakeTexture(dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                              src_px.data(), w * 4);
    ColorConverter conv;
    ColorConverter::Config cfg;
    cfg.src_size = SizeI{w, h};
    cfg.dst_size = SizeI{w, h};
    cfg.force_video_processor = true;
    CT_REQUIRE(SUCCEEDED(conv.Init(dev, cfg)));
    CT_CHECK(conv.UsingVideoProcessorFallback());
    std::vector<uint8_t> gpu;
    CT_REQUIRE(RunConvert(dev, conv, src.Get(), w, h, gpu));
    std::vector<uint8_t> ref(static_cast<size_t>(w) * h * 3 / 2);
    RgbToNv12Ref(src_px.data(), w, h, ref.data(), ref.data() + static_cast<size_t>(w) * h);
    const Diff d = CompareNv12(gpu, ref, w, h, 10000);
    std::printf("  video processor: max |dY| %d, max |dUV| %d\n", d.max_y, d.max_uv);
    // The driver picks its own chroma siting and filter, so this compares on a smooth image.
    CT_CHECK(d.max_y <= 2);
    CT_CHECK(d.max_uv <= 4);
}

CT_TEST_DEVICE(gpu_convert_hdr_fp16_tonemap) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    if (!dev.SupportsNv12RenderTarget()) CT_SKIP("no NV12 render target support");
    // Four 64x64 quadrants of scRGB grey: 0, 0.5 linear (sRGB 0.735), SDR white (2.5 at 200 nits), 4x SDR white.
    const int w = 128, h = 128;
    const float white_nits = 200.f, sdr = white_nits / 80.f;
    const float level[4] = {0.f, 0.5f * sdr, 1.f * sdr, 4.f * sdr};
    std::vector<uint16_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = level[(y / 64) * 2 + (x / 64)];
            const uint16_t hv = DirectX::PackedVector::XMConvertFloatToHalf(v);
            uint16_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = p[1] = p[2] = hv;
            p[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
        }
    ComPtr<ID3D11Texture2D> src = MakeTexture(dev, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, px.data(), w * 8);
    CT_REQUIRE(src != nullptr);
    for (int tm = 0; tm < 4; ++tm) {
        ColorConverter conv;
        ColorConverter::Config cfg;
        cfg.allow_compute = tm >= 2;
        cfg.src_size = SizeI{w, h};
        cfg.src_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cfg.dst_size = SizeI{w, h};
        cfg.hdr_tonemap = (tm & 1) != 0;
        cfg.sdr_white_nits = white_nits;
        CT_REQUIRE(SUCCEEDED(conv.Init(dev, cfg)));
        std::vector<uint8_t> gpu;
        CT_REQUIRE(RunConvert(dev, conv, src.Get(), w, h, gpu));
        auto y_at = [&](int x, int y) { return static_cast<int>(gpu[static_cast<size_t>(y) * w + x]); };
        CT_CHECK_NEAR(y_at(32, 32), 16, 1);     // black
        CT_CHECK_NEAR(y_at(96, 32), 177, 2);    // 0.5 linear -> sRGB 0.735 -> 16 + 219 * 0.735
        CT_CHECK_NEAR(y_at(32, 96), 235, 1);    // SDR white
        CT_CHECK_NEAR(y_at(96, 96), 235, 1);    // above white: clipped, or rolled off to white
        const size_t c = static_cast<size_t>(w) * h + static_cast<size_t>(16) * w + 2 * 16;
        CT_CHECK_NEAR(gpu[c], 128, 1);
        CT_CHECK_NEAR(gpu[c + 1], 128, 1);
    }
}

CT_TEST_DEVICE(gpu_convert_region_of_larger_source) {
    // ConvertRegion (what the zero-copy capture path uses) equals cropping on the CPU and converting the crop.
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    if (!dev.SupportsNv12RenderTarget()) CT_SKIP("no NV12 render target support");
    const int sw = 800, sh = 600, rx = 136, ry = 90, rw = 320, rh = 240;
    const std::vector<uint8_t> src_px = Gradient(sw, sh);
    ComPtr<ID3D11Texture2D> src = MakeTexture(dev, sw, sh, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, src_px.data(), sw * 4);
    std::vector<uint8_t> crop(static_cast<size_t>(rw) * rh * 4);
    for (int y = 0; y < rh; ++y)
        std::memcpy(&crop[static_cast<size_t>(y) * rw * 4], &src_px[(static_cast<size_t>(y + ry) * sw + rx) * 4], static_cast<size_t>(rw) * 4);
    std::vector<uint8_t> ref(static_cast<size_t>(rw) * rh * 3 / 2);
    RgbToNv12Ref(crop.data(), rw, rh, ref.data(), ref.data() + static_cast<size_t>(rw) * rh);
    for (int mode = 0; mode < 3; ++mode) {   // compute, pixel shaders, video processor
        ColorConverter conv;
        ColorConverter::Config cfg;
        cfg.src_size = SizeI{rw, rh};
        cfg.dst_size = SizeI{rw, rh};
        cfg.allow_compute = mode == 0;
        cfg.force_video_processor = mode == 2;
        if (mode == 2 && !dev.VideoDevice()) continue;
        CT_REQUIRE(SUCCEEDED(conv.Init(dev, cfg)));
        ComPtr<ID3D11Texture2D> dst = MakeTexture(dev, rw, rh, DXGI_FORMAT_NV12, conv.DstBindFlags(), nullptr, 0);
        const D3D11_BOX box{static_cast<UINT>(rx), static_cast<UINT>(ry), 0, static_cast<UINT>(rx + rw), static_cast<UINT>(ry + rh), 1};
        {
            D3DDevice::Lock lock(dev);
            CT_REQUIRE(SUCCEEDED(conv.ConvertRegion(dev.Context(), src.Get(), box, dst.Get())));
            const D3D11_BOX wrong{0, 0, 0, 10, 10, 1};
            CT_CHECK(conv.ConvertRegion(dev.Context(), src.Get(), wrong, dst.Get()) == E_INVALIDARG);
        }
        std::vector<uint8_t> gpu;
        UINT w = 0, h = 0;
        CT_REQUIRE(test_util::ReadbackTexture(dev, dst.Get(), gpu, w, h));
        const Diff d = CompareNv12(gpu, ref, rw, rh, 10000);
        std::printf("  region, %s: max |dY| %d, max |dUV| %d\n", mode == 0 ? "compute" : (mode == 1 ? "pixel shaders" : "video processor"),
                    d.max_y, d.max_uv);
        CT_CHECK(d.max_y <= 2);
        if (mode < 2) CT_CHECK(d.max_uv <= 2);   // the video processor's chroma filter differs on the sawtooth (see above)
    }
}
