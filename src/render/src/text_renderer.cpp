#define CT_LOG_CATEGORY "render"
#include <ct/render/text_renderer.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
using ct::project::PointF;

namespace ct::render {

namespace {

struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void Add(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void F(float v) { Add(&v, sizeof(v)); }
    void I(int64_t v) { Add(&v, sizeof(v)); }
    void S(const std::string& s) {
        I(static_cast<int64_t>(s.size()));
        Add(s.data(), s.size());
    }
    void C(const ColorF& c) {
        F(c.r);
        F(c.g);
        F(c.b);
        F(c.a);
    }
};

D2D1_COLOR_F D2dColor(const ColorF& c, float alpha_mul = 1.f) { return D2D1_COLOR_F{c.r, c.g, c.b, c.a * alpha_mul}; }
D2D1_COLOR_F Hex(uint32_t rgb, float a) {
    return D2D1_COLOR_F{static_cast<float>((rgb >> 16) & 0xff) / 255.f, static_cast<float>((rgb >> 8) & 0xff) / 255.f,
                        static_cast<float>(rgb & 0xff) / 255.f, a};
}

// docs/05 tokens used by the badges.
constexpr uint32_t kBg0 = 0x0F1115, kBg3 = 0x262B36, kBorder = 0x2C323E, kText = 0xE6E8EE;

RectF Normalized(RectF r) {
    if (r.w < 0) {
        r.x += r.w;
        r.w = -r.w;
    }
    if (r.h < 0) {
        r.y += r.h;
        r.h = -r.h;
    }
    return r;
}

RectF Inflate(RectF r, float d) { return RectF{r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d}; }

RectF Union(RectF a, float px, float py) {
    const float x0 = std::min(a.x, px), y0 = std::min(a.y, py);
    const float x1 = std::max(a.x + a.w, px), y1 = std::max(a.y + a.h, py);
    return RectF{x0, y0, x1 - x0, y1 - y0};
}

SizeI TexSize(RectF r, float scale) {
    const int w = std::clamp(static_cast<int>(std::ceil(r.w * scale)), 1, 8192);
    const int h = std::clamp(static_cast<int>(std::ceil(r.h * scale)), 1, 8192);
    return SizeI{w, h};
}

} // namespace

TextRenderer::~TextRenderer() { Shutdown(); }

HRESULT TextRenderer::Init(RenderContext& rc) {
    Shutdown();
    if (!rc.D2D() || !rc.DWrite()) return E_UNEXPECTED;
    m_rc = &rc;
    // Fixed rendering parameters: goldens must not depend on the user's ClearType tuning.
    CT_RETURN_IF_FAILED(rc.DWrite()->CreateCustomRenderingParams(1.8f, 0.5f, 0.f, DWRITE_PIXEL_GEOMETRY_FLAT,
                                                                 DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, m_params.GetAddressOf()));
    return S_OK;
}

void TextRenderer::Shutdown() {
    m_entries.clear();
    m_bytes = 0;
    m_params.Reset();
    m_rc = nullptr;
}

void TextRenderer::Trim(size_t max_bytes) {
    while (m_bytes > max_bytes && !m_entries.empty()) {
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (it->second->last_use < oldest->second->last_use) oldest = it;
        m_bytes -= Bytes(*oldest->second);
        m_entries.erase(oldest);
    }
}

void TextRenderer::EndFrame(uint32_t max_idle_frames, size_t max_bytes) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (m_frame - it->second->last_frame > max_idle_frames) {
            m_bytes -= Bytes(*it->second);
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    Trim(max_bytes);
    ++m_frame;
}

TextRenderer::Entry* TextRenderer::Find(uint64_t key) {
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return nullptr;
    Entry* e = it->second.get();
    e->last_use = ++m_tick;
    e->last_frame = m_frame;
    return e;
}

HRESULT TextRenderer::NewEntry(uint64_t key, SizeI size, Entry*& out) {
    ID3D11Device* d = m_rc->Device().Device();
    auto e = std::make_unique<Entry>();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(size.w);
    td.Height = static_cast<UINT>(size.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CT_RETURN_IF_FAILED(d->CreateTexture2D(&td, nullptr, e->tex.GetAddressOf()));
    CT_RETURN_IF_FAILED(d->CreateShaderResourceView(e->tex.Get(), nullptr, e->layer.srv.GetAddressOf()));
    ComPtr<IDXGISurface> surface;
    CT_RETURN_IF_FAILED(e->tex.As(&surface));
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat = D2D1_PIXEL_FORMAT{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
    bp.dpiX = bp.dpiY = 96.f;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    CT_RETURN_IF_FAILED(m_rc->D2D()->CreateBitmapFromDxgiSurface(surface.Get(), &bp, e->bitmap.GetAddressOf()));
    e->key = key;
    e->layer.size = size;
    e->last_use = ++m_tick;
    e->last_frame = m_frame;
    out = e.get();
    m_bytes += Bytes(*e);
    m_entries[key] = std::move(e);
    ++m_rasterizations;
    return S_OK;
}

void TextRenderer::Drop(uint64_t key) {
    const auto it = m_entries.find(key);
    if (it == m_entries.end()) return;
    m_bytes -= Bytes(*it->second);
    m_entries.erase(it);
}

HRESULT TextRenderer::BeginDraw(Entry& e) {
    ID2D1DeviceContext* dc = m_rc->D2D();
    m_rc->UnbindSrvs();   // the target must not be bound as a D3D SRV while D2D draws into it
    dc->SetTarget(e.bitmap.Get());
    dc->BeginDraw();
    dc->SetTextAntialiasMode(m_grayscale ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
    dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (m_params) dc->SetTextRenderingParams(m_params.Get());
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    dc->Clear(D2D1_COLOR_F{0, 0, 0, 0});
    return S_OK;
}

HRESULT TextRenderer::EndDraw() {
    ID2D1DeviceContext* dc = m_rc->D2D();
    const HRESULT hr = dc->EndDraw();   // flushes D2D's work before D3D samples the texture
    dc->SetTarget(nullptr);
    if (FAILED(hr)) ::ct::LogHr(hr, "ID2D1DeviceContext::EndDraw", __FILE__, __LINE__);
    return hr;
}

const wchar_t* TextRenderer::ResolveFont(const std::string& requested) {
    for (const FontName& f : m_fonts)
        if (f.requested == requested) return f.resolved.c_str();
    FontName f;
    f.requested = requested;
    f.resolved = Utf8ToWide(requested);
    ComPtr<IDWriteFontCollection> coll;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (m_rc && SUCCEEDED(m_rc->DWrite()->GetSystemFontCollection(coll.GetAddressOf(), FALSE)))
        coll->FindFamilyName(f.resolved.c_str(), &index, &exists);
    if (!exists || f.resolved.empty()) {
        CT_LOG_WARN("font '%s' is not installed; using Segoe UI", requested.c_str());
        f.resolved = L"Segoe UI";
    }
    m_fonts.push_back(std::move(f));
    return m_fonts.back().resolved.c_str();
}

HRESULT TextRenderer::MakeFormat(const std::string& font, float size_px, int weight, IDWriteTextFormat** out) {
    const wchar_t* family = ResolveFont(font);
    const DWRITE_FONT_WEIGHT w = static_cast<DWRITE_FONT_WEIGHT>(std::clamp(weight, 1, 999));
    HRESULT hr = m_rc->DWrite()->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                  std::max(size_px, 1.f), L"en-us", out);
    if (FAILED(hr))
        hr = m_rc->DWrite()->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              std::max(size_px, 1.f), L"en-us", out);
    return hr;
}

HRESULT TextRenderer::Annotation(const project::Annotation& a, SizeI canvas, float scale, const Layer*& out) {
    out = nullptr;
    using T = project::Annotation::Type;
    if (!m_rc) return E_UNEXPECTED;
    if (a.type == T::Highlight || a.type == T::Blur || a.type == T::Keystrokes) return E_INVALIDARG;
    scale = std::clamp(scale, 0.25f, 8.f);
    Hasher hs;
    hs.I(1);
    hs.I(static_cast<int>(a.type));
    hs.S(a.text);
    hs.S(a.font);
    hs.I(a.size_px);
    hs.I(a.weight);
    hs.C(a.color);
    hs.C(a.fill);
    hs.C(a.stroke);
    hs.I(a.stroke_px);
    hs.I(a.radius_px);
    hs.F(a.rect.x);
    hs.F(a.rect.y);
    hs.F(a.rect.w);
    hs.F(a.rect.h);
    hs.F(a.pointer.x);
    hs.F(a.pointer.y);
    hs.F(a.arrow_head_px);
    hs.I(canvas.w);
    hs.I(canvas.h);
    hs.F(scale);
    hs.I(m_grayscale ? 1 : 0);
    if (Entry* e = Find(hs.h)) {
        out = &e->layer;
        return S_OK;
    }

    const float cw = static_cast<float>(canvas.w), ch = static_cast<float>(canvas.h);
    const RectF raw{a.rect.x * cw, a.rect.y * ch, a.rect.w * cw, a.rect.h * ch};
    const RectF r = Normalized(raw);
    const float stroke = static_cast<float>(std::max(a.stroke_px, 0));
    const PointF ptr{a.pointer.x * cw, a.pointer.y * ch};
    const PointF tail{raw.x, raw.y}, head{raw.x + raw.w, raw.y + raw.h};
    const float head_len = std::max(a.arrow_head_px, 4.f);
    const float arrow_w = stroke > 0 ? stroke : std::max(4.f, head_len * 0.3f);
    RectF bounds;
    switch (a.type) {
    case T::Callout: bounds = Inflate(Union(r, ptr.x, ptr.y), stroke + 2.f); break;
    case T::Arrow: bounds = Inflate(r, head_len + arrow_w); break;
    default: bounds = Inflate(r, stroke * 0.5f + 2.f); break;
    }
    const SizeI size = TexSize(bounds, scale);

    Entry* e = nullptr;
    CT_RETURN_IF_FAILED(NewEntry(hs.h, size, e));
    e->layer.canvas_rect = RectF{bounds.x, bounds.y, static_cast<float>(size.w) / scale, static_cast<float>(size.h) / scale};

    ID2D1DeviceContext* dc = m_rc->D2D();
    ID2D1Factory1* f = m_rc->D2DFactory();
    BeginDraw(*e);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.x, -bounds.y) * D2D1::Matrix3x2F::Scale(scale, scale));
    ComPtr<ID2D1SolidColorBrush> fill, line, text;
    dc->CreateSolidColorBrush(D2dColor(a.fill), fill.GetAddressOf());
    dc->CreateSolidColorBrush(D2dColor(a.stroke), line.GetAddressOf());
    dc->CreateSolidColorBrush(D2dColor(a.color), text.GetAddressOf());
    const D2D1_RECT_F rr{r.x, r.y, r.x + r.w, r.y + r.h};
    const float radius = std::min(static_cast<float>(std::max(a.radius_px, 0)), std::min(r.w, r.h) * 0.5f);
    bool draw_text = !a.text.empty();
    float text_pad = 0;

    switch (a.type) {
    case T::Callout: {
        ComPtr<ID2D1RoundedRectangleGeometry> body;
        f->CreateRoundedRectangleGeometry(D2D1_ROUNDED_RECT{rr, radius, radius}, body.GetAddressOf());
        ComPtr<ID2D1Geometry> shape = body;
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        const bool inside = ptr.x >= r.x && ptr.x <= r.x + r.w && ptr.y >= r.y && ptr.y <= r.y + r.h;
        if (!inside && body) {
            float dx = ptr.x - cx, dy = ptr.y - cy;
            const float len = std::sqrt(dx * dx + dy * dy);
            dx /= len;
            dy /= len;
            const float base = std::clamp(std::min(r.w, r.h) * 0.35f, 12.f, 48.f) * 0.5f;
            ComPtr<ID2D1PathGeometry> tri;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(f->CreatePathGeometry(tri.GetAddressOf())) && SUCCEEDED(tri->Open(sink.GetAddressOf()))) {
                sink->BeginFigure(D2D1_POINT_2F{cx - dy * base, cy + dx * base}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1_POINT_2F{ptr.x, ptr.y});
                sink->AddLine(D2D1_POINT_2F{cx + dy * base, cy - dx * base});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                ComPtr<ID2D1PathGeometry> merged;
                ComPtr<ID2D1GeometrySink> msink;
                if (SUCCEEDED(f->CreatePathGeometry(merged.GetAddressOf())) && SUCCEEDED(merged->Open(msink.GetAddressOf()))) {
                    body->CombineWithGeometry(tri.Get(), D2D1_COMBINE_MODE_UNION, nullptr, msink.Get());
                    msink->Close();
                    shape = merged;
                }
            }
        }
        if (shape) {
            if (a.fill.a > 0) dc->FillGeometry(shape.Get(), fill.Get());
            if (stroke > 0) dc->DrawGeometry(shape.Get(), line.Get(), stroke);
        }
        text_pad = std::min(r.h * 0.1f, 16.f) + radius * 0.3f;
        break;
    }
    case T::Arrow: {
        float dx = head.x - tail.x, dy = head.y - tail.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.5f) {
            dx /= len;
            dy /= len;
            const float hl = std::min(head_len, len);
            const PointF base{head.x - dx * hl, head.y - dy * hl};
            ComPtr<ID2D1StrokeStyle> style;
            D2D1_STROKE_STYLE_PROPERTIES sp{};
            sp.startCap = D2D1_CAP_STYLE_ROUND;
            sp.endCap = D2D1_CAP_STYLE_FLAT;
            sp.lineJoin = D2D1_LINE_JOIN_ROUND;
            f->CreateStrokeStyle(&sp, nullptr, 0, style.GetAddressOf());
            // The shaft ends inside the head so no gap shows between them.
            const PointF shaft_end{base.x + dx * hl * 0.3f, base.y + dy * hl * 0.3f};
            dc->DrawLine(D2D1_POINT_2F{tail.x, tail.y}, D2D1_POINT_2F{shaft_end.x, shaft_end.y}, fill.Get(), arrow_w, style.Get());
            const float hw = std::max(hl * 0.45f, arrow_w);
            ComPtr<ID2D1PathGeometry> tri;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(f->CreatePathGeometry(tri.GetAddressOf())) && SUCCEEDED(tri->Open(sink.GetAddressOf()))) {
                sink->BeginFigure(D2D1_POINT_2F{head.x, head.y}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1_POINT_2F{base.x - dy * hw, base.y + dx * hw});
                sink->AddLine(D2D1_POINT_2F{base.x + dy * hw, base.y - dx * hw});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(tri.Get(), fill.Get());
            }
        }
        draw_text = false;
        break;
    }
    case T::Rect: {
        const D2D1_ROUNDED_RECT rrect{rr, radius, radius};
        if (a.fill.a > 0) dc->FillRoundedRectangle(rrect, fill.Get());
        if (stroke > 0) dc->DrawRoundedRectangle(rrect, line.Get(), stroke);
        text_pad = stroke + 4.f;
        break;
    }
    case T::Ellipse: {
        const D2D1_ELLIPSE el{D2D1_POINT_2F{r.x + r.w * 0.5f, r.y + r.h * 0.5f}, r.w * 0.5f, r.h * 0.5f};
        if (a.fill.a > 0) dc->FillEllipse(el, fill.Get());
        if (stroke > 0) dc->DrawEllipse(el, line.Get(), stroke);
        text_pad = std::min(r.w, r.h) * 0.15f;
        break;
    }
    default: break;   // Text: glyphs only
    }

    if (draw_text) {
        ComPtr<IDWriteTextFormat> fmt;
        if (SUCCEEDED(MakeFormat(a.font, static_cast<float>(a.size_px), a.weight, fmt.GetAddressOf()))) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            m_text = Utf8ToWide(a.text);
            const D2D1_RECT_F tr{rr.left + text_pad, rr.top, rr.right - text_pad, rr.bottom};
            dc->DrawText(m_text.c_str(), static_cast<UINT32>(m_text.size()), fmt.Get(), tr, text.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    const HRESULT hr = EndDraw();
    if (FAILED(hr)) {
        Drop(hs.h);
        return hr;
    }
    out = &e->layer;
    return S_OK;
}

HRESULT TextRenderer::KeystrokeBadge(const std::vector<std::string>& keys, const project::KeystrokeStyle& style, SizeI canvas,
                                     float scale, const Layer*& out, float font_px) {
    out = nullptr;
    if (!m_rc) return E_UNEXPECTED;
    if (keys.empty()) return E_INVALIDARG;
    scale = std::clamp(scale, 0.25f, 8.f);
    if (font_px <= 0) font_px = std::max(12.f, std::round(static_cast<float>(canvas.h) * 0.032f));
    Hasher hs;
    hs.I(2);
    hs.I(static_cast<int>(keys.size()));
    for (const std::string& k : keys) hs.S(k);
    hs.I(static_cast<int>(style.style));
    hs.I(canvas.w);
    hs.I(canvas.h);
    hs.F(scale);
    hs.F(font_px);
    hs.I(m_grayscale ? 1 : 0);
    if (Entry* e = Find(hs.h)) {
        out = &e->layer;
        return S_OK;
    }

    ComPtr<IDWriteTextFormat> fmt;
    CT_RETURN_IF_FAILED(MakeFormat("Segoe UI", font_px, 600, fmt.GetAddressOf()));
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteFactory3* dw = m_rc->DWrite();
    auto measure = [&](const std::wstring& s, float& w, float& h) {
        ComPtr<IDWriteTextLayout> layout;
        w = 0;
        h = font_px * 1.33f;
        if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt.Get(), 8192.f, 1024.f, layout.GetAddressOf()))) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(layout->GetMetrics(&tm))) {
                w = tm.widthIncludingTrailingWhitespace;
                h = tm.height;
            }
        }
    };

    const bool pill = style.style == project::KeystrokeStyle::Style::Pill;
    // Layout in badge pixels (before scale).
    struct Cap {
        std::wstring text;
        float x, w;
    };
    std::vector<Cap> caps;
    float total_w = 0, line_h = font_px * 1.33f;
    const float pad_x = font_px * 0.8f, pad_y = font_px * 0.45f;
    if (pill) {
        std::wstring all;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) all += L"   ";
            all += Utf8ToWide(keys[i]);
        }
        float w = 0;
        measure(all, w, line_h);
        caps.push_back(Cap{all, 0, w + 2 * pad_x});
        total_w = w + 2 * pad_x;
    } else {
        const float cap_pad = font_px * 0.5f, key_gap = font_px * 0.25f, stroke_gap = font_px * 0.8f;
        float x = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i) x += stroke_gap - key_gap;
            size_t start = 0;
            const std::string& s = keys[i];
            while (start <= s.size()) {
                size_t plus = s.find('+', start);
                if (plus == start && plus + 1 == s.size()) plus = std::string::npos;   // the key itself is '+'
                const std::string part = s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
                if (!part.empty()) {
                    Cap c{Utf8ToWide(part), x, 0};
                    float w = 0;
                    measure(c.text, w, line_h);
                    c.w = std::max(w + 2 * cap_pad, line_h + pad_y);
                    caps.push_back(c);
                    x += c.w + key_gap;
                }
                if (plus == std::string::npos) break;
                start = plus + 1;
            }
        }
        total_w = std::max(x - key_gap, 1.f);
    }
    const float h = line_h + 2 * pad_y;
    const float shade = pill ? 0.f : 2.f;
    const RectF bounds{0, 0, total_w + 2.f, h + shade + 2.f};
    const SizeI size = TexSize(bounds, scale);
    Entry* e = nullptr;
    CT_RETURN_IF_FAILED(NewEntry(hs.h, size, e));
    e->layer.canvas_rect = RectF{0, 0, static_cast<float>(size.w) / scale, static_cast<float>(size.h) / scale};

    ID2D1DeviceContext* dc = m_rc->D2D();
    BeginDraw(*e);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(1.f, 1.f) * D2D1::Matrix3x2F::Scale(scale, scale));
    ComPtr<ID2D1SolidColorBrush> bg, border, text, shade_brush;
    dc->CreateSolidColorBrush(Hex(pill ? kBg0 : kBg3, pill ? 0.85f : 0.95f), bg.GetAddressOf());
    dc->CreateSolidColorBrush(Hex(kBorder, 1.f), border.GetAddressOf());
    dc->CreateSolidColorBrush(Hex(kText, 1.f), text.GetAddressOf());
    dc->CreateSolidColorBrush(Hex(kBg0, 0.95f), shade_brush.GetAddressOf());
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (const Cap& c : caps) {
        const D2D1_RECT_F cr{c.x, 0, c.x + c.w, h};
        const float rad = pill ? h * 0.5f : std::min(6.f * font_px / 24.f + 2.f, h * 0.3f);
        if (!pill) {
            const D2D1_RECT_F sr{cr.left, cr.top + shade, cr.right, cr.bottom + shade};
            dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{sr, rad, rad}, shade_brush.Get());
        }
        dc->FillRoundedRectangle(D2D1_ROUNDED_RECT{cr, rad, rad}, bg.Get());
        const D2D1_RECT_F br{cr.left + 0.5f, cr.top + 0.5f, cr.right - 0.5f, cr.bottom - 0.5f};
        dc->DrawRoundedRectangle(D2D1_ROUNDED_RECT{br, rad - 0.5f, rad - 0.5f}, border.Get(), 1.f);
        dc->DrawText(c.text.c_str(), static_cast<UINT32>(c.text.size()), fmt.Get(), cr, text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    const HRESULT hr = EndDraw();
    if (FAILED(hr)) {
        Drop(hs.h);
        return hr;
    }
    out = &e->layer;
    return S_OK;
}

} // namespace ct::render
