#pragma once
// ct/platform/nv12_blit.h
// NV12 (BT.709 limited range) -> RGBA render pass: Y/UV plane SRVs (R8 / R8G8) + a full-screen-triangle pixel shader.
// Used by the webcam preview (P05), the decoder thumbnails and player_demo (P06) and the compositor's video layers (P08).
// Thread model: Init on any thread; ToRgba/Draw on one thread at a time with the immediate context under
// D3DDevice::Lock (or on the thread that owns the context). They change the pipeline state (IA, VS, PS, RS viewport,
// OM render target, PS SRVs/sampler/constant buffer) and unbind the SRVs afterwards; callers set their own state again.
// Lifetime: Init -> ... -> Shutdown (or the destructor).

#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>

#include <wrl/client.h>

#include <cstdint>

namespace ct::platform {

class Nv12Blit {
public:
    Nv12Blit() = default;
    ~Nv12Blit() { Shutdown(); }
    Nv12Blit(const Nv12Blit&) = delete;
    Nv12Blit& operator=(const Nv12Blit&) = delete;

    HRESULT Init(D3DDevice&);
    void Shutdown();

    // The whole NV12 texture stretched over the whole RGBA render target (BIND_RENDER_TARGET, any 8-bit RGBA/BGRA
    // format). Views are cached for the last few textures, so per-frame calls with pool textures create nothing.
    HRESULT ToRgba(ID3D11DeviceContext*, ID3D11Texture2D* nv12, ID3D11Texture2D* rgba_rt);

    // Addition (P06): draws the planes into `viewport` of `rtv` (letterboxing, thumbnails). box_filter averages
    // ceil(scale / 2)^2 bilinear taps per output pixel (quality downscale); src_size is the NV12 size in pixels.
    HRESULT Draw(ID3D11DeviceContext*, ID3D11ShaderResourceView* y, ID3D11ShaderResourceView* uv, SizeI src_size,
                 ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& viewport, bool box_filter, float alpha = 1.0f);

    // Plane views of an NV12 texture: R8_UNORM (Y) and R8G8_UNORM (UV).
    static HRESULT CreatePlaneViews(ID3D11Device*, ID3D11Texture2D* nv12, ID3D11ShaderResourceView** y, ID3D11ShaderResourceView** uv);

private:
    struct CachedViews {
        ID3D11Resource* key = nullptr;   // raw pointer of the texture the views belong to (kept alive by `holder`)
        Microsoft::WRL::ComPtr<ID3D11Resource> holder;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y, uv;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        uint64_t last_use = 0;
    };
    static constexpr int kCache = 8;
    CachedViews* Lookup(ID3D11Resource* res);

    D3DDevice* m_dev = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps, m_ps_box;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rs;
    CachedViews m_cache[kCache];
    uint64_t m_use = 0;
};

} // namespace ct::platform
