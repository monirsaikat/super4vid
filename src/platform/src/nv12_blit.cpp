#define CT_LOG_CATEGORY "d3d"
#include <ct/platform/nv12_blit.h>

#include <ct/core/error.h>
#include <ct/core/log.h>

#include "nv12_blit_ps_box.h"
#include "nv12_blit_ps_main.h"
#include "nv12_blit_vs_main.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace ct::platform {

namespace {
struct Constants {
    float dst_texel_uv[2];
    float taps;
    float alpha;
};
} // namespace

HRESULT Nv12Blit::Init(D3DDevice& dev) {
    Shutdown();
    m_dev = &dev;
    ID3D11Device* d = dev.Device();
    CT_RETURN_IF_FAILED(d->CreateVertexShader(g_nv12_blit_vs_main, sizeof(g_nv12_blit_vs_main), nullptr, m_vs.GetAddressOf()));
    CT_RETURN_IF_FAILED(d->CreatePixelShader(g_nv12_blit_ps_main, sizeof(g_nv12_blit_ps_main), nullptr, m_ps.GetAddressOf()));
    CT_RETURN_IF_FAILED(d->CreatePixelShader(g_nv12_blit_ps_box, sizeof(g_nv12_blit_ps_box), nullptr, m_ps_box.GetAddressOf()));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CT_RETURN_IF_FAILED(d->CreateSamplerState(&sd, m_sampler.GetAddressOf()));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CT_RETURN_IF_FAILED(d->CreateBuffer(&bd, nullptr, m_cb.GetAddressOf()));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    CT_RETURN_IF_FAILED(d->CreateRasterizerState(&rd, m_rs.GetAddressOf()));
    return S_OK;
}

void Nv12Blit::Shutdown() {
    for (CachedViews& c : m_cache) c = CachedViews{};
    m_vs.Reset();
    m_ps.Reset();
    m_ps_box.Reset();
    m_sampler.Reset();
    m_cb.Reset();
    m_rs.Reset();
    m_dev = nullptr;
}

HRESULT Nv12Blit::CreatePlaneViews(ID3D11Device* d, ID3D11Texture2D* nv12, ID3D11ShaderResourceView** y, ID3D11ShaderResourceView** uv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8_UNORM;
    CT_RETURN_IF_FAILED(d->CreateShaderResourceView(nv12, &sd, y));
    sd.Format = DXGI_FORMAT_R8G8_UNORM;
    CT_RETURN_IF_FAILED(d->CreateShaderResourceView(nv12, &sd, uv));
    return S_OK;
}

Nv12Blit::CachedViews* Nv12Blit::Lookup(ID3D11Resource* res) {
    ++m_use;
    CachedViews* oldest = &m_cache[0];
    for (CachedViews& c : m_cache) {
        if (c.key == res) {
            c.last_use = m_use;
            return &c;
        }
        if (c.last_use < oldest->last_use) oldest = &c;
    }
    *oldest = CachedViews{};
    oldest->key = res;
    oldest->holder = res;
    oldest->last_use = m_use;
    return oldest;
}

HRESULT Nv12Blit::ToRgba(ID3D11DeviceContext* ctx, ID3D11Texture2D* nv12, ID3D11Texture2D* rgba_rt) {
    if (!m_vs || !ctx || !nv12 || !rgba_rt) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC src{}, dst{};
    nv12->GetDesc(&src);
    rgba_rt->GetDesc(&dst);
    if (src.Format != DXGI_FORMAT_NV12) return E_INVALIDARG;
    CachedViews* sv = Lookup(nv12);
    if (!sv->y) {
        HRESULT hr = CreatePlaneViews(m_dev->Device(), nv12, sv->y.GetAddressOf(), sv->uv.GetAddressOf());
        if (FAILED(hr)) {
            *sv = CachedViews{};
            return hr;
        }
    }
    ID3D11ShaderResourceView* y = sv->y.Get();
    ID3D11ShaderResourceView* uv = sv->uv.Get();
    CachedViews* rv = Lookup(rgba_rt);
    if (!rv->rtv) {
        HRESULT hr = m_dev->Device()->CreateRenderTargetView(rgba_rt, nullptr, rv->rtv.GetAddressOf());
        if (FAILED(hr)) {
            *rv = CachedViews{};
            return hr;
        }
    }
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(dst.Width), static_cast<float>(dst.Height), 0, 1};
    const bool box = src.Width > dst.Width * 2 || src.Height > dst.Height * 2;
    return Draw(ctx, y, uv, {static_cast<int32_t>(src.Width), static_cast<int32_t>(src.Height)}, rv->rtv.Get(), vp, box);
}

HRESULT Nv12Blit::Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* y, ID3D11ShaderResourceView* uv, SizeI src_size,
                       ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& viewport, bool box_filter, float alpha) {
    if (!m_vs || !ctx || !y || !uv || !rtv || viewport.Width <= 0 || viewport.Height <= 0) return E_INVALIDARG;
    Constants c{};
    c.dst_texel_uv[0] = 1.0f / viewport.Width;
    c.dst_texel_uv[1] = 1.0f / viewport.Height;
    const float scale = std::max(src_size.w / viewport.Width, src_size.h / viewport.Height);
    c.taps = box_filter ? std::clamp(std::ceil(scale / 2.0f), 1.0f, 16.0f) : 1.0f;
    c.alpha = alpha;
    D3D11_MAPPED_SUBRESOURCE m{};
    CT_RETURN_IF_FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    *static_cast<Constants*>(m.pData) = c;
    ctx->Unmap(m_cb.Get(), 0);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->PSSetShader(c.taps > 1.0f ? m_ps_box.Get() : m_ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {y, uv};
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[1] = {m_sampler.Get()};
    ctx->PSSetSamplers(0, 1, samplers);
    ID3D11Buffer* cbs[1] = {m_cb.Get()};
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->RSSetState(m_rs.Get());
    ctx->RSSetViewports(1, &viewport);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    return S_OK;
}

} // namespace ct::platform
