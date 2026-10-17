#define CT_LOG_CATEGORY "render"
#include <ct/render/cursor_renderer.h>

#include <ct/core/error.h>
#include <ct/core/log.h>

#include "render_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace ct::render {

namespace {

constexpr TimeUs kSampleStepUs = 8'333;     // 120 Hz, the recorder's cursor rate
constexpr TimeUs kClickSnapUs = 60'000;
constexpr float kSpotlightRadius = 220.f;

float EaseOut(float u) {
    const float v = 1.f - std::clamp(u, 0.f, 1.f);
    return 1.f - v * v * v;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// ShelfPacker

ShelfPacker::ShelfPacker(int width, int height, int padding) : m_w(width), m_h(height), m_pad(padding) {}

void ShelfPacker::Reset() {
    m_shelf_y = 0;
    m_shelf_h = 0;
    m_x = 0;
}

bool ShelfPacker::Insert(int w, int h, int& x, int& y) {
    const int pw = w + 2 * m_pad, ph = h + 2 * m_pad;
    if (w <= 0 || h <= 0 || pw > m_w || ph > m_h) return false;
    if (m_x + pw > m_w) {   // next shelf
        m_shelf_y += m_shelf_h;
        m_shelf_h = 0;
        m_x = 0;
    }
    if (m_shelf_y + ph > m_h) return false;
    x = m_x + m_pad;
    y = m_shelf_y + m_pad;
    m_x += pw;
    m_shelf_h = std::max(m_shelf_h, ph);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Built-in arrow

void CursorRenderer::MakeArrowShape(std::vector<uint8_t>& bgra, int& w, int& h, int& hot_x, int& hot_y) {
    // Classic arrow outline (tip at the origin), in base pixels; scaled 1.5x and offset by the outline width.
    static const float kPoly[][2] = {{0, 0}, {0, 16}, {4, 12.5f}, {6.8f, 19}, {9.3f, 18}, {6.6f, 11.6f}, {11.5f, 11.6f}};
    constexpr int kN = 7;
    constexpr float kScale = 1.5f, kOutline = 1.5f, kOff = 2.f;
    constexpr int kSs = 4;   // supersampling per axis
    w = 24;
    h = 34;
    hot_x = static_cast<int>(kOff);
    hot_y = static_cast<int>(kOff);
    auto inside = [&](float px, float py) {
        bool in = false;
        for (int i = 0, j = kN - 1; i < kN; j = i++) {
            const float xi = kPoly[i][0] * kScale + kOff, yi = kPoly[i][1] * kScale + kOff;
            const float xj = kPoly[j][0] * kScale + kOff, yj = kPoly[j][1] * kScale + kOff;
            if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
        }
        return in;
    };
    // Distance from a point to the polygon edges (for the outline).
    auto edge_dist = [&](float px, float py) {
        float best = 1e9f;
        for (int i = 0, j = kN - 1; i < kN; j = i++) {
            const float ax = kPoly[j][0] * kScale + kOff, ay = kPoly[j][1] * kScale + kOff;
            const float bx = kPoly[i][0] * kScale + kOff, by = kPoly[i][1] * kScale + kOff;
            const float dx = bx - ax, dy = by - ay;
            const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
            const float ex = ax + t * dx - px, ey = ay + t * dy - py;
            best = std::min(best, std::sqrt(ex * ex + ey * ey));
        }
        return best;
    };
    bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float white = 0, black = 0;
            for (int sy = 0; sy < kSs; ++sy)
                for (int sx = 0; sx < kSs; ++sx) {
                    const float px = x + (sx + 0.5f) / kSs, py = y + (sy + 0.5f) / kSs;
                    const bool in = inside(px, py);
                    const float d = edge_dist(px, py);
                    if (in && d > kOutline * 0.5f) white += 1;
                    else if (in || d <= kOutline * 0.5f) black += 1;
                }
            const float n = static_cast<float>(kSs * kSs);
            const float a = (white + black) / n;
            const float c = white / n;   // premultiplied white
            uint8_t* p = &bgra[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = p[1] = p[2] = static_cast<uint8_t>(std::lround(c * 255.f));
            p[3] = static_cast<uint8_t>(std::lround(a * 255.f));
        }
}

// ---------------------------------------------------------------------------------------------------------------------
// CursorRenderer

CursorRenderer::~CursorRenderer() { Shutdown(); }

HRESULT CursorRenderer::Init(RenderContext& rc) {
    Shutdown();
    m_rc = &rc;
    for (auto& p : m_packers) p = ShelfPacker(kAtlasSize, kAtlasSize, 2);
    m_slots.reserve(64);
    m_downs.reserve(64);
    CT_RETURN_IF_FAILED(EnsureAtlas(0));
    return S_OK;
}

void CursorRenderer::Shutdown() {
    for (auto& a : m_atlas) a.Reset();
    for (auto& s : m_atlas_srv) s.Reset();
    m_atlas_count = 0;
    m_slots.clear();
    m_prepared_log = nullptr;
    m_prepared_shapes = 0;
    m_rc = nullptr;
}

HRESULT CursorRenderer::EnsureAtlas(int index) {
    if (index < m_atlas_count) return S_OK;
    if (index >= kMaxAtlases) return E_OUTOFMEMORY;
    ID3D11Device* d = m_rc->Device().Device();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = kAtlasSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    CT_RETURN_IF_FAILED(d->CreateTexture2D(&td, nullptr, m_atlas[index].GetAddressOf()));
    CT_RETURN_IF_FAILED(d->CreateShaderResourceView(m_atlas[index].Get(), nullptr, m_atlas_srv[index].GetAddressOf()));
    ComPtr<ID3D11RenderTargetView> rtv;   // only to clear the padding to transparent
    CT_RETURN_IF_FAILED(d->CreateRenderTargetView(m_atlas[index].Get(), nullptr, rtv.GetAddressOf()));
    const float zero[4] = {};
    m_rc->Ctx()->ClearRenderTargetView(rtv.Get(), zero);
    m_atlas_count = index + 1;
    return S_OK;
}

const CursorRenderer::Slot* CursorRenderer::FindSlot(uint32_t id) const {
    auto it = std::lower_bound(m_slots.begin(), m_slots.end(), id, [](const Slot& s, uint32_t v) { return s.id < v; });
    return it != m_slots.end() && it->id == id ? &*it : nullptr;
}

bool CursorRenderer::HasShape(uint32_t shape_id) const { return FindSlot(shape_id) != nullptr; }

void CursorRenderer::Prepare(const events::EventLog& log) {
    if (!m_rc) return;
    if (&log == m_prepared_log && log.Shapes().size() == m_prepared_shapes) return;
    m_prepared_log = &log;
    m_prepared_shapes = log.Shapes().size();
    m_slots.clear();
    for (auto& p : m_packers) p.Reset();
    int atlas = 0;
    auto add = [&](uint32_t id, int w, int h, int hx, int hy, const uint8_t* bgra) {
        int x = 0, y = 0;
        while (!m_packers[atlas].Insert(w, h, x, y)) {
            if (atlas + 1 >= kMaxAtlases || FAILED(EnsureAtlas(atlas + 1))) {
                CT_LOG_WARN("cursor atlas full: shape %08x not uploaded", id);
                return;
            }
            ++atlas;
        }
        D3D11_BOX box{static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x + w), static_cast<UINT>(y + h), 1};
        m_rc->Ctx()->UpdateSubresource(m_atlas[atlas].Get(), 0, &box, bgra, static_cast<UINT>(w) * 4, 0);
        ++m_uploads;
        Slot s{id, atlas, x, y, w, h, hx, hy};
        auto it = std::lower_bound(m_slots.begin(), m_slots.end(), id, [](const Slot& a, uint32_t v) { return a.id < v; });
        if (it != m_slots.end() && it->id == id) *it = s;
        else m_slots.insert(it, s);
    };
    std::vector<uint8_t> arrow;
    int aw = 0, ah = 0, ahx = 0, ahy = 0;
    MakeArrowShape(arrow, aw, ah, ahx, ahy);
    add(0, aw, ah, ahx, ahy, arrow.data());
    for (const events::CursorShapeData& sh : log.Shapes())
        if (sh.id != 0 && sh.w > 0 && sh.h > 0 && sh.bgra.size() >= static_cast<size_t>(sh.w) * sh.h * 4)
            add(sh.id, sh.w, sh.h, sh.hot_x, sh.hot_y, sh.bgra.data());
}

PointF CursorRenderer::SmoothedPosition(const events::EventLog& log, TimeUs t, float smoothing) {
    const events::CursorState at = log.CursorAt(t);
    if (!at.valid) return PointF{0, 0};
    PointF p{at.x, at.y};
    if (smoothing > 0.f) {
        const double w = static_cast<double>(smoothing) * 120'000.0;   // half window, us
        const double sigma = w * 0.5;
        const int n = static_cast<int>(w / static_cast<double>(kSampleStepUs));
        double sx = 0, sy = 0, sw = 0;
        for (int k = -n; k <= n; ++k) {
            const TimeUs tk = t + static_cast<TimeUs>(k) * kSampleStepUs;
            const events::CursorState s = log.CursorAt(tk);
            if (!s.valid) continue;
            const double dt = static_cast<double>(k) * static_cast<double>(kSampleStepUs);
            const double wt = std::exp(-0.5 * dt * dt / (sigma * sigma));
            sx += wt * s.x;
            sy += wt * s.y;
            sw += wt;
        }
        if (sw > 0) p = PointF{static_cast<float>(sx / sw), static_cast<float>(sy / sw)};
    }
    // Click snap: ripples appear where the click happened.
    if (const events::ButtonEvent* d = log.LastMouseDown(t + kClickSnapUs, 2 * kClickSnapUs)) {
        const TimeUs dt = d->t_us > t ? d->t_us - t : t - d->t_us;
        if (dt < kClickSnapUs) {
            const float wgt = 1.f - static_cast<float>(dt) / static_cast<float>(kClickSnapUs);
            p.x += (static_cast<float>(d->x) - p.x) * wgt;
            p.y += (static_cast<float>(d->y) - p.y) * wgt;
        }
    }
    return p;
}

void CursorRenderer::Draw(const project::CursorStyle& style, const events::EventLog& log, TimeUs t, const Mat3& m,
                          float eff_scale, ID3D11RenderTargetView* target, SizeI target_size) {
    if (!m_rc || !style.visible || !target) return;
    const events::CursorState st = log.CursorAt(t);
    if (!st.valid || !st.visible) return;
    float alpha = 1.f;
    if (style.hide_when_idle_us > 0) {
        TimeUs last = log.LastCursorChangeAt(t);
        const auto buttons = log.ButtonsBetween(t - style.hide_when_idle_us - kIdleFadeUs, t + 1);
        if (!buttons.empty()) last = std::max(last, buttons.back().t_us);
        if (last >= 0) {
            const TimeUs idle = t - last;
            if (idle > style.hide_when_idle_us)
                alpha = 1.f - std::clamp(static_cast<float>(idle - style.hide_when_idle_us) / static_cast<float>(kIdleFadeUs), 0.f, 1.f);
        }
    }
    if (alpha <= 0.f) return;
    const float layer_scale = style.scale > 0.f ? eff_scale / style.scale : eff_scale;
    const PointF pos = m.Apply(SmoothedPosition(log, t, style.smoothing));

    m_rc->SetFullscreen(target, target_size);
    m_rc->SetBlendPremul();
    QuadCb cb;

    if (style.spotlight) {
        ZeroCb(cb);
        BuildScreenQuad(cb, RectF{0, 0, static_cast<float>(target_size.w), static_cast<float>(target_size.h)}, SizeI{});
        for (auto& c : cb.corner) {
            c[2] = c[0] - pos.x;
            c[3] = c[1] - pos.y;
        }
        cb.p0[0] = kSpotlightRadius * layer_scale;
        cb.p0[1] = 80.f * layer_scale;
        cb.p0[2] = 0.55f;
        cb.opacity = alpha;
        DrawQuad(*m_rc, Ps::Spotlight, cb);
    }

    using Click = project::CursorStyle::Click;
    const bool ripple = style.click == Click::Ripple || style.click == Click::Both;
    const bool highlight = style.click == Click::Highlight || style.click == Click::Both;
    if (highlight) {
        // A disc under the cursor while any button is held (the last event of that button is a down).
        const auto recent = log.ButtonsBetween(t - 60 * kUsPerSecond, t + 1);
        bool held = false;
        for (int b = 0; b < 3 && !held; ++b)
            for (size_t i = recent.size(); i-- > 0;)
                if (static_cast<int>(recent[i].button) == b) {
                    held = recent[i].down;
                    break;
                }
        if (held) {
            const float r = 20.f * eff_scale;
            ZeroCb(cb);
            BuildScreenQuad(cb, RectF{pos.x - r - 2, pos.y - r - 2, 2 * r + 4, 2 * r + 4}, SizeI{});
            cb.p0[0] = r;
            SetColor(cb.color, style.click_color);
            cb.color[3] = 0.35f;
            cb.opacity = alpha;
            DrawQuad(*m_rc, Ps::Disc, cb);
        }
    }
    if (ripple) {
        m_downs.clear();
        log.MouseDownsBetween(t - kClickEffectUs, t + 1, m_downs);
        for (const events::ButtonEvent& d : m_downs) {
            const float u = static_cast<float>(t - d.t_us) / static_cast<float>(kClickEffectUs);
            if (u < 0.f || u > 1.f) continue;
            const PointF c = m.Apply(PointF{static_cast<float>(d.x), static_cast<float>(d.y)});
            const float r = (12.f + 36.f * EaseOut(u)) * eff_scale;
            const float th = 3.f * eff_scale;
            const float ext = r + th + 2.f;
            ZeroCb(cb);
            BuildScreenQuad(cb, RectF{c.x - ext, c.y - ext, 2 * ext, 2 * ext}, SizeI{});
            cb.p0[0] = r;
            cb.p0[1] = th;
            SetColor(cb.color, style.click_color);
            cb.color[3] = 0.7f * (1.f - u);
            cb.opacity = alpha;
            DrawQuad(*m_rc, Ps::Ring, cb);
        }
    }

    const Slot* slot = FindSlot(st.shape_id);
    if (!slot) slot = FindSlot(0);
    if (!slot) return;
    const float inv = 1.f / static_cast<float>(kAtlasSize);
    const RectF quad{pos.x - static_cast<float>(slot->hot_x) * eff_scale, pos.y - static_cast<float>(slot->hot_y) * eff_scale,
                     static_cast<float>(slot->w) * eff_scale, static_cast<float>(slot->h) * eff_scale};
    const float u0 = static_cast<float>(slot->x) * inv, v0 = static_cast<float>(slot->y) * inv;
    const float u1 = static_cast<float>(slot->x + slot->w) * inv, v1 = static_cast<float>(slot->y + slot->h) * inv;
    ID3D11ShaderResourceView* atlas = m_atlas_srv[slot->atlas].Get();
    if (style.shadow) {
        const float off = 2.f * layer_scale;
        const float grow = 2.f * layer_scale;   // room for the blurred edge
        const float gu = grow / eff_scale * inv, gv = grow / eff_scale * inv;
        ZeroCb(cb);
        BuildScreenQuad(cb, RectF{quad.x + off - grow, quad.y + off - grow, quad.w + 2 * grow, quad.h + 2 * grow}, SizeI{});
        const float us[2] = {u0 - gu, u1 + gu}, vs[2] = {v0 - gv, v1 + gv};
        for (int i = 0; i < 4; ++i) {
            cb.uv[i][0] = us[i & 1];
            cb.uv[i][1] = vs[i >> 1];
        }
        cb.p0[0] = 1.5f * inv;
        cb.p0[1] = 1.5f * inv;
        cb.p1[0] = u0 - 2 * inv;
        cb.p1[1] = v0 - 2 * inv;
        cb.p1[2] = u1 + 2 * inv;
        cb.p1[3] = v1 + 2 * inv;
        cb.color[3] = 0.5f;
        cb.opacity = alpha;
        DrawQuad(*m_rc, Ps::CursorShadow, cb, atlas);
    }
    ZeroCb(cb);
    BuildScreenQuad(cb, quad, SizeI{});
    const float us[2] = {u0, u1}, vs[2] = {v0, v1};
    for (int i = 0; i < 4; ++i) {
        cb.uv[i][0] = us[i & 1];
        cb.uv[i][1] = vs[i >> 1];
    }
    cb.premul = 1.f;
    cb.opacity = alpha;
    DrawQuad(*m_rc, Ps::Rgba, cb, atlas);
}

} // namespace ct::render
