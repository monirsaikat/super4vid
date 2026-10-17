// ct/platform/{d3d_device,texture_pool,gpu_timer}.h
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/gpu_timer.h>
#include <ct/platform/texture_pool.h>
#include <ct/test/ct_test.h>

#include <atomic>
#include <cstdio>
#include <cstring>

using ct::platform::D3DDevice;
using ct::platform::GpuTimer;
using ct::platform::TexturePool;

namespace {

D3D11_TEXTURE2D_DESC Nv12Desc(UINT w, UINT h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return d;
}

bool LuidEqual(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

} // namespace

CT_TEST_DEVICE(d3d_device_create_with_and_without_debug) {
    for (int debug = 0; debug < 2; ++debug) {
        D3DDevice dev;
        CT_REQUIRE(SUCCEEDED(dev.Init(debug != 0)));
        CT_CHECK(dev.Device() != nullptr);
        CT_CHECK(dev.Context() != nullptr);
        CT_CHECK(dev.Adapter() != nullptr);
        DXGI_ADAPTER_DESC3 desc{};
        CT_CHECK_HR(dev.Adapter()->GetDesc3(&desc));
        CT_CHECK(LuidEqual(desc.AdapterLuid, dev.AdapterLuid()));
        CT_CHECK_HR(dev.CheckRemoved());
        if (ct::test::Verbose() || debug == 0)
            std::printf("  adapter: %s%s, NV12 RT %s, FP16 RT %s, video device %s, debug layer %s\n",
                        dev.AdapterName().c_str(), dev.IsWarp() ? " (WARP)" : "",
                        dev.SupportsNv12RenderTarget() ? "yes" : "no", dev.SupportsFp16RenderTarget() ? "yes" : "no",
                        dev.VideoDevice() ? "yes" : "no", dev.DebugLayerActive() ? "yes" : "no");
        if (!dev.IsWarp()) {
            CT_CHECK(dev.VideoDevice() != nullptr);
            CT_CHECK(dev.VideoContext() != nullptr);
        }
        dev.Shutdown();
        dev.ReportLiveObjects();
    }
}

CT_TEST_DEVICE(d3d_device_primary_monitor_adapter) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    if (dev.IsWarp()) CT_SKIP("WARP device");
    // The chosen adapter must own an output that contains (0,0).
    bool found = false;
    Microsoft::WRL::ComPtr<IDXGIOutput> out;
    for (UINT o = 0; dev.Adapter()->EnumOutputs(o, out.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++o) {
        DXGI_OUTPUT_DESC d{};
        if (SUCCEEDED(out->GetDesc(&d)) && d.AttachedToDesktop && d.DesktopCoordinates.left <= 0 &&
            d.DesktopCoordinates.top <= 0 && d.DesktopCoordinates.right > 0 && d.DesktopCoordinates.bottom > 0)
            found = true;
    }
    CT_CHECK(found);
}

CT_TEST_DEVICE(d3d_device_mf_manager_twice) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    CT_CHECK(dev.MfManager() == nullptr);
    CT_CHECK_HR(dev.InitMfManager());
    IMFDXGIDeviceManager* first = dev.MfManager();
    const UINT token = dev.MfToken();
    CT_CHECK(first != nullptr);
    CT_CHECK_HR(dev.InitMfManager());
    CT_CHECK(dev.MfManager() == first);
    CT_CHECK_EQ(dev.MfToken(), token);
    // The manager hands back our device.
    HANDLE h = nullptr;
    CT_REQUIRE(SUCCEEDED(first->OpenDeviceHandle(&h)));
    Microsoft::WRL::ComPtr<ID3D11Device> got;
    CT_CHECK_HR(first->GetVideoService(h, IID_PPV_ARGS(got.GetAddressOf())));
    Microsoft::WRL::ComPtr<ID3D11Device> mine;
    dev.Device()->QueryInterface(IID_PPV_ARGS(mine.GetAddressOf()));
    CT_CHECK(got.Get() == mine.Get());
    first->CloseDeviceHandle(h);
}

namespace {
struct PredicateState {
    bool allow[4] = {true, true, true, true};
    int calls = 0;
};
bool CanReuse(void* user, uint32_t index) {
    auto* s = static_cast<PredicateState*>(user);
    ++s->calls;
    return s->allow[index];
}
} // namespace

CT_TEST_DEVICE(texture_pool_acquire_release_predicate) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    if (!dev.SupportsNv12RenderTarget()) CT_SKIP("no NV12 render target support");
    TexturePool pool;
    CT_REQUIRE(SUCCEEDED(pool.Init(dev, Nv12Desc(256, 128), 4, "test.nv12")));
    CT_CHECK_EQ(pool.Count(), 4u);
    CT_CHECK_EQ(pool.InUse(), 0u);
    CT_CHECK_EQ(pool.Desc().Width, 256u);

    char name[64] = {};
    UINT size = sizeof(name) - 1;
    CT_CHECK_HR(pool.At(2)->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name));
    CT_CHECK(std::strcmp(name, "test.nv12[2]") == 0);

    uint32_t idx[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
        CT_REQUIRE(pool.Acquire(idx[i]) != nullptr);
        CT_CHECK_EQ(idx[i], i);
    }
    uint32_t extra = 99;
    CT_CHECK(pool.Acquire(extra) == nullptr);   // exhausted
    CT_CHECK_EQ(pool.InUse(), 4u);
    pool.Release(idx[1]);
    CT_CHECK_EQ(pool.InUse(), 3u);
    uint32_t again = 99;
    CT_CHECK(pool.Acquire(again) == pool.At(1));
    CT_CHECK_EQ(again, 1u);
    for (uint32_t i = 0; i < 4; ++i) pool.Release(i);
    CT_CHECK_EQ(pool.InUse(), 0u);

    PredicateState st;
    st.allow[0] = st.allow[1] = st.allow[2] = false;
    pool.SetReusePredicate(&CanReuse, &st);
    uint32_t got = 99;
    CT_CHECK(pool.Acquire(got) != nullptr);
    CT_CHECK_EQ(got, 3u);
    CT_CHECK(pool.Acquire(got) == nullptr);   // 3 is in use, the rest are refused
    CT_CHECK(st.calls >= 4);
    pool.Release(3);

    int tag = 7;
    pool.SetUserData(2, &tag);
    CT_CHECK(pool.UserData(2) == &tag);
    CT_CHECK(pool.UserData(1) == nullptr);
    pool.Shutdown();
    CT_CHECK_EQ(pool.Count(), 0u);
}

CT_TEST_DEVICE(texture_pool_cross_thread_release) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    D3D11_TEXTURE2D_DESC d = Nv12Desc(64, 64);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    TexturePool pool;
    CT_REQUIRE(SUCCEEDED(pool.Init(dev, d, 3, "test.bgra")));
    // Producer here, consumer on a ct::Thread via a one-slot handoff; every acquired index comes back.
    std::atomic<int> handoff{-1};
    std::atomic<bool> done{false};
    ct::Thread consumer;
    CT_REQUIRE(consumer.Start({}, [&] {
        while (!done.load()) {
            const int v = handoff.exchange(-1);
            if (v >= 0) pool.Release(static_cast<uint32_t>(v));
            else YieldProcessor();
        }
    }));
    int acquired = 0;
    const int64_t deadline = ct::Clock::QpcNow() + ct::Clock::QpcFrequency() * 5;
    while (acquired < 10000 && ct::Clock::QpcNow() < deadline) {
        uint32_t i = 0;
        if (!pool.Acquire(i)) continue;
        ++acquired;
        int expected = -1;
        while (!handoff.compare_exchange_weak(expected, static_cast<int>(i))) expected = -1;
    }
    while (handoff.load() >= 0) YieldProcessor();
    done.store(true);
    CT_CHECK(consumer.Join(5000));
    CT_CHECK_EQ(acquired, 10000);
    CT_CHECK_EQ(pool.InUse(), 0u);
}

CT_TEST_DEVICE(gpu_timer_measures_clear) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 3840;
    d.Height = 2160;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    CT_REQUIRE(SUCCEEDED(dev.Device()->CreateTexture2D(&d, nullptr, tex.GetAddressOf())));
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    CT_REQUIRE(SUCCEEDED(dev.Device()->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf())));
    GpuTimer timer;
    CT_REQUIRE(SUCCEEDED(timer.Init(dev)));
    int resolved = 0;
    double last_ms = -1;
    {
        D3DDevice::Lock lock(dev);
        ID3D11DeviceContext4* ctx = dev.Context();
        for (int frame = 0; frame < 200 && resolved < 3; ++frame) {
            timer.Begin(ctx);
            const float c[4] = {frame / 200.f, 0.f, 0.f, 1.f};
            for (int k = 0; k < 4; ++k) ctx->ClearRenderTargetView(rtv.Get(), c);
            timer.End(ctx);
            ctx->Flush();
            double ms = 0;
            if (timer.Resolve(ctx, ms)) {
                ++resolved;
                last_ms = ms;
            }
            Sleep(2);
        }
    }
    CT_CHECK(resolved >= 1);
    CT_CHECK(last_ms >= 0.0 && last_ms < 100.0);
    if (ct::test::Verbose()) std::printf("  4 x 4K clears: %.3f ms GPU\n", last_ms);
}
