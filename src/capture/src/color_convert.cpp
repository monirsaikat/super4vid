#define CT_LOG_CATEGORY "capture"
#include <ct/capture/color_convert.h>

#include <ct/core/error.h>
#include <ct/core/log.h>

#include "bgra_to_nv12_ps_chroma.h"
#include "bgra_to_nv12_ps_luma.h"
#include "fullscreen_vs_main.h"
#include "rgba16f_to_nv12_ps_chroma.h"
#include "rgba16f_to_nv12_ps_luma.h"

#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::capture {

namespace {

struct alignas(16) ConvertCb {
    float inv_src_size[2];
    float pad[2];
    float rot_uv[16];
    float hdr_scale;
    float tonemap;
    float pad2[2];
};
static_assert(sizeof(ConvertCb) == 96, "must match cbuffer CB in bgra_to_nv12.hlsl");

constexpr int kViewCache = 16;

template <class View>
struct CachedView {
    ID3D11Texture2D* tex = nullptr;   // the view holds a reference, so the address cannot be reused meanwhile
    ComPtr<View> a, b;
};

template <class View, class CreateFn>
HRESULT FindOrCreate(CachedView<View> (&cache)[kViewCache], int& next, ID3D11Texture2D* tex, CachedView<View>*& out,
                     CreateFn create) {
    for (auto& c : cache)
        if (c.tex == tex) {
            out = &c;
            return S_OK;
        }
    CachedView<View>& slot = cache[next];
    next = (next + 1) % kViewCache;
    slot.tex = nullptr;
    slot.a.Reset();
    slot.b.Reset();
    CT_RETURN_IF_FAILED(create(slot));
    slot.tex = tex;
    out = &slot;
    return S_OK;
}

D3D11_VIDEO_PROCESSOR_ROTATION VpRotation(Rotation r) {
    switch (r) {
    case Rotation::R90: return D3D11_VIDEO_PROCESSOR_ROTATION_90;     // clockwise: output (u,v) <- source (v, 1-u)
    case Rotation::R180: return D3D11_VIDEO_PROCESSOR_ROTATION_180;
    case Rotation::R270: return D3D11_VIDEO_PROCESSOR_ROTATION_270;
    default: return D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    }
}

uint8_t ToByte(float v) {
    const float x = v * 255.f + 0.5f;
    return static_cast<uint8_t>(x < 0.f ? 0.f : (x > 255.f ? 255.f : x));
}

} // namespace

struct ColorConverter::Impl {
    platform::D3DDevice* dev = nullptr;
    Config cfg;
    // Shader path
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps_luma, ps_chroma;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> raster;
    CachedView<ID3D11ShaderResourceView> srvs[kViewCache];
    CachedView<ID3D11RenderTargetView> rtvs[kViewCache];
    int srv_next = 0, rtv_next = 0;
    // Video processor path
    ComPtr<ID3D11VideoProcessorEnumerator> vp_enum;
    ComPtr<ID3D11VideoProcessor> vp;
    CachedView<ID3D11VideoProcessorInputView> vp_in[kViewCache];
    CachedView<ID3D11VideoProcessorOutputView> vp_out[kViewCache];
    int vp_in_next = 0, vp_out_next = 0;

    HRESULT InitShaders();
    HRESULT InitVideoProcessor();
    HRESULT ConvertShaders(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst);
    HRESULT ConvertVideoProcessor(ID3D11Texture2D* src, ID3D11Texture2D* dst);
};

ColorConverter::~ColorConverter() { Shutdown(); }

HRESULT ColorConverter::Init(platform::D3DDevice& dev, const Config& cfg) {
    Shutdown();
    if (!dev.Device()) return E_UNEXPECTED;
    if (cfg.dst_size.IsEmpty() || (cfg.dst_size.w & 1) || (cfg.dst_size.h & 1) || cfg.src_size.IsEmpty()) return E_INVALIDARG;
    if (cfg.src_format != DXGI_FORMAT_B8G8R8A8_UNORM && cfg.src_format != DXGI_FORMAT_R16G16B16A16_FLOAT) return E_INVALIDARG;
    m_impl = new Impl;
    m_impl->dev = &dev;
    m_impl->cfg = cfg;
    m_use_vp = cfg.force_video_processor || !dev.SupportsNv12RenderTarget();
    const HRESULT hr = m_use_vp ? m_impl->InitVideoProcessor() : m_impl->InitShaders();
    if (FAILED(hr)) {
        Shutdown();
        return hr;
    }
    CT_LOG_INFO("ColorConverter: %dx%d %s -> NV12 %dx%d, rotation %s, %s%s", cfg.src_size.w, cfg.src_size.h,
                cfg.src_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? "RGBA16F" : "BGRA8", cfg.dst_size.w, cfg.dst_size.h,
                RotationName(cfg.rotation), m_use_vp ? "video processor" : "shaders",
                cfg.src_format == DXGI_FORMAT_R16G16B16A16_FLOAT && !m_use_vp && cfg.hdr_tonemap ? ", HDR tone-map" : "");
    return S_OK;
}

HRESULT ColorConverter::Impl::InitShaders() {
    ID3D11Device* d = dev->Device();
    const bool hdr = cfg.src_format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    CT_RETURN_IF_FAILED(d->CreateVertexShader(g_fullscreen_vs_main, sizeof(g_fullscreen_vs_main), nullptr, vs.GetAddressOf()));
    if (hdr) {
        CT_RETURN_IF_FAILED(d->CreatePixelShader(g_rgba16f_to_nv12_ps_luma, sizeof(g_rgba16f_to_nv12_ps_luma), nullptr, ps_luma.GetAddressOf()));
        CT_RETURN_IF_FAILED(d->CreatePixelShader(g_rgba16f_to_nv12_ps_chroma, sizeof(g_rgba16f_to_nv12_ps_chroma), nullptr, ps_chroma.GetAddressOf()));
    } else {
        CT_RETURN_IF_FAILED(d->CreatePixelShader(g_bgra_to_nv12_ps_luma, sizeof(g_bgra_to_nv12_ps_luma), nullptr, ps_luma.GetAddressOf()));
        CT_RETURN_IF_FAILED(d->CreatePixelShader(g_bgra_to_nv12_ps_chroma, sizeof(g_bgra_to_nv12_ps_chroma), nullptr, ps_chroma.GetAddressOf()));
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    CT_RETURN_IF_FAILED(d->CreateSamplerState(&sd, sampler.GetAddressOf()));

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    CT_RETURN_IF_FAILED(d->CreateRasterizerState(&rd, raster.GetAddressOf()));

    ConvertCb data{};
    data.inv_src_size[0] = 1.f / static_cast<float>(cfg.src_size.w);
    data.inv_src_size[1] = 1.f / static_cast<float>(cfg.src_size.h);
    RotationUvMatrix(cfg.rotation, data.rot_uv);
    data.hdr_scale = 80.f / (cfg.sdr_white_nits > 1.f ? cfg.sdr_white_nits : 80.f);
    data.tonemap = cfg.hdr_tonemap ? 1.f : 0.f;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ConvertCb);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&data, 0, 0};
    CT_RETURN_IF_FAILED(d->CreateBuffer(&bd, &init, cb.GetAddressOf()));
    return S_OK;
}

HRESULT ColorConverter::Impl::InitVideoProcessor() {
    ID3D11VideoDevice* vd = dev->VideoDevice();
    if (!vd || !dev->VideoContext()) {
        CT_LOG_ERROR("ColorConverter: no NV12 render target and no video device; cannot convert");
        return E_NOTIMPL;
    }
    if (cfg.src_format == DXGI_FORMAT_R16G16B16A16_FLOAT) CT_LOG_WARN("ColorConverter: video processor fallback has no HDR tone-map");
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = DXGI_RATIONAL{60, 1};
    cd.OutputFrameRate = DXGI_RATIONAL{60, 1};
    cd.InputWidth = static_cast<UINT>(cfg.src_size.w);
    cd.InputHeight = static_cast<UINT>(cfg.src_size.h);
    cd.OutputWidth = static_cast<UINT>(cfg.dst_size.w);
    cd.OutputHeight = static_cast<UINT>(cfg.dst_size.h);
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    CT_RETURN_IF_FAILED(vd->CreateVideoProcessorEnumerator(&cd, vp_enum.GetAddressOf()));
    UINT in_flags = 0, out_flags = 0;
    CT_RETURN_IF_FAILED(vp_enum->CheckVideoProcessorFormat(cfg.src_format, &in_flags));
    CT_RETURN_IF_FAILED(vp_enum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &out_flags));
    if (!(in_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(out_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        CT_LOG_ERROR("ColorConverter: video processor cannot convert format %d to NV12", static_cast<int>(cfg.src_format));
        return E_NOTIMPL;
    }
    CT_RETURN_IF_FAILED(vd->CreateVideoProcessor(vp_enum.Get(), 0, vp.GetAddressOf()));

    ID3D11VideoContext* vc = dev->VideoContext();
    platform::D3DDevice::Lock lock(*dev);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.Usage = 0;          // playback
    in_cs.RGB_Range = 0;      // full 0-255
    in_cs.YCbCr_Matrix = 1;   // BT.709
    in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    vc->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in_cs);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.Usage = 0;
    out_cs.RGB_Range = 1;
    out_cs.YCbCr_Matrix = 1;  // BT.709
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vc->VideoProcessorSetOutputColorSpace(vp.Get(), &out_cs);
    vc->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    vc->VideoProcessorSetStreamRotation(vp.Get(), 0, cfg.rotation != Rotation::Identity, VpRotation(cfg.rotation));
    const RECT src_rect{0, 0, cfg.src_size.w, cfg.src_size.h};
    const RECT dst_rect{0, 0, cfg.dst_size.w, cfg.dst_size.h};
    vc->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src_rect);
    vc->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst_rect);
    vc->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dst_rect);
    return S_OK;
}

HRESULT ColorConverter::Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst_nv12) {
    if (!m_impl || !src || !dst_nv12) return E_UNEXPECTED;
    return m_use_vp ? m_impl->ConvertVideoProcessor(src, dst_nv12) : m_impl->ConvertShaders(ctx, src, dst_nv12);
}

HRESULT ColorConverter::Impl::ConvertShaders(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst) {
    ID3D11Device* d = dev->Device();
    CachedView<ID3D11ShaderResourceView>* sv = nullptr;
    CT_RETURN_IF_FAILED(FindOrCreate(srvs, srv_next, src, sv, [&](CachedView<ID3D11ShaderResourceView>& c) {
        return d->CreateShaderResourceView(src, nullptr, c.a.GetAddressOf());
    }));
    CachedView<ID3D11RenderTargetView>* rv = nullptr;
    CT_RETURN_IF_FAILED(FindOrCreate(rtvs, rtv_next, dst, rv, [&](CachedView<ID3D11RenderTargetView>& c) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = 0;
        rd.Format = DXGI_FORMAT_R8_UNORM;     // plane 0
        HRESULT hr = d->CreateRenderTargetView(dst, &rd, c.a.GetAddressOf());
        if (FAILED(hr)) return hr;
        rd.Format = DXGI_FORMAT_R8G8_UNORM;   // plane 1
        return d->CreateRenderTargetView(dst, &rd, c.b.GetAddressOf());
    }));

    // The context is shared with the UI and P04: set every piece of state this pass depends on.
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(raster.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ID3D11ShaderResourceView* srv = sv->a.Get();
    ID3D11SamplerState* smp = sampler.Get();
    ID3D11Buffer* cbs = cb.Get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->PSSetConstantBuffers(0, 1, &cbs);

    const float w = static_cast<float>(cfg.dst_size.w), h = static_cast<float>(cfg.dst_size.h);
    D3D11_VIEWPORT vp_luma{0.f, 0.f, w, h, 0.f, 1.f};
    ID3D11RenderTargetView* y = rv->a.Get();
    ctx->OMSetRenderTargets(1, &y, nullptr);
    ctx->RSSetViewports(1, &vp_luma);
    ctx->PSSetShader(ps_luma.Get(), nullptr, 0);
    ctx->Draw(3, 0);

    D3D11_VIEWPORT vp_chroma{0.f, 0.f, w * 0.5f, h * 0.5f, 0.f, 1.f};
    ID3D11RenderTargetView* uv = rv->b.Get();
    ctx->OMSetRenderTargets(1, &uv, nullptr);
    ctx->RSSetViewports(1, &vp_chroma);
    ctx->PSSetShader(ps_chroma.Get(), nullptr, 0);
    ctx->Draw(3, 0);

    // Unbind so the next copy into src / encoder read of dst sees no hazard.
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    return S_OK;
}

HRESULT ColorConverter::Impl::ConvertVideoProcessor(ID3D11Texture2D* src, ID3D11Texture2D* dst) {
    ID3D11VideoDevice* vd = dev->VideoDevice();
    CachedView<ID3D11VideoProcessorInputView>* iv = nullptr;
    CT_RETURN_IF_FAILED(FindOrCreate(vp_in, vp_in_next, src, iv, [&](CachedView<ID3D11VideoProcessorInputView>& c) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.FourCC = 0;
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = 0;
        d.Texture2D.ArraySlice = 0;
        return vd->CreateVideoProcessorInputView(src, vp_enum.Get(), &d, c.a.GetAddressOf());
    }));
    CachedView<ID3D11VideoProcessorOutputView>* ov = nullptr;
    CT_RETURN_IF_FAILED(FindOrCreate(vp_out, vp_out_next, dst, ov, [&](CachedView<ID3D11VideoProcessorOutputView>& c) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = 0;
        return vd->CreateVideoProcessorOutputView(dst, vp_enum.Get(), &d, c.a.GetAddressOf());
    }));
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = iv->a.Get();
    return dev->VideoContext()->VideoProcessorBlt(vp.Get(), ov->a.Get(), 0, 1, &stream);
}

void ColorConverter::Shutdown() {
    delete m_impl;
    m_impl = nullptr;
    m_use_vp = false;
}

// ---- CPU references -------------------------------------------------------------------------------------------

void RotationUvMatrix(Rotation rotation, float m[16]) {
    // Row vector [u v 0 1] times M: su = u*m[0] + v*m[4] + m[12], sv = u*m[1] + v*m[5] + m[13].
    std::memset(m, 0, sizeof(float) * 16);
    m[10] = 1.f;
    m[15] = 1.f;
    switch (rotation) {
    case Rotation::R90:   // (v, 1-u)
        m[4] = 1.f;
        m[1] = -1.f;
        m[13] = 1.f;
        break;
    case Rotation::R180:  // (1-u, 1-v)
        m[0] = -1.f;
        m[12] = 1.f;
        m[5] = -1.f;
        m[13] = 1.f;
        break;
    case Rotation::R270:  // (1-v, u)
        m[4] = -1.f;
        m[12] = 1.f;
        m[1] = 1.f;
        break;
    default:
        m[0] = 1.f;
        m[5] = 1.f;
        break;
    }
}

void RotateUvRef(Rotation rotation, float u, float v, float& su, float& sv) {
    switch (rotation) {
    case Rotation::R90: su = v; sv = 1.f - u; break;
    case Rotation::R180: su = 1.f - u; sv = 1.f - v; break;
    case Rotation::R270: su = 1.f - v; sv = u; break;
    default: su = u; sv = v; break;
    }
}

void RgbToNv12Ref(const uint8_t* bgra, int width, int height, uint8_t* y, uint8_t* uv) {
    for (int j = 0; j < height; ++j)
        for (int i = 0; i < width; ++i) {
            const uint8_t* p = bgra + (static_cast<size_t>(j) * width + i) * 4;
            const float b = p[0] / 255.f, g = p[1] / 255.f, r = p[2] / 255.f;
            y[static_cast<size_t>(j) * width + i] = ToByte(0.1826f * r + 0.6142f * g + 0.0620f * b + 16.f / 255.f);
        }
    for (int j = 0; j < height / 2; ++j)
        for (int i = 0; i < width / 2; ++i) {
            float r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = bgra + (static_cast<size_t>(2 * j + dy) * width + 2 * i + dx) * 4;
                    b += p[0];
                    g += p[1];
                    r += p[2];
                }
            r /= 4.f * 255.f;
            g /= 4.f * 255.f;
            b /= 4.f * 255.f;
            uint8_t* o = uv + static_cast<size_t>(j) * width + 2 * i;
            o[0] = ToByte(-0.1006f * r - 0.3386f * g + 0.4392f * b + 128.f / 255.f);
            o[1] = ToByte(0.4392f * r - 0.3989f * g - 0.0403f * b + 128.f / 255.f);
        }
}

} // namespace ct::capture
