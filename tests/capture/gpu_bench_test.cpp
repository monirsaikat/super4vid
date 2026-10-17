// GPU cost of the capture work at 1080p, paced at 60 Hz (like capture) and back to back. Runs with --bench.
#include <ct/capture/color_convert.h>
#include <ct/core/time.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/gpu_timer.h>
#include <ct/test/ct_test.h>

#include <cstdio>
#include <functional>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;
using Microsoft::WRL::ComPtr;

namespace {
ComPtr<ID3D11Texture2D> Tex(D3DDevice& dev, int w, int h, DXGI_FORMAT f) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (f == DXGI_FORMAT_NV12) d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> t;
    dev.Device()->CreateTexture2D(&d, nullptr, t.GetAddressOf());
    return t;
}

void Measure(D3DDevice& dev, const char* name, bool paced, const std::function<void(ID3D11DeviceContext*)>& work) {
    platform::GpuTimer timer;
    timer.Init(dev);
    double sum = 0;
    int count = 0;
    Stopwatch sw;
    sw.Start();
    for (int i = 0; i < 240; ++i) {
        {
            D3DDevice::Lock lock(dev);
            ID3D11DeviceContext* ctx = dev.Context();
            timer.Begin(ctx);
            work(ctx);
            timer.End(ctx);
            ctx->Flush();
            double ms = 0;
            if (i > 30 && timer.Resolve(ctx, ms)) {
                sum += ms;
                ++count;
            }
        }
        if (paced) Sleep(16);
    }
    std::printf("  %-34s %s: GPU avg %.3f ms over %d frames\n", name, paced ? "paced 60 Hz" : "back to back", count ? sum / count : -1.0, count);
}
} // namespace

CT_BENCH(capture_gpu_costs_1080p, 1) {
    (void)CT_BENCH_N;
    D3DDevice dev;
    if (FAILED(dev.Init(false))) return;
    const int w = 1920, h = 1080;
    auto src = Tex(dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
    auto bgra = Tex(dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
    auto nv12 = Tex(dev, w, h, DXGI_FORMAT_NV12);
    auto nv12b = Tex(dev, w, h, DXGI_FORMAT_NV12);
    ColorConverter conv, ps, vp;
    ColorConverter::Config cfg;
    cfg.src_size = cfg.dst_size = SizeI{w, h};
    conv.Init(dev, cfg);
    cfg.allow_compute = false;
    ps.Init(dev, cfg);
    cfg.force_video_processor = true;
    vp.Init(dev, cfg);
    for (int paced = 1; paced >= 0; --paced) {
        Measure(dev, "copy BGRA 1080p", paced != 0, [&](ID3D11DeviceContext* ctx) { ctx->CopyResource(bgra.Get(), src.Get()); });
        Measure(dev, "convert compute", paced != 0, [&](ID3D11DeviceContext* ctx) { conv.Convert(ctx, bgra.Get(), nv12.Get()); });
        Measure(dev, "convert pixel shaders", paced != 0, [&](ID3D11DeviceContext* ctx) { ps.Convert(ctx, bgra.Get(), nv12.Get()); });
        Measure(dev, "convert video processor", paced != 0, [&](ID3D11DeviceContext* ctx) { vp.Convert(ctx, bgra.Get(), nv12.Get()); });
        Measure(dev, "copy NV12 1080p", paced != 0, [&](ID3D11DeviceContext* ctx) { ctx->CopyResource(nv12b.Get(), nv12.Get()); });
    }
}

CT_BENCH(capture_nv12_uav_probe, 1) {
    (void)CT_BENCH_N;
    D3DDevice dev;
    if (FAILED(dev.Init(false))) return;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs2{DXGI_FORMAT_NV12, 0};
    dev.Device()->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs2, sizeof(fs2));
    UINT fs = 0;
    dev.Device()->CheckFormatSupport(DXGI_FORMAT_NV12, &fs);
    D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
    dev.Device()->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
    std::printf("  NV12 support 0x%08x support2 0x%08x (UAV typed store %s), typed UAV load additional formats n/a, VPAndRTArrayIndex %d\n", fs,
                fs2.OutFormatSupport2, (fs2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) ? "yes" : "no",
                o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
    std::printf("  TYPED_UNORDERED_ACCESS_VIEW in support1: %s\n", (fs & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) ? "yes" : "no");
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 1920;
    d.Height = 1080;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> t;
    HRESULT hr = dev.Device()->CreateTexture2D(&d, nullptr, t.GetAddressOf());
    std::printf("  NV12 with UAV bind: 0x%08lx\n", static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11UnorderedAccessView1> uav;
        D3D11_UNORDERED_ACCESS_VIEW_DESC1 ud{};
        ud.Format = DXGI_FORMAT_R8_UNORM;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.PlaneSlice = 0;
        hr = dev.Device()->CreateUnorderedAccessView1(t.Get(), &ud, uav.GetAddressOf());
        std::printf("  R8 UAV plane 0: 0x%08lx\n", static_cast<unsigned long>(hr));
        ud.Format = DXGI_FORMAT_R8G8_UNORM;
        ud.Texture2D.PlaneSlice = 1;
        ComPtr<ID3D11UnorderedAccessView1> uav2;
        hr = dev.Device()->CreateUnorderedAccessView1(t.Get(), &ud, uav2.GetAddressOf());
        std::printf("  R8G8 UAV plane 1: 0x%08lx\n", static_cast<unsigned long>(hr));
    }
}
