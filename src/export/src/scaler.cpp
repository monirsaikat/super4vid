#define CT_LOG_CATEGORY "export"
#include <ct/export/scaler.h>

#include <ct/core/error.h>
#include <ct/core/sync.h>
#include <ct/platform/win.h>

#include "scale_ps_main.h"
#include "scale_vs_main.h"

#include <wrl/client.h>

namespace ct::exportng {

using Microsoft::WRL::ComPtr;

namespace {

struct Pipeline {
    ID3D11Device* device = nullptr;   // identity only
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> lin;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> opaque;
    ComPtr<ID3D11Buffer> cb;   // float4 source uv transform
};

SrwLock g_lock;
Pipeline g_pipe;

HRESULT GetPipeline(ID3D11Device* dev, Pipeline*& out) {
    ExclusiveGuard g(g_lock);
    if (g_pipe.device == dev && g_pipe.ps) {
        out = &g_pipe;
        return S_OK;
    }
    Pipeline p;
    p.device = dev;
    CT_RETURN_IF_FAILED(dev->CreateVertexShader(g_scale_vs_main, sizeof(g_scale_vs_main), nullptr, p.vs.GetAddressOf()));
    CT_RETURN_IF_FAILED(dev->CreatePixelShader(g_scale_ps_main, sizeof(g_scale_ps_main), nullptr, p.ps.GetAddressOf()));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CT_RETURN_IF_FAILED(dev->CreateSamplerState(&sd, p.lin.GetAddressOf()));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    CT_RETURN_IF_FAILED(dev->CreateRasterizerState(&rd, p.raster.GetAddressOf()));
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CT_RETURN_IF_FAILED(dev->CreateBlendState(&bd, p.opaque.GetAddressOf()));
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CT_RETURN_IF_FAILED(dev->CreateBuffer(&cbd, nullptr, p.cb.GetAddressOf()));
    g_pipe = std::move(p);
    out = &g_pipe;
    return S_OK;
}

// src_used: the part of src's texture that holds the image (top-left); dst is drawn into its top-left `size` pixels.
void Pass(ID3D11DeviceContext* ctx, const Pipeline& p, ID3D11ShaderResourceView* src, SizeI src_used, ID3D11RenderTargetView* dst,
          SizeI size) {
    ComPtr<ID3D11Resource> res;
    src->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> tex;
    D3D11_TEXTURE2D_DESC td{};
    if (res && SUCCEEDED(res.As(&tex))) tex->GetDesc(&td);
    const float tw = td.Width ? static_cast<float>(td.Width) : static_cast<float>(src_used.w);
    const float th = td.Height ? static_cast<float>(td.Height) : static_cast<float>(src_used.h);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(p.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* f = static_cast<float*>(m.pData);
        f[0] = static_cast<float>(src_used.w) / tw;
        f[1] = static_cast<float>(src_used.h) / th;
        f[2] = 0.5f / static_cast<float>(src_used.w);
        f[3] = 0.5f / static_cast<float>(src_used.h);
        ctx->Unmap(p.cb.Get(), 0);
    }
    ID3D11Buffer* cb = p.cb.Get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(1, &dst, nullptr);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(size.w), static_cast<float>(size.h), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(p.raster.Get());
    const float bf[4] = {};
    ctx->OMSetBlendState(p.opaque.Get(), bf, 0xffffffff);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    ID3D11SamplerState* s = p.lin.Get();
    ctx->PSSetSamplers(0, 1, &s);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ID3D11RenderTargetView* null_rtv = nullptr;
    ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
}

} // namespace

int Scaler::BoxPasses(SizeI src, SizeI dst) {
    int n = 0;
    while (dst.w > 0 && dst.h > 0 && src.w / 2 >= dst.w && src.h / 2 >= dst.h) {
        src = SizeI{src.w / 2, src.h / 2};
        ++n;
    }
    return n;
}

HRESULT Scaler::Downscale(render::RenderContext& rc, ID3D11ShaderResourceView* src, SizeI src_size, ID3D11RenderTargetView* dst,
                          SizeI dst_size) {
    if (!src || !dst || src_size.IsEmpty() || dst_size.IsEmpty()) return E_INVALIDARG;
    Pipeline* p = nullptr;
    CT_RETURN_IF_FAILED(GetPipeline(rc.Device().Device(), p));
    ID3D11DeviceContext* ctx = rc.Ctx();
    SizeI cur = src_size;
    ID3D11ShaderResourceView* cur_srv = src;
    render::RenderContext::TempRT* temps[2] = {nullptr, nullptr};
    int k = 0;
    const int passes = BoxPasses(src_size, dst_size);
    for (int i = 0; i < passes; ++i) {
        const SizeI half{cur.w / 2, cur.h / 2};
        render::RenderContext::TempRT* t = rc.AcquireTemp(half);
        if (!t) {
            for (auto* x : temps) if (x) rc.ReleaseTemp(x);
            return E_OUTOFMEMORY;
        }
        Pass(ctx, *p, cur_srv, cur, t->rtv.Get(), half);
        if (temps[k]) rc.ReleaseTemp(temps[k]);
        temps[k] = t;
        k ^= 1;
        cur = half;
        cur_srv = t->srv.Get();
    }
    Pass(ctx, *p, cur_srv, cur, dst, dst_size);
    for (auto* x : temps)
        if (x) rc.ReleaseTemp(x);
    return S_OK;
}

} // namespace ct::exportng
