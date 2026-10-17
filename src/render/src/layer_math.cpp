#include <ct/render/layer_math.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ct::render {

namespace {
constexpr float kPi = 3.14159265358979f;
}

Mat3 Mat3::Identity() { return Mat3{{1, 0, 0, 0, 1, 0, 0, 0, 1}}; }
Mat3 Mat3::Translate(float tx, float ty) { return Mat3{{1, 0, tx, 0, 1, ty, 0, 0, 1}}; }
Mat3 Mat3::Scale(float sx, float sy) { return Mat3{{sx, 0, 0, 0, sy, 0, 0, 0, 1}}; }

Mat3 Mat3::Rotate(float deg) {
    const float r = deg * kPi / 180.f;
    const float c = std::cos(r), s = std::sin(r);
    // y points down, so the usual counter-clockwise matrix turns clockwise on screen.
    return Mat3{{c, -s, 0, s, c, 0, 0, 0, 1}};
}

Mat3 Mat3::operator*(const Mat3& o) const {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float v = 0;
            for (int k = 0; k < 3; ++k) v += m[i * 3 + k] * o.m[k * 3 + j];
            r.m[i * 3 + j] = v;
        }
    return r;
}

PointF Mat3::Apply(PointF p) const { return PointF{m[0] * p.x + m[1] * p.y + m[2], m[3] * p.x + m[4] * p.y + m[5]}; }

Mat3 Mat3::Inverse() const {
    const float a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) return Identity();
    const float inv = 1.f / det;
    Mat3 r{};
    r.m[0] = A * inv;
    r.m[1] = -(b * i - c * h) * inv;
    r.m[2] = (b * f - c * e) * inv;
    r.m[3] = B * inv;
    r.m[4] = (a * i - c * g) * inv;
    r.m[5] = -(a * f - c * d) * inv;
    r.m[6] = C * inv;
    r.m[7] = -(a * h - b * g) * inv;
    r.m[8] = (a * e - b * d) * inv;
    return r;
}

float Mat3::ScaleFactor() const { return std::sqrt(m[0] * m[0] + m[3] * m[3]); }

void TransformCorners(const Mat3& m, const RectF& r, PointF out[4]) {
    out[0] = m.Apply({r.x, r.y});
    out[1] = m.Apply({r.x + r.w, r.y});
    out[2] = m.Apply({r.x, r.y + r.h});
    out[3] = m.Apply({r.x + r.w, r.y + r.h});
}

RectF TransformBounds(const Mat3& m, const RectF& r) {
    PointF c[4];
    TransformCorners(m, r, c);
    float x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
    for (int i = 1; i < 4; ++i) {
        x0 = std::min(x0, c[i].x);
        x1 = std::max(x1, c[i].x);
        y0 = std::min(y0, c[i].y);
        y1 = std::max(y1, c[i].y);
    }
    return RectF{x0, y0, x1 - x0, y1 - y0};
}

namespace layer_math {

RectF StageRect(const project::Canvas& c) {
    const float w = static_cast<float>(c.width), h = static_cast<float>(c.height);
    float pad = static_cast<float>(std::max(c.padding_px, 0));
    pad = std::min(pad, std::min(w, h) * 0.5f - 0.5f);
    return RectF{pad, pad, std::max(w - 2 * pad, 1.f), std::max(h - 2 * pad, 1.f)};
}

RectF FitContain(SizeI content, RectF bounds) {
    if (content.IsEmpty() || bounds.w <= 0 || bounds.h <= 0) return bounds;
    const float s = std::min(bounds.w / static_cast<float>(content.w), bounds.h / static_cast<float>(content.h));
    const float w = static_cast<float>(content.w) * s, h = static_cast<float>(content.h) * s;
    return RectF{bounds.x + (bounds.w - w) * 0.5f, bounds.y + (bounds.h - h) * 0.5f, w, h};
}

float PropAt(const project::Clip& clip, const char* key, TimeUs t, float static_value) {
    if (clip.keyframes.empty()) return static_value;
    for (const auto& kv : clip.keyframes)
        if (std::strcmp(kv.first.c_str(), key) == 0)
            return kv.second.Empty() ? static_value : kv.second.At(t - clip.start_us, static_value);
    return static_value;
}

Mat3 ClipTransform(const project::Clip& clip, TimeUs t, RectF base, const project::Canvas& canvas) {
    const float x = PropAt(clip, "transform.x", t, clip.transform.x);
    const float y = PropAt(clip, "transform.y", t, clip.transform.y);
    const float s = PropAt(clip, "transform.scale", t, clip.transform.scale);
    const float r = PropAt(clip, "transform.rotation_deg", t, clip.transform.rotation_deg);
    const float cx = base.x + base.w * 0.5f, cy = base.y + base.h * 0.5f;
    const float ox = x * static_cast<float>(canvas.width), oy = y * static_cast<float>(canvas.height);
    return Mat3::Translate(cx + ox, cy + oy) * Mat3::Rotate(r) * Mat3::Scale(s, s) * Mat3::Translate(-cx, -cy);
}

Mat3 CameraTransform(const project::Camera::View& v, const project::Canvas& canvas) {
    const float w = static_cast<float>(canvas.width), h = static_cast<float>(canvas.height);
    const float z = v.zoom > 0.f ? v.zoom : 1.f;
    return Mat3::Translate(w * 0.5f, h * 0.5f) * Mat3::Scale(z, z) * Mat3::Translate(-v.cx * w, -v.cy * h);
}

RectF PipRect(const project::Pip& pip, SizeI cam_size, const project::Canvas& canvas) {
    const float cw = static_cast<float>(canvas.width), ch = static_cast<float>(canvas.height);
    const float w = std::clamp(pip.size, 0.01f, 1.f) * cw;
    float h = w;
    if (pip.mask != project::Pip::Mask::Circle && !cam_size.IsEmpty())
        h = w * static_cast<float>(cam_size.h) / static_cast<float>(cam_size.w);
    const float m = static_cast<float>(pip.margin_px);
    const bool left = pip.corner == project::Pip::Corner::TL || pip.corner == project::Pip::Corner::BL;
    const bool top = pip.corner == project::Pip::Corner::TL || pip.corner == project::Pip::Corner::TR;
    return RectF{left ? m : cw - m - w, top ? m : ch - m - h, w, h};
}

} // namespace layer_math
} // namespace ct::render
