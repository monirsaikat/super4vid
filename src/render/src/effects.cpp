#define CT_LOG_CATEGORY "render"
#include <ct/render/effects.h>
#include <ct/render/readback.h>
#include <ct/render/transitions.h>

#include <ct/core/error.h>
#include <ct/core/log.h>

#include "render_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::render {

namespace {

constexpr float kMaxSigma = 8.f;
constexpr int kMaxDownsamples = 5;

HRESULT CreateRt(ID3D11Device* d, SizeI size, RenderContext::TempRT& rt) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(size.w);
    td.Height = static_cast<UINT>(size.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    rt = {};
    CT_RETURN_IF_FAILED(d->CreateTexture2D(&td, nullptr, rt.tex.GetAddressOf()));
    CT_RETURN_IF_FAILED(d->CreateRenderTargetView(rt.tex.Get(), nullptr, rt.rtv.GetAddressOf()));
    CT_RETURN_IF_FAILED(d->CreateShaderResourceView(rt.tex.Get(), nullptr, rt.srv.GetAddressOf()));
    rt.size = size;
    return S_OK;
}

void BlurPass(RenderContext& rc, ID3D11ShaderResourceView* src, SizeI size, float sigma, bool horizontal, ID3D11RenderTargetView* dst) {
    rc.SetFullscreen(dst, size);
    rc.SetBlendOpaque();
    QuadCb cb;
    ZeroCb(cb);
    cb.p0[0] = horizontal ? 1.f / static_cast<float>(size.w) : 0.f;
    cb.p0[1] = horizontal ? 0.f : 1.f / static_cast<float>(size.h);
    cb.p0[2] = sigma;
    cb.p0[3] = std::min(std::ceil(sigma * 3.f), kMaxSigma * 3.f);
    SetSourceUv(cb, 0, src, size);
    DrawFull(rc, Ps::Blur, cb, src);
}

// src_size: the used area of src (pooled temps are larger than what they hold).
void CopyPass(RenderContext& rc, ID3D11ShaderResourceView* src, SizeI src_size, SizeI dst_size, ID3D11RenderTargetView* dst) {
    rc.SetFullscreen(dst, dst_size);
    rc.SetBlendOpaque();
    QuadCb cb;
    ZeroCb(cb);
    SetSourceUv(cb, 0, src, src_size);
    DrawFull(rc, Ps::Copy, cb, src);
}

} // namespace

namespace effects {

void GaussianBlur(RenderContext& rc, ID3D11ShaderResourceView* src, SizeI src_size, float sigma_px, RenderContext::TempRT& dst) {
    if (!src || src_size.IsEmpty() || !dst.rtv) return;
    if (sigma_px < 0.3f) {
        CopyPass(rc, src, src_size, dst.size, dst.rtv.Get());
        return;
    }
    // Downsample by 2 while the blur is wide; each level halves sigma.
    RenderContext::TempRT* levels[kMaxDownsamples] = {};
    int n = 0;
    ID3D11ShaderResourceView* cur = src;
    SizeI cur_size = src_size;
    float sigma = sigma_px;
    while (sigma > kMaxSigma && n < kMaxDownsamples && cur_size.w > 2 && cur_size.h > 2) {
        const SizeI half{(cur_size.w + 1) / 2, (cur_size.h + 1) / 2};
        RenderContext::TempRT* t = rc.AcquireTemp(half);
        if (!t) break;
        CopyPass(rc, cur, cur_size, half, t->rtv.Get());
        levels[n++] = t;
        cur = t->srv.Get();
        cur_size = half;
        sigma *= 0.5f;
    }
    RenderContext::TempRT* tmp = rc.AcquireTemp(cur_size);
    if (!tmp) return;
    BlurPass(rc, cur, cur_size, sigma, true, tmp->rtv.Get());
    if (n == 0 && cur_size.w == dst.size.w && cur_size.h == dst.size.h) {
        BlurPass(rc, tmp->srv.Get(), cur_size, sigma, false, dst.rtv.Get());
    } else {
        RenderContext::TempRT* out = rc.AcquireTemp(cur_size);
        if (out) {
            BlurPass(rc, tmp->srv.Get(), cur_size, sigma, false, out->rtv.Get());
            CopyPass(rc, out->srv.Get(), cur_size, dst.size, dst.rtv.Get());   // bilinear upsample
            rc.ReleaseTemp(out);
        }
    }
    rc.ReleaseTemp(tmp);
    for (int i = 0; i < n; ++i) rc.ReleaseTemp(levels[i]);
}

void Pixelate(RenderContext& rc, ID3D11ShaderResourceView* src, SizeI src_size, int block_px, RectF region,
              ID3D11RenderTargetView* dst, SizeI dst_size, float radius_px, float opacity) {
    if (!src || src_size.IsEmpty() || !dst) return;
    rc.SetFullscreen(dst, dst_size);
    rc.SetBlendPremul();
    QuadCb cb;
    ZeroCb(cb);
    BuildScreenQuad(cb, region, SizeI{});
    const float b = static_cast<float>(std::max(block_px, 1));
    cb.p0[0] = b / static_cast<float>(src_size.w);
    cb.p0[1] = b / static_cast<float>(src_size.h);
    cb.radius = radius_px;
    cb.mask = 1.f;
    cb.opacity = opacity;
    SetSourceUv(cb, 0, src, src_size);
    DrawQuad(rc, Ps::Pixelate, cb, src);
}

void RoundedShadow(RenderContext& rc, RectF rect, float radius_px, const project::Shadow& shadow, Mat3 m,
                   ID3D11RenderTargetView* dst, SizeI dst_size, float opacity_mul) {
    if (!shadow.enabled || shadow.opacity * opacity_mul <= 0.f || rect.w <= 0 || rect.h <= 0 || !dst || dst_size.IsEmpty()) return;
    ShadowCache& cache = rc.P().shadows;
    const float s = m.ScaleFactor();
    const float sigma = std::max(shadow.blur_px, 0.f) * 0.5f * s;   // target px
    const SizeI mask_size{(dst_size.w + 3) / 4, (dst_size.h + 3) / 4};
    const float key[ShadowCache::kKeyFloats] = {rect.x, rect.y, rect.w, rect.h, radius_px, shadow.blur_px,
                                                m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5],
                                                static_cast<float>(dst_size.w), static_cast<float>(dst_size.h)};
    ++cache.tick;
    ShadowCache::Entry* hit = nullptr;
    for (auto& e : cache.e)
        if (e.valid && std::memcmp(e.key, key, sizeof(key)) == 0) {
            hit = &e;
            break;
        }
    if (!hit) {
        // The least recently used entry (an invalid one first).
        ShadowCache::Entry* slot = &cache.e[0];
        for (auto& e : cache.e)
            if (!e.valid || e.last_use < slot->last_use) {
                slot = &e;
                if (!e.valid) break;
            }
        if (!slot->tex.tex || slot->tex.size.w != mask_size.w || slot->tex.size.h != mask_size.h) {
            if (FAILED(CreateRt(rc.Device().Device(), mask_size, slot->tex))) return;
        }
        slot->valid = false;
        RenderContext::TempRT* raw = rc.AcquireTemp(mask_size);
        if (!raw) return;
        const float clear[4] = {};
        rc.Ctx()->ClearRenderTargetView(raw->rtv.Get(), clear);
        rc.SetFullscreen(raw->rtv.Get(), mask_size);
        rc.SetBlendOpaque();
        const float qx = static_cast<float>(mask_size.w) / static_cast<float>(dst_size.w);
        const float qy = static_cast<float>(mask_size.h) / static_cast<float>(dst_size.h);
        const Mat3 mq = Mat3::Scale(qx, qy) * m;
        QuadCb cb;
        ZeroCb(cb);
        PointF uv[4];
        FullUv(uv);
        BuildRectQuad(cb, mq, rect, 1.f, uv);
        cb.radius = radius_px * mq.ScaleFactor();
        DrawQuad(rc, Ps::ShadowMask, cb);
        GaussianBlur(rc, raw->srv.Get(), mask_size, sigma * qx, slot->tex);
        rc.ReleaseTemp(raw);
        std::memcpy(slot->key, key, sizeof(key));
        slot->valid = true;
        ++cache.renders;
        hit = slot;
    }
    hit->last_use = cache.tick;

    // Composite over the transformed rect's bounds, grown by the blur reach and the offset.
    const float off = shadow.offset_y_px * s;
    RectF b = TransformBounds(m, rect);
    const float reach = sigma * 3.f + 2.f;
    b.x -= reach;
    b.w += 2 * reach;
    b.y -= reach;
    b.h += 2 * reach + std::fabs(off);
    if (off < 0) b.y += off;
    const float x0 = std::max(b.x, 0.f), y0 = std::max(b.y, 0.f);
    const float x1 = std::min(b.x + b.w, static_cast<float>(dst_size.w)), y1 = std::min(b.y + b.h, static_cast<float>(dst_size.h));
    if (x1 <= x0 || y1 <= y0) return;
    rc.SetFullscreen(dst, dst_size);
    rc.SetBlendPremul();
    QuadCb cb;
    ZeroCb(cb);
    BuildScreenQuad(cb, RectF{x0, y0, x1 - x0, y1 - y0}, dst_size);
    const float duv = off / static_cast<float>(dst_size.h);
    for (auto& uv : cb.uv) uv[1] -= duv;
    cb.color[3] = std::clamp(shadow.opacity * opacity_mul, 0.f, 1.f);
    cb.opacity = 1.f;
    DrawQuad(rc, Ps::ShadowComposite, cb, hit->tex.srv.Get());
}

uint64_t ShadowMaskRenders(RenderContext& rc) { return rc.P().shadows.renders; }

} // namespace effects

namespace transitions {

void Apply(RenderContext& rc, const project::Transition& tr, float u, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b,
           ID3D11RenderTargetView* dst, SizeI size) {
    rc.SetFullscreen(dst, size);
    rc.SetBlendPremul();
    QuadCb cb;
    ZeroCb(cb);
    cb.p0[0] = std::clamp(u, 0.f, 1.f);
    float dx = 0, dy = 0;
    switch (tr.kind) {
    case project::Transition::Kind::Crossfade: cb.p0[1] = 0.f; break;
    case project::Transition::Kind::Dip: cb.p0[1] = 1.f; break;
    case project::Transition::Kind::Slide:
        cb.p0[1] = 2.f;
        switch (tr.dir) {
        case project::Transition::Dir::Left: dx = -1; break;
        case project::Transition::Dir::Right: dx = 1; break;
        case project::Transition::Dir::Up: dy = -1; break;
        case project::Transition::Dir::Down: dy = 1; break;
        }
        break;
    }
    cb.p0[2] = dx;
    cb.p0[3] = dy;
    SetColor(cb.color, tr.color);
    SetSourceUv(cb, 0, a, size);   // a and b may be pooled temps larger than the canvas
    SetSourceUv(cb, 1, b, size);
    DrawFull(rc, Ps::Transition, cb, a, b);
}

} // namespace transitions

namespace {

HRESULT ReadbackImpl(platform::D3DDevice& dev, ID3D11Texture2D* src, std::vector<uint8_t>& out, SizeI& size, bool rgba_out) {
    if (!src || !dev.Device()) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    const bool src_bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                          d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    const bool src_rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                          d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    if (!src_bgra && !src_rgba) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    CT_RETURN_IF_FAILED(dev.Device()->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()));
    platform::D3DDevice::Lock lock(dev);
    ID3D11DeviceContext* c = dev.Context();
    c->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE ms{};
    CT_RETURN_IF_FAILED(c->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms));
    out.resize(static_cast<size_t>(d.Width) * d.Height * 4);
    const bool swap = src_bgra == rgba_out;   // BGRA -> RGBA or RGBA -> BGRA
    for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch;
        uint8_t* o = out.data() + static_cast<size_t>(y) * d.Width * 4;
        if (!swap) {
            std::memcpy(o, row, static_cast<size_t>(d.Width) * 4);
        } else {
            for (UINT x = 0; x < d.Width; ++x) {
                o[x * 4 + 0] = row[x * 4 + 2];
                o[x * 4 + 1] = row[x * 4 + 1];
                o[x * 4 + 2] = row[x * 4 + 0];
                o[x * 4 + 3] = row[x * 4 + 3];
            }
        }
    }
    c->Unmap(staging.Get(), 0);
    size = SizeI{static_cast<int32_t>(d.Width), static_cast<int32_t>(d.Height)};
    return S_OK;
}

} // namespace

HRESULT Readback::ToBgra8(platform::D3DDevice& dev, ID3D11Texture2D* src, std::vector<uint8_t>& out, SizeI& size) {
    return ReadbackImpl(dev, src, out, size, false);
}

HRESULT Readback::ToRgba8(platform::D3DDevice& dev, ID3D11Texture2D* src, std::vector<uint8_t>& out, SizeI& size) {
    return ReadbackImpl(dev, src, out, size, true);
}

} // namespace ct::render
