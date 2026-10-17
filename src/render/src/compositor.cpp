#define CT_LOG_CATEGORY "render"
#include <ct/render/compositor.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/render/effects.h>
#include <ct/render/transitions.h>

#include "render_internal.h"

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace ct::render {

namespace {

using project::Annotation;
using project::Clip;
using project::Project;

double QpcMs(int64_t ticks) {
    static int64_t freq = 0;
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

int64_t Qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

float EaseOut(float u) {
    const float v = 1.f - std::clamp(u, 0.f, 1.f);
    return 1.f - v * v * v;
}

// Display uv (of the rotated frame) -> texture uv, for a clockwise rotation applied to the decoded texture.
PointF RotateUv(PointF d, int rotation_deg) {
    switch (((rotation_deg % 360) + 360) % 360) {
    case 90: return PointF{d.y, 1.f - d.x};
    case 180: return PointF{1.f - d.x, 1.f - d.y};
    case 270: return PointF{1.f - d.y, d.x};
    default: return d;
    }
}

bool ClipActive(const Clip& c, TimeUs t) { return t >= c.start_us && t < c.End(); }

bool TrackHidden(const Project& p, const project::Id& track) {
    const project::Track* tr = p.FindTrack(track);
    return !tr || tr->hidden;
}

constexpr TimeUs kBadgeFadeInUs = 120'000, kBadgeFadeOutUs = 200'000;
constexpr int kMaxBadgeStrokes = 3;

} // namespace

Compositor::~Compositor() { Shutdown(); }

HRESULT Compositor::Init(RenderContext& rc) {
    Shutdown();
    if (!rc.IsInitialized()) return E_UNEXPECTED;
    m_rc = &rc;
    CT_RETURN_IF_FAILED(m_text.Init(rc));
    CT_RETURN_IF_FAILED(m_cursor.Init(rc));
    m_ann.reserve(64);
    m_keys.reserve(kMaxBadgeStrokes);
    m_done.reserve(16);
    return S_OK;
}

void Compositor::Shutdown() {
    m_text.Shutdown();
    m_cursor.Shutdown();
    m_bg_blur = {};
    m_bg_key_srv = nullptr;
    m_rc = nullptr;
}

bool Compositor::IsPrimaryScreen(const Project& p, const Clip& c) {
    if (c.source != Clip::Source::Screen) return false;
    const Clip* primary = p.PrimaryScreenClip();
    return primary && primary->track == c.track;
}

TimeUs Compositor::ClipMediaTime(const Clip& c, TimeUs t) {
    const TimeUs m = c.MediaTimeAt(t);
    return std::clamp(m, c.in_us, std::max(c.in_us, c.out_us - 1));
}

bool Compositor::TransitionProgress(const Project& p, const project::Transition& tr, TimeUs t, float& u) {
    const Clip* b = p.FindClip(tr.b);
    if (!b || !p.FindClip(tr.a) || tr.duration_us <= 0) return false;
    const TimeUs start = b->start_us - tr.duration_us / 2;
    if (t < start || t >= start + tr.duration_us) return false;
    u = static_cast<float>(static_cast<double>(t - start) / static_cast<double>(tr.duration_us));
    return true;
}

void Compositor::ActiveTransitions(const Project& p, TimeUs t,
                                   std::vector<std::pair<const project::Clip*, const project::Transition*>>& out) {
    out.clear();
    for (const project::Transition& tr : p.transitions) {
        float u = 0;
        if (TransitionProgress(p, tr, t, u)) out.emplace_back(p.FindClip(tr.b), &tr);
    }
}

bool Compositor::ComputeGeom(const Project& p, const Clip& clip, TimeUs t, SizeI video, int rotation_deg, const Mat3& cam,
                             LayerGeom& g) const {
    if (video.IsEmpty()) return false;
    const project::Canvas& cv = p.canvas;
    const float vw = static_cast<float>(video.w), vh = static_cast<float>(video.h);
    // Crop (source pixels of the displayed frame) as a uv window.
    float u0 = std::clamp(static_cast<float>(clip.crop.left) / vw, 0.f, 1.f);
    float v0 = std::clamp(static_cast<float>(clip.crop.top) / vh, 0.f, 1.f);
    float u1 = std::clamp(1.f - static_cast<float>(clip.crop.right) / vw, 0.f, 1.f);
    float v1 = std::clamp(1.f - static_cast<float>(clip.crop.bottom) / vh, 0.f, 1.f);
    if (u1 - u0 < 1.f / vw) u1 = std::min(1.f, u0 + 1.f / vw);
    if (v1 - v0 < 1.f / vh) v1 = std::min(1.f, v0 + 1.f / vh);
    SizeI content{std::max(1, static_cast<int>(std::lround((u1 - u0) * vw))), std::max(1, static_cast<int>(std::lround((v1 - v0) * vh)))};

    g.primary = IsPrimaryScreen(p, clip);
    const bool pip = clip.source == Clip::Source::Camera && clip.pip.has_value();
    bool mirror = false;
    if (g.primary) {
        g.base = layer_math::FitContain(content, layer_math::StageRect(cv));
    } else if (pip) {
        g.base = layer_math::PipRect(*clip.pip, content, cv);
        mirror = clip.pip->mirror;
        if (clip.pip->mask == project::Pip::Mask::Circle) {
            // Centre-square crop of the camera image.
            const float cw = (u1 - u0) * vw, ch = (v1 - v0) * vh;
            if (cw > ch) {
                const float d = (cw - ch) * 0.5f / vw;
                u0 += d;
                u1 -= d;
            } else if (ch > cw) {
                const float d = (ch - cw) * 0.5f / vh;
                v0 += d;
                v1 -= d;
            }
        }
    } else {
        g.base = layer_math::FitContain(content, RectF{0, 0, static_cast<float>(cv.width), static_cast<float>(cv.height)});
    }
    g.clip_m = layer_math::ClipTransform(clip, t, g.base, cv);
    g.full = cam * g.clip_m;

    // Corner radius and shadow.
    g.radius = static_cast<float>(clip.corner_radius_px.value_or(g.primary ? cv.corner_radius_px : 0));
    if (pip && !clip.corner_radius_px) {
        switch (clip.pip->mask) {
        case project::Pip::Mask::Circle: g.radius = std::min(g.base.w, g.base.h) * 0.5f; break;
        case project::Pip::Mask::Rounded: g.radius = static_cast<float>(clip.pip->radius_px); break;
        case project::Pip::Mask::None: g.radius = 0; break;
        }
    }
    if (pip && clip.pip->mask == project::Pip::Mask::Circle) g.radius = std::min(g.base.w, g.base.h) * 0.5f;
    // Pointers, not copies: project structs carry a Json member and copying one allocates.
    static const project::Shadow kPipShadow = [] {
        project::Shadow sh;
        sh.blur_px = 32;
        sh.offset_y_px = 12;
        sh.opacity = 0.4f;
        return sh;
    }();
    if (clip.shadow) g.shadow = &*clip.shadow;
    else if (g.primary) g.shadow = &cv.shadow;
    else if (pip && clip.pip->shadow) g.shadow = &kPipShadow;
    else g.shadow = nullptr;
    if (g.shadow && !g.shadow->enabled) g.shadow = nullptr;

    // uv corners (TL, TR, BL, BR) in display space, mirrored, then rotated into texture space.
    const float du[2] = {mirror ? u1 : u0, mirror ? u0 : u1};
    const float dv[2] = {v0, v1};
    for (int i = 0; i < 4; ++i) g.uv[i] = RotateUv(PointF{du[i & 1], dv[i >> 1]}, rotation_deg);

    // video px -> base rect.
    const float sx = g.base.w / ((u1 - u0) * vw), sy = g.base.h / ((v1 - v0) * vh);
    if (mirror)
        g.video_to_base = Mat3::Translate(g.base.x + g.base.w, g.base.y) * Mat3::Scale(-sx, sy) * Mat3::Translate(-u0 * vw, -v0 * vh);
    else
        g.video_to_base = Mat3::Translate(g.base.x, g.base.y) * Mat3::Scale(sx, sy) * Mat3::Translate(-u0 * vw, -v0 * vh);
    return true;
}

RectF Compositor::LayerRectOnCanvas(const Project& p, const Clip& clip, TimeUs t, SizeI video_size) const {
    LayerGeom g;
    const Mat3 cam = layer_math::CameraTransform(p.camera.At(t), p.canvas);
    if (!ComputeGeom(p, clip, t, video_size, 0, cam, g)) return RectF{};
    return TransformBounds(g.full, g.base);
}

Mat3 Compositor::VideoToCanvas(const Project& p, const Clip& clip, TimeUs t, SizeI video_size) const {
    LayerGeom g;
    const Mat3 cam = layer_math::CameraTransform(p.camera.At(t), p.canvas);
    if (!ComputeGeom(p, clip, t, video_size, 0, cam, g)) return cam;
    return g.full * g.video_to_base;
}

void Compositor::DrawLayer(const Project& p, const VideoLayerInput& in, TimeUs t, const Mat3& cam, ID3D11RenderTargetView* rtv,
                           SizeI size) {
    LayerGeom g;
    if (!in.clip || !ComputeGeom(p, *in.clip, t, in.video_size, in.rotation_deg, cam, g)) return;
    const bool have = in.is_rgba ? in.rgba_srv != nullptr : (in.y_srv && in.uv_srv);
    if (!have) return;
    const float opacity = std::clamp(layer_math::PropAt(*in.clip, "transform.opacity", t, in.clip->transform.opacity), 0.f, 1.f);
    if (opacity <= 0.f) return;
    if (g.shadow) effects::RoundedShadow(*m_rc, g.base, g.radius, *g.shadow, g.full, rtv, size, opacity);
    m_rc->SetFullscreen(rtv, size);
    m_rc->SetBlendPremul();
    QuadCb cb;
    ZeroCb(cb);
    BuildRectQuad(cb, g.full, g.base, 1.f, g.uv);
    cb.radius = g.radius * g.full.ScaleFactor();
    cb.mask = 1.f;
    cb.opacity = opacity;
    cb.premul = in.rgba_premultiplied ? 1.f : 0.f;
    if (in.is_rgba) DrawQuad(*m_rc, Ps::Rgba, cb, in.rgba_srv);
    else DrawQuad(*m_rc, Ps::Nv12, cb, in.y_srv, in.uv_srv);
}

void Compositor::DrawBackground(const Project& p, const FrameInputs& in, const RenderOptions& opt, ID3D11RenderTargetView* rtv,
                                SizeI size) {
    ID3D11DeviceContext* c = m_rc->Ctx();
    const project::Background& bg = p.canvas.background;
    if (!opt.background) {
        const float zero[4] = {};
        c->ClearRenderTargetView(rtv, zero);
        return;
    }
    const float solid[4] = {bg.color_a.r, bg.color_a.g, bg.color_a.b, 1.f};
    switch (bg.kind) {
    case project::Background::Kind::Solid: c->ClearRenderTargetView(rtv, solid); return;
    case project::Background::Kind::Gradient: {
        m_rc->SetFullscreen(rtv, size);
        m_rc->SetBlendOpaque();
        QuadCb cb;
        ZeroCb(cb);
        const float a = bg.angle_deg * 3.14159265f / 180.f;
        const float dx = std::sin(a), dy = -std::cos(a);
        const float len = std::fabs(static_cast<float>(size.w) * dx) + std::fabs(static_cast<float>(size.h) * dy);
        cb.p0[0] = dx;
        cb.p0[1] = dy;
        cb.p0[2] = std::max(len, 1.f);
        SetColor(cb.color, bg.color_a);
        SetColor(cb.p1, bg.color_b);
        DrawFull(*m_rc, Ps::Gradient, cb);
        return;
    }
    case project::Background::Kind::Image: {
        c->ClearRenderTargetView(rtv, solid);
        if (!in.background_image || in.background_image_size.IsEmpty()) return;
        ID3D11ShaderResourceView* src = in.background_image;
        if (bg.blur > 0.5f) {
            const SizeI is = in.background_image_size;
            if (m_bg_key_srv != src || m_bg_key_blur != bg.blur || m_bg_blur.size.w != is.w || m_bg_blur.size.h != is.h) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = static_cast<UINT>(is.w);
                td.Height = static_cast<UINT>(is.h);
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                ID3D11Device* d = m_rc->Device().Device();
                m_bg_blur = {};
                if (FAILED(d->CreateTexture2D(&td, nullptr, m_bg_blur.tex.GetAddressOf())) ||
                    FAILED(d->CreateRenderTargetView(m_bg_blur.tex.Get(), nullptr, m_bg_blur.rtv.GetAddressOf())) ||
                    FAILED(d->CreateShaderResourceView(m_bg_blur.tex.Get(), nullptr, m_bg_blur.srv.GetAddressOf()))) {
                    m_bg_blur = {};
                    m_bg_key_srv = nullptr;
                } else {
                    m_bg_blur.size = is;
                    effects::GaussianBlur(*m_rc, src, is, bg.blur, m_bg_blur);
                    m_bg_key_srv = src;
                    m_bg_key_blur = bg.blur;
                }
            }
            if (m_bg_blur.srv) src = m_bg_blur.srv.Get();
        }
        // Cover fit: scale the image so it fills the canvas, centred, cropping the overflow.
        const float iw = static_cast<float>(in.background_image_size.w), ih = static_cast<float>(in.background_image_size.h);
        const float cw = static_cast<float>(size.w), ch = static_cast<float>(size.h);
        const float s = std::max(cw / iw, ch / ih);
        const float uw = cw / (iw * s), vh = ch / (ih * s);
        const float u0 = (1.f - uw) * 0.5f, v0 = (1.f - vh) * 0.5f;
        m_rc->SetFullscreen(rtv, size);
        m_rc->SetBlendPremul();
        QuadCb cb;
        ZeroCb(cb);
        BuildScreenQuad(cb, RectF{0, 0, cw, ch}, SizeI{});
        for (int i = 0; i < 4; ++i) {
            cb.uv[i][0] = u0 + ((i & 1) ? uw : 0.f);
            cb.uv[i][1] = v0 + ((i >> 1) ? vh : 0.f);
        }
        cb.opacity = 1.f;
        DrawQuad(*m_rc, Ps::Rgba, cb, src);
        return;
    }
    }
}

const VideoLayerInput* Compositor::TopScreenLayer(const FrameInputs& in, TimeUs t) const {
    for (size_t i = in.layers.size(); i-- > 0;) {
        const VideoLayerInput& l = in.layers[i];
        if (l.clip && l.events && l.clip->source == Clip::Source::Screen && ClipActive(*l.clip, t)) return &l;
    }
    return nullptr;
}

void Compositor::PrepareText(const Project& p, const FrameInputs& in, const RenderOptions& opt, TimeUs t, float zoom) {
    m_ann.clear();
    m_badge = nullptr;
    m_badge_alpha = 0;
    const SizeI canvas{p.canvas.width, p.canvas.height};
    const float scale = std::clamp(std::ceil(zoom * 2.f) * 0.5f, 1.f, 4.f);   // raster scale, quantized for the cache
    const VideoLayerInput* screen = TopScreenLayer(in, t);

    if (opt.annotations) {
        for (const project::Track& tr : p.tracks) {
            if (tr.kind != project::Track::Kind::Annotations || tr.hidden) continue;
            for (const Clip& c : p.clips) {
                if (c.track != tr.id || !c.annotation || !ClipActive(c, t)) continue;
                const Annotation& a = *c.annotation;
                AnnItem item;
                item.clip = &c;
                if (a.type == Annotation::Type::Highlight || a.type == Annotation::Type::Blur) {
                    m_ann.push_back(item);
                    continue;
                }
                if (a.type == Annotation::Type::Keystrokes) {
                    if (!screen) continue;
                    const auto strokes = screen->events->StrokesVisibleAt(screen->media_t_us,
                                                                          static_cast<TimeUs>(screen->clip->keystrokes.hold_ms) * 1000);
                    if (strokes.empty()) continue;
                    const size_t n = std::min<size_t>(strokes.size(), kMaxBadgeStrokes);
                    m_keys.resize(n);
                    for (size_t i = 0; i < n; ++i) m_keys[i] = strokes[strokes.size() - n + i].text;
                    if (SUCCEEDED(m_text.KeystrokeBadge(m_keys, screen->clip->keystrokes, canvas, scale, item.layer, static_cast<float>(a.size_px))))
                        m_ann.push_back(item);
                    continue;
                }
                if (SUCCEEDED(m_text.Annotation(a, canvas, scale, item.layer))) m_ann.push_back(item);
            }
        }
    }

    if (opt.keystrokes && screen && screen->clip->keystrokes.enabled) {
        const TimeUs hold = static_cast<TimeUs>(std::max(screen->clip->keystrokes.hold_ms, 1)) * 1000;
        const TimeUs mt = screen->media_t_us;
        const auto strokes = screen->events->StrokesVisibleAt(mt, hold);
        if (!strokes.empty()) {
            const size_t n = std::min<size_t>(strokes.size(), kMaxBadgeStrokes);
            m_keys.resize(n);
            for (size_t i = 0; i < n; ++i) m_keys[i] = strokes[strokes.size() - n + i].text;
            const TimeUs first = strokes[strokes.size() - n].t_us;
            const TimeUs last = strokes.back().t_us;
            const float fin = std::clamp(static_cast<float>(mt - first) / static_cast<float>(kBadgeFadeInUs), 0.f, 1.f);
            const float fout = std::clamp(static_cast<float>(last + hold - mt) / static_cast<float>(kBadgeFadeOutUs), 0.f, 1.f);
            m_badge_alpha = std::min(fin, fout);
            if (m_badge_alpha > 0.f && FAILED(m_text.KeystrokeBadge(m_keys, screen->clip->keystrokes, canvas, 1.f, m_badge)))
                m_badge = nullptr;
        }
    }
}

void Compositor::DrawCursorAndBadges(const Project& p, const FrameInputs& in, const RenderOptions& opt, TimeUs t,
                                     const Mat3& cam, ID3D11RenderTargetView* rtv, SizeI size) {
    const VideoLayerInput* screen = TopScreenLayer(in, t);
    if (opt.cursor && screen && screen->clip->cursor.visible) {
        LayerGeom g;
        if (ComputeGeom(p, *screen->clip, t, screen->video_size, screen->rotation_deg, cam, g)) {
            Mat3 m = g.full * g.video_to_base;
            const SizeI region = screen->events->RegionSize();
            if (!region.IsEmpty() && (region.w != screen->video_size.w || region.h != screen->video_size.h))
                m = m * Mat3::Scale(static_cast<float>(screen->video_size.w) / static_cast<float>(region.w),
                                    static_cast<float>(screen->video_size.h) / static_cast<float>(region.h));
            const float layer_scale = m.ScaleFactor();
            m_cursor.Prepare(*screen->events);
            m_cursor.Draw(screen->clip->cursor, *screen->events, screen->media_t_us, m, screen->clip->cursor.scale * layer_scale,
                          rtv, size);
        }
    }
    if (m_badge && m_badge_alpha > 0.f) {
        const project::KeystrokeStyle& ks = screen->clip->keystrokes;
        const float cw = static_cast<float>(size.w), ch = static_cast<float>(size.h);
        const float bw = m_badge->canvas_rect.w, bh = m_badge->canvas_rect.h;
        float x = (cw - bw) * 0.5f, y = ch * 0.94f - bh;
        switch (ks.pos) {
        case project::KeystrokeStyle::Pos::BottomLeft: x = ch * 0.06f; break;
        case project::KeystrokeStyle::Pos::BottomRight: x = cw - ch * 0.06f - bw; break;
        case project::KeystrokeStyle::Pos::TopCenter: y = ch * 0.06f; break;
        default: break;
        }
        x = std::round(x);
        y = std::round(y);
        m_rc->SetFullscreen(rtv, size);
        m_rc->SetBlendPremul();
        QuadCb cb;
        ZeroCb(cb);
        BuildScreenQuad(cb, RectF{x, y, bw, bh}, SizeI{});
        cb.premul = 1.f;
        cb.opacity = m_badge_alpha;
        DrawQuad(*m_rc, Ps::Rgba, cb, m_badge->srv.Get());
    }
}

void Compositor::DrawRegionEffect(const Clip& c, const Annotation& a, TimeUs, const Mat3& cam, float zoom, float alpha,
                                  ID3D11RenderTargetView* rtv, SizeI size) {
    if (!m_target_res) return;
    const float cw = static_cast<float>(size.w), ch = static_cast<float>(size.h);
    (void)c;
    RectF r{a.rect.x * cw, a.rect.y * ch, a.rect.w * cw, a.rect.h * ch};
    if (r.w < 0) {
        r.x += r.w;
        r.w = -r.w;
    }
    if (r.h < 0) {
        r.y += r.h;
        r.h = -r.h;
    }
    r = TransformBounds(cam, r);
    // Integer region inside the target.
    const int x0 = std::clamp(static_cast<int>(std::floor(r.x)), 0, size.w), y0 = std::clamp(static_cast<int>(std::floor(r.y)), 0, size.h);
    const int x1 = std::clamp(static_cast<int>(std::ceil(r.x + r.w)), 0, size.w), y1 = std::clamp(static_cast<int>(std::ceil(r.y + r.h)), 0, size.h);
    if (x1 - x0 < 2 || y1 - y0 < 2) return;
    const float radius = static_cast<float>(std::max(a.radius_px, 0)) * zoom;
    const bool blur = a.type == Annotation::Type::Blur;
    // Blur reads a margin around the region so its edge is not darkened or clamped.
    const int margin = blur && !a.pixelate ? std::min(static_cast<int>(std::ceil(a.blur_px * 0.5f * zoom * 3.f)), 256) : 0;
    const int bx0 = std::max(x0 - margin, 0), by0 = std::max(y0 - margin, 0);
    const int bx1 = std::min(x1 + margin, size.w), by1 = std::min(y1 + margin, size.h);
    const SizeI copy_size{bx1 - bx0, by1 - by0};
    RenderContext::TempRT* copy = m_rc->AcquireTemp(copy_size);
    if (!copy) return;
    m_rc->UnbindSrvs();
    const D3D11_BOX box{static_cast<UINT>(bx0), static_cast<UINT>(by0), 0, static_cast<UINT>(bx1), static_cast<UINT>(by1), 1};
    m_rc->Ctx()->CopySubresourceRegion(copy->tex.Get(), 0, 0, 0, 0, m_target_res, 0, &box);
    const RectF region{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0), static_cast<float>(y1 - y0)};

    if (blur && a.pixelate) {
        effects::Pixelate(*m_rc, copy->srv.Get(), copy_size, std::max(1, static_cast<int>(std::lround(a.blur_px * zoom))), region, rtv,
                          size, radius, alpha);
        m_rc->ReleaseTemp(copy);
        return;
    }
    ID3D11ShaderResourceView* src = copy->srv.Get();
    RenderContext::TempRT* blurred = nullptr;
    if (blur) {
        blurred = m_rc->AcquireTemp(copy_size);
        if (!blurred) {
            m_rc->ReleaseTemp(copy);
            return;
        }
        effects::GaussianBlur(*m_rc, copy->srv.Get(), copy_size, static_cast<float>(a.blur_px) * 0.5f * zoom, *blurred);
        src = blurred->srv.Get();
    }
    m_rc->SetFullscreen(rtv, size);
    m_rc->SetBlendPremul();
    QuadCb cb;
    ZeroCb(cb);
    BuildScreenQuad(cb, region, SizeI{});
    const float iw = 1.f / static_cast<float>(copy_size.w), ih = 1.f / static_cast<float>(copy_size.h);
    for (int i = 0; i < 4; ++i) {
        cb.uv[i][0] = (static_cast<float>(x0 - bx0) + ((i & 1) ? region.w : 0.f)) * iw;
        cb.uv[i][1] = (static_cast<float>(y0 - by0) + ((i >> 1) ? region.h : 0.f)) * ih;
    }
    cb.radius = radius;
    cb.mask = 1.f;
    cb.opacity = alpha;
    cb.premul = 1.f;
    if (blur) {
        DrawQuad(*m_rc, Ps::Rgba, cb, src);
    } else {   // Highlight: multiply by the fill colour
        SetColor(cb.color, a.fill);
        DrawQuad(*m_rc, Ps::Multiply, cb, src);
    }
    if (blurred) m_rc->ReleaseTemp(blurred);
    m_rc->ReleaseTemp(copy);
}

void Compositor::DrawAnnotations(const Project& p, TimeUs t, const Mat3& cam, float zoom, ID3D11RenderTargetView* rtv, SizeI size) {
    const float cw = static_cast<float>(size.w), ch = static_cast<float>(size.h);
    for (const AnnItem& item : m_ann) {
        const Clip& c = *item.clip;
        const Annotation& a = *c.annotation;
        // Entrance / exit animation.
        const float anim_us = static_cast<float>(std::max(a.anim_ms, 1)) * 1000.f;
        const float u_in = std::clamp(static_cast<float>(t - c.start_us) / anim_us, 0.f, 1.f);
        const float u_out = std::clamp(static_cast<float>(c.End() - t) / anim_us, 0.f, 1.f);
        float alpha = std::clamp(layer_math::PropAt(c, "transform.opacity", t, c.transform.opacity), 0.f, 1.f);
        float scale = 1.f, dy = 0.f;
        auto apply = [&](Annotation::Anim anim, float u) {
            if (anim == Annotation::Anim::None || u >= 1.f) return;
            const float e = EaseOut(u);
            switch (anim) {
            case Annotation::Anim::Fade: alpha *= e; break;
            case Annotation::Anim::Pop:
                alpha *= e;
                scale *= 0.8f + 0.2f * e;
                break;
            case Annotation::Anim::Slide:
                alpha *= e;
                dy += 24.f * (1.f - e);
                break;
            default: break;
            }
        };
        apply(a.anim_in, u_in);
        apply(a.anim_out, u_out);
        if (alpha <= 0.f) continue;
        const float ox = layer_math::PropAt(c, "transform.x", t, c.transform.x) * cw;
        const float oy = layer_math::PropAt(c, "transform.y", t, c.transform.y) * ch;
        if (!item.layer) {
            DrawRegionEffect(c, a, t, cam, zoom, alpha, rtv, size);
            continue;
        }
        RectF r = item.layer->canvas_rect;
        if (a.type == Annotation::Type::Keystrokes) {   // badge centred in the annotation rect
            const float ax = (a.rect.x + a.rect.w * 0.5f) * cw, ay = (a.rect.y + a.rect.h * 0.5f) * ch;
            r.x = std::round(ax - r.w * 0.5f);
            r.y = std::round(ay - r.h * 0.5f);
        }
        const float acx = (a.rect.x + a.rect.w * 0.5f) * cw, acy = (a.rect.y + a.rect.h * 0.5f) * ch;
        const Mat3 anim = Mat3::Translate(acx + ox, acy + oy + dy) * Mat3::Scale(scale, scale) * Mat3::Translate(-acx, -acy);
        const Mat3 m = cam * anim;
        m_rc->SetFullscreen(rtv, size);
        m_rc->SetBlendPremul();
        QuadCb cb;
        ZeroCb(cb);
        PointF uv[4];
        FullUv(uv);
        BuildRectQuad(cb, m, r, 0.f, uv);
        cb.premul = 1.f;
        cb.opacity = alpha;
        DrawQuad(*m_rc, Ps::Rgba, cb, item.layer->srv.Get());
    }
    (void)p;
}

HRESULT Compositor::RenderFrame(const Project& p, TimeUs t, const FrameInputs& in, ID3D11RenderTargetView* target,
                                const RenderOptions& opt) {
    if (!m_rc || !target) return E_UNEXPECTED;
    const int64_t t0 = Qpc();
    m_timing.background_ms = m_timing.layers_ms = m_timing.cursor_ms = m_timing.annotations_ms = m_timing.total_ms = 0;
    const SizeI size{p.canvas.width, p.canvas.height};
    {
        ComPtr<ID3D11Resource> res;
        target->GetResource(res.GetAddressOf());
        ComPtr<ID3D11Texture2D> tex;
        if (!res || FAILED(res.As(&tex))) return E_INVALIDARG;
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (static_cast<int>(d.Width) != size.w || static_cast<int>(d.Height) != size.h) return E_INVALIDARG;
        m_target_res = res.Get();   // the caller keeps the target alive for the frame
    }
    ID3D11DeviceContext* ctx = m_rc->Ctx();
    m_rc->BeginFrame();
    m_text.SetDeterministic(opt.deterministic_text);
    if (opt.gpu_timing) {
        m_rc->Timer(0).Begin(ctx);
        m_rc->Timer(1).Begin(ctx);
    }

    // 1. Camera.
    const project::Camera::View view = p.camera.At(t);
    const Mat3 cam = layer_math::CameraTransform(view, p.canvas);
    const float zoom = view.zoom > 0.f ? view.zoom : 1.f;

    // 0. Text rasterization (Direct2D) before any D3D pass of this frame.
    PrepareText(p, in, opt, t, zoom);

    // 2. Background.
    DrawBackground(p, in, opt, target, size);
    const int64_t t1 = Qpc();
    if (opt.gpu_timing) {
        m_rc->Timer(1).End(ctx);
        m_rc->Timer(2).Begin(ctx);
    }

    // 3. Layers, bottom to top.
    m_done.clear();
    for (const VideoLayerInput& L : in.layers) {
        if (!L.clip || TrackHidden(p, L.clip->track)) continue;
        if (std::find(m_done.begin(), m_done.end(), L.clip) != m_done.end()) continue;
        const project::Transition* tr = nullptr;
        for (const auto& at : in.active_transitions)
            if (at.second && (at.second->a == L.clip->id || at.second->b == L.clip->id)) {
                tr = at.second;
                break;
            }
        float u = 0;
        if (tr && TransitionProgress(p, *tr, t, u)) {
            const VideoLayerInput* la = nullptr;
            const VideoLayerInput* lb = nullptr;
            for (const VideoLayerInput& o : in.layers) {
                if (!o.clip) continue;
                if (o.clip->id == tr->a) la = &o;
                if (o.clip->id == tr->b) lb = &o;
            }
            RenderContext::TempRT* ta = m_rc->AcquireTemp(size);
            RenderContext::TempRT* tb = m_rc->AcquireTemp(size);
            if (ta && tb) {
                const float zero[4] = {};
                m_rc->UnbindSrvs();
                ctx->ClearRenderTargetView(ta->rtv.Get(), zero);
                ctx->ClearRenderTargetView(tb->rtv.Get(), zero);
                if (la) DrawLayer(p, *la, t, cam, ta->rtv.Get(), size);
                if (lb) DrawLayer(p, *lb, t, cam, tb->rtv.Get(), size);
                transitions::Apply(*m_rc, *tr, u, ta->srv.Get(), tb->srv.Get(), target, size);
            }
            m_rc->ReleaseTemp(ta);
            m_rc->ReleaseTemp(tb);
            if (la) m_done.push_back(la->clip);
            if (lb) m_done.push_back(lb->clip);
            continue;
        }
        if (!ClipActive(*L.clip, t)) continue;
        DrawLayer(p, L, t, cam, target, size);
    }
    const int64_t t2 = Qpc();
    if (opt.gpu_timing) {
        m_rc->Timer(2).End(ctx);
        m_rc->Timer(3).Begin(ctx);
    }

    // 4./5. Cursor and keystroke badge.
    DrawCursorAndBadges(p, in, opt, t, cam, target, size);
    const int64_t t3 = Qpc();

    // 6. Annotations.
    DrawAnnotations(p, t, cam, zoom, target, size);
    const int64_t t4 = Qpc();

    m_rc->UnbindSrvs();
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_target_res = nullptr;
    if (opt.gpu_timing) {
        m_rc->Timer(3).End(ctx);
        m_rc->Timer(0).End(ctx);
        double ms = 0;
        if (m_rc->Timer(0).Resolve(ctx, ms)) m_timing.gpu_total_ms = ms;
        if (m_rc->Timer(1).Resolve(ctx, ms)) m_timing.gpu_background_ms = ms;
        if (m_rc->Timer(2).Resolve(ctx, ms)) m_timing.gpu_layers_ms = ms;
        if (m_rc->Timer(3).Resolve(ctx, ms)) m_timing.gpu_overlay_ms = ms;
    }
    m_timing.background_ms = QpcMs(t1 - t0);
    m_timing.layers_ms = QpcMs(t2 - t1);
    m_timing.cursor_ms = QpcMs(t3 - t2);
    m_timing.annotations_ms = QpcMs(t4 - t3);
    m_timing.total_ms = QpcMs(Qpc() - t0);
    return S_OK;
}

} // namespace ct::render
