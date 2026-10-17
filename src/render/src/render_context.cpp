#define CT_LOG_CATEGORY "render"
#include <ct/render/render_context.h>

#include <ct/core/assert.h>
#include <ct/core/error.h>
#include <ct/core/log.h>

#include "render_internal.h"

#include "blur_ps_blur.h"
#include "blur_ps_copy.h"
#include "composite_ps_multiply.h"
#include "composite_ps_shadow.h"
#include "cursor_ps_disc.h"
#include "cursor_ps_ring.h"
#include "cursor_ps_shadow.h"
#include "cursor_ps_spotlight.h"
#include "fullscreen_vs_main.h"
#include "fullscreen_vs_quad.h"
#include "gradient_ps_main.h"
#include "layer_ps_nv12.h"
#include "layer_ps_rgba.h"
#include "layer_ps_solid.h"
#include "pixelate_ps_main.h"
#include "shadow_mask_ps_main.h"
#include "transition_ps_main.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::render {

namespace {

struct PsBlob {
    Ps id;
    const BYTE* code;
    size_t size;
};

} // namespace

RenderContext::RenderContext() = default;
RenderContext::~RenderContext() { Shutdown(); }

HRESULT RenderContext::Init(platform::D3DDevice& dev) {
    Shutdown();
    if (!dev.Device()) return E_UNEXPECTED;
    ID3D11Device* d = dev.Device();
    // Direct2D on the shared device needs BGRA support (D3DDevice::Init sets it).
    const bool bgra = (d->GetCreationFlags() & D3D11_CREATE_DEVICE_BGRA_SUPPORT) != 0;
    CT_ASSERT(bgra);
    if (!bgra) {
        CT_LOG_ERROR("RenderContext: the D3D11 device was created without D3D11_CREATE_DEVICE_BGRA_SUPPORT");
        return E_INVALIDARG;
    }

    auto p = std::make_unique<Pipelines>();
    CT_RETURN_IF_FAILED(d->CreateVertexShader(g_fullscreen_vs_main, sizeof(g_fullscreen_vs_main), nullptr, p->vs_full.GetAddressOf()));
    CT_RETURN_IF_FAILED(d->CreateVertexShader(g_fullscreen_vs_quad, sizeof(g_fullscreen_vs_quad), nullptr, p->vs_quad.GetAddressOf()));
    const PsBlob blobs[] = {
        {Ps::Nv12, g_layer_ps_nv12, sizeof(g_layer_ps_nv12)},
        {Ps::Rgba, g_layer_ps_rgba, sizeof(g_layer_ps_rgba)},
        {Ps::Solid, g_layer_ps_solid, sizeof(g_layer_ps_solid)},
        {Ps::Gradient, g_gradient_ps_main, sizeof(g_gradient_ps_main)},
        {Ps::Blur, g_blur_ps_blur, sizeof(g_blur_ps_blur)},
        {Ps::Copy, g_blur_ps_copy, sizeof(g_blur_ps_copy)},
        {Ps::Pixelate, g_pixelate_ps_main, sizeof(g_pixelate_ps_main)},
        {Ps::Ring, g_cursor_ps_ring, sizeof(g_cursor_ps_ring)},
        {Ps::Disc, g_cursor_ps_disc, sizeof(g_cursor_ps_disc)},
        {Ps::Spotlight, g_cursor_ps_spotlight, sizeof(g_cursor_ps_spotlight)},
        {Ps::CursorShadow, g_cursor_ps_shadow, sizeof(g_cursor_ps_shadow)},
        {Ps::ShadowMask, g_shadow_mask_ps_main, sizeof(g_shadow_mask_ps_main)},
        {Ps::ShadowComposite, g_composite_ps_shadow, sizeof(g_composite_ps_shadow)},
        {Ps::Multiply, g_composite_ps_multiply, sizeof(g_composite_ps_multiply)},
        {Ps::Transition, g_transition_ps_main, sizeof(g_transition_ps_main)},
    };
    static_assert(sizeof(blobs) / sizeof(blobs[0]) == static_cast<size_t>(Ps::Count), "one blob per pixel shader");
    for (const PsBlob& b : blobs)
        CT_RETURN_IF_FAILED(d->CreatePixelShader(b.code, b.size, nullptr, p->ps[static_cast<int>(b.id)].GetAddressOf()));

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(QuadCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CT_RETURN_IF_FAILED(d->CreateBuffer(&bd, nullptr, p->cb.GetAddressOf()));

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CT_RETURN_IF_FAILED(d->CreateBlendState(&blend, p->blend_premul.GetAddressOf()));
    blend.RenderTarget[0].BlendEnable = FALSE;
    CT_RETURN_IF_FAILED(d->CreateBlendState(&blend, p->blend_opaque.GetAddressOf()));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    CT_RETURN_IF_FAILED(d->CreateSamplerState(&sd, p->lin.GetAddressOf()));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    CT_RETURN_IF_FAILED(d->CreateSamplerState(&sd, p->pt.GetAddressOf()));

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    CT_RETURN_IF_FAILED(d->CreateRasterizerState(&rd, p->raster.GetAddressOf()));

    for (auto& t : m_timers) CT_RETURN_IF_FAILED(t.Init(dev));

    // Direct2D device from the DXGI device of the shared D3D11 device; DirectWrite factory.
    D2D1_FACTORY_OPTIONS fo{};
    fo.debugLevel = D2D1_DEBUG_LEVEL_NONE;
    CT_RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo,
                                          reinterpret_cast<void**>(m_d2d_factory.GetAddressOf())));
    ComPtr<IDXGIDevice> dxgi;
    CT_RETURN_IF_FAILED(d->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf())));
    CT_RETURN_IF_FAILED(m_d2d_factory->CreateDevice(dxgi.Get(), m_d2d_device.GetAddressOf()));
    CT_RETURN_IF_FAILED(m_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2d_ctx.GetAddressOf()));
    m_d2d_ctx->SetDpi(96.f, 96.f);   // 1 DIP = 1 px
    CT_RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                            reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf())));

    m_p = std::move(p);
    m_dev = &dev;
    return S_OK;
}

void RenderContext::Shutdown() {
    m_temps.clear();
    for (auto& t : m_timers) t.Shutdown();
    m_d2d_ctx.Reset();
    m_d2d_device.Reset();
    m_d2d_factory.Reset();
    m_dwrite.Reset();
    m_p.reset();
    m_dev = nullptr;
}

RenderContext::TempRT* RenderContext::AcquireTemp(SizeI size, DXGI_FORMAT format) {
    if (!m_dev || size.IsEmpty()) return nullptr;
    for (auto& e : m_temps)
        if (!e->in_use && e->format == format && e->rt.size.w == size.w && e->rt.size.h == size.h) {
            e->in_use = true;
            return &e->rt;
        }
    auto e = std::make_unique<TempEntry>();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(size.w);
    td.Height = static_cast<UINT>(size.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device* d = m_dev->Device();
    if (FAILED(d->CreateTexture2D(&td, nullptr, e->rt.tex.GetAddressOf())) ||
        FAILED(d->CreateRenderTargetView(e->rt.tex.Get(), nullptr, e->rt.rtv.GetAddressOf())) ||
        FAILED(d->CreateShaderResourceView(e->rt.tex.Get(), nullptr, e->rt.srv.GetAddressOf()))) {
        CT_LOG_ERROR("RenderContext: could not create a %dx%d temp render target", size.w, size.h);
        return nullptr;
    }
    e->rt.size = size;
    e->format = format;
    e->in_use = true;
    m_temps.push_back(std::move(e));
    return &m_temps.back()->rt;
}

void RenderContext::ReleaseTemp(TempRT* rt) {
    if (!rt) return;
    for (auto& e : m_temps)
        if (&e->rt == rt) {
            e->in_use = false;
            return;
        }
}

void RenderContext::BeginFrame() {
    for (auto& e : m_temps) e->in_use = false;
}

void RenderContext::UnbindSrvs() {
    ID3D11ShaderResourceView* nulls[4] = {};
    Ctx()->PSSetShaderResources(0, 4, nulls);
}

void RenderContext::SetFullscreen(ID3D11RenderTargetView* rtv, SizeI viewport) {
    ID3D11DeviceContext* c = Ctx();
    UnbindSrvs();
    c->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(viewport.w), static_cast<float>(viewport.h), 0.f, 1.f};
    c->RSSetViewports(1, &vp);
    c->RSSetState(m_p->raster.Get());
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(m_p->vs_full.Get(), nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->OMSetDepthStencilState(nullptr, 0);
    ID3D11SamplerState* samplers[2] = {m_p->lin.Get(), m_p->pt.Get()};
    c->PSSetSamplers(0, 2, samplers);
    ID3D11Buffer* cb = m_p->cb.Get();
    c->VSSetConstantBuffers(0, 1, &cb);
    c->PSSetConstantBuffers(0, 1, &cb);
    m_p->target_size = viewport;
}

void RenderContext::DrawFullscreen() {
    ID3D11DeviceContext* c = Ctx();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(m_p->vs_full.Get(), nullptr, 0);
    c->Draw(3, 0);
}

void RenderContext::SetBlendPremul() {
    const float f[4] = {};
    Ctx()->OMSetBlendState(m_p->blend_premul.Get(), f, 0xffffffffu);
}

void RenderContext::SetBlendOpaque() {
    const float f[4] = {};
    Ctx()->OMSetBlendState(m_p->blend_opaque.Get(), f, 0xffffffffu);
}

ID3D11SamplerState* RenderContext::Linear() { return m_p->lin.Get(); }
ID3D11SamplerState* RenderContext::Point() { return m_p->pt.Get(); }

platform::GpuTimer& RenderContext::Timer(int slot) {
    if (slot < 0) slot = 0;
    if (slot >= kTimerSlots) slot = kTimerSlots - 1;
    return m_timers[slot];
}

// ---------------------------------------------------------------------------------------------------------------------
// Draw helpers (render_internal.h)

void ZeroCb(QuadCb& cb) { std::memset(&cb, 0, sizeof(cb)); }

void SetColor(float out[4], const ColorF& c) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a;
}

void FullUv(PointF out[4]) {
    out[0] = {0, 0};
    out[1] = {1, 0};
    out[2] = {0, 1};
    out[3] = {1, 1};
}

void BuildRectQuad(QuadCb& cb, const Mat3& m, const RectF& rect, float expand_px, const PointF uv_corners[4]) {
    const float s = std::max(m.ScaleFactor(), 1e-6f);
    const float e = expand_px / s;   // expansion in base units
    const float w = std::max(rect.w, 1e-6f), h = std::max(rect.h, 1e-6f);
    const float fx[2] = {-e / w, 1.f + e / w};
    const float fy[2] = {-e / h, 1.f + e / h};
    const float hx = rect.w * 0.5f * s, hy = rect.h * 0.5f * s;
    for (int i = 0; i < 4; ++i) {
        const int ix = i & 1, iy = i >> 1;
        const float u = fx[ix], v = fy[iy];
        const PointF base{rect.x + u * rect.w, rect.y + v * rect.h};
        const PointF p = m.Apply(base);
        // Bilinear (affine for our corner sets) extrapolation of the uv corners.
        const PointF& a = uv_corners[0];
        const PointF& b = uv_corners[1];
        const PointF& c = uv_corners[2];
        const PointF& d = uv_corners[3];
        const float tu = (1 - u) * (1 - v) * a.x + u * (1 - v) * b.x + (1 - u) * v * c.x + u * v * d.x;
        const float tv = (1 - u) * (1 - v) * a.y + u * (1 - v) * b.y + (1 - u) * v * c.y + u * v * d.y;
        cb.corner[i][0] = p.x;
        cb.corner[i][1] = p.y;
        cb.corner[i][2] = (u - 0.5f) * rect.w * s;
        cb.corner[i][3] = (v - 0.5f) * rect.h * s;
        cb.uv[i][0] = tu;
        cb.uv[i][1] = tv;
    }
    cb.half[0] = hx;
    cb.half[1] = hy;
}

void BuildScreenQuad(QuadCb& cb, const RectF& px, SizeI uv_space) {
    for (int i = 0; i < 4; ++i) {
        const int ix = i & 1, iy = i >> 1;
        const float x = px.x + (ix ? px.w : 0.f);
        const float y = px.y + (iy ? px.h : 0.f);
        cb.corner[i][0] = x;
        cb.corner[i][1] = y;
        cb.corner[i][2] = (ix ? 0.5f : -0.5f) * px.w;
        cb.corner[i][3] = (iy ? 0.5f : -0.5f) * px.h;
        if (uv_space.w > 0 && uv_space.h > 0) {
            cb.uv[i][0] = x / static_cast<float>(uv_space.w);
            cb.uv[i][1] = y / static_cast<float>(uv_space.h);
        } else {
            cb.uv[i][0] = static_cast<float>(ix);
            cb.uv[i][1] = static_cast<float>(iy);
        }
    }
    cb.half[0] = px.w * 0.5f;
    cb.half[1] = px.h * 0.5f;
}

namespace {

void Upload(RenderContext& rc, QuadCb& cb) {
    Pipelines& p = rc.P();
    cb.target_size[0] = static_cast<float>(p.target_size.w);
    cb.target_size[1] = static_cast<float>(p.target_size.h);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(rc.Ctx()->Map(p.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof(cb));
        rc.Ctx()->Unmap(p.cb.Get(), 0);
    }
}

void Bind(RenderContext& rc, Ps ps, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1) {
    ID3D11DeviceContext* c = rc.Ctx();
    c->PSSetShader(rc.P().ps[static_cast<int>(ps)].Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {t0, t1};
    c->PSSetShaderResources(0, 2, srvs);
}

} // namespace

void DrawQuad(RenderContext& rc, Ps ps, QuadCb& cb, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1) {
    Upload(rc, cb);
    Bind(rc, ps, t0, t1);
    ID3D11DeviceContext* c = rc.Ctx();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    c->VSSetShader(rc.P().vs_quad.Get(), nullptr, 0);
    c->Draw(4, 0);
}

void DrawFull(RenderContext& rc, Ps ps, QuadCb& cb, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1) {
    Upload(rc, cb);
    Bind(rc, ps, t0, t1);
    rc.DrawFullscreen();
}

} // namespace ct::render
