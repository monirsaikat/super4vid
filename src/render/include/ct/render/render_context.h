#pragma once
// ct/render/render_context.h
// Shared GPU state of the compositor: one per D3DDevice. Every shader, state object, sampler and constant buffer is
// created once in Init; temporary render targets are pooled per size and format. Also owns the Direct2D device context
// and DirectWrite factory used for text (D2D device created from the DXGI device; needs BGRA_SUPPORT).
// Thread model: one thread renders; the caller holds D3DDevice::Lock for the whole RenderFrame.
// Lifetime: Init() -> frames -> Shutdown() (or destructor), before the D3DDevice shuts down.

#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/gpu_timer.h>

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

namespace ct::render {

struct Pipelines;   // internal (src/render/src/render_internal.h)

class RenderContext {
public:
    RenderContext();
    ~RenderContext();
    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    HRESULT Init(platform::D3DDevice&);
    void Shutdown();
    bool IsInitialized() const { return m_dev != nullptr; }

    platform::D3DDevice& Device() { return *m_dev; }
    ID3D11DeviceContext4* Ctx() { return m_dev->Context(); }   // caller holds D3DDevice::Lock for the whole RenderFrame
    ID2D1DeviceContext* D2D() { return m_d2d_ctx.Get(); }
    ID2D1Factory1* D2DFactory() { return m_d2d_factory.Get(); }
    IDWriteFactory3* DWrite() { return m_dwrite.Get(); }

    struct TempRT {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        SizeI size;
    };
    // Exactly the requested size. Pooled per (size, format); BeginFrame() returns every temp to the pool.
    TempRT* AcquireTemp(SizeI, DXGI_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM);
    void ReleaseTemp(TempRT*);
    void BeginFrame();                 // releases all temps (call at the start of a frame)
    size_t TempCount() const { return m_temps.size(); }

    // Pipeline helpers used by all passes.
    // Binds `rtv` as the only render target (unbinding pixel-shader SRVs first), sets the viewport, the fullscreen VS,
    // triangle-list topology and the default rasterizer state.
    void SetFullscreen(ID3D11RenderTargetView*, SizeI viewport);
    void DrawFullscreen();   // 3-vertex triangle (SV_VertexID)
    void SetBlendPremul();   // ONE, INV_SRC_ALPHA (colour and alpha)
    void SetBlendOpaque();   // blending off
    ID3D11SamplerState* Linear();   // clamp
    ID3D11SamplerState* Point();    // clamp
    void UnbindSrvs();       // PS slots 0..3
    platform::GpuTimer& Timer(int slot);   // slots 0..kTimerSlots-1
    static constexpr int kTimerSlots = 4;

    Pipelines& P() { return *m_p; }

private:
    struct TempEntry {
        TempRT rt;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool in_use = false;
    };

    platform::D3DDevice* m_dev = nullptr;
    std::unique_ptr<Pipelines> m_p;
    std::vector<std::unique_ptr<TempEntry>> m_temps;
    platform::GpuTimer m_timers[kTimerSlots];
    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2d_factory;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2d_device;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2d_ctx;
    Microsoft::WRL::ComPtr<IDWriteFactory3> m_dwrite;
};

} // namespace ct::render
