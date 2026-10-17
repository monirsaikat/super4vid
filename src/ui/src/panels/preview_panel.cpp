// Preview panel: canvas image, overlays, gizmos (clip transform, annotations, camera key window) and transport.
#include <ct/ui/panels/preview_panel.h>

#include <ct/core/time.h>
#include <ct/project/edit_ops.h>
#include <ct/ui/editor_commands.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/strings.h>
#include <ct/ui/theme.h>
#include <ct/ui/widgets.h>

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ct::ui {

using namespace ct::project;
using namespace metrics;
using render::Mat3;

namespace {

constexpr float kPi = 3.14159265f;

ImU32 ToU32(const ColorF& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }
ImVec2 V(PointF p) { return ImVec2(p.x, p.y); }
PointF P(ImVec2 v) { return PointF{v.x, v.y}; }
float Len(ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
float LenP(PointF a, PointF b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

bool Keyed(const Clip& c, const char* prop) {
    const auto it = c.keyframes.find(prop);
    return it != c.keyframes.end() && !it->second.Empty();
}

TimeUs RelTime(const EditorState& s, const Clip& c) { return std::clamp<TimeUs>(s.Playhead() - c.start_us, 0, c.Duration()); }

enum PropMask : unsigned { kX = 1, kY = 2, kScale = 4, kRot = 8 };

// Writes transform props of one clip at the playhead: a key where the property is animated, else the static value.
bool ApplyTransform(EditorState& s, const Id& id, unsigned mask, float x, float y, float scale, float rot) {
    return s.Edit(str::kUndoProperty, [&](Project& p) {
        const Clip* c = p.FindClip(id);
        if (!c) return false;
        const TimeUs rel = RelTime(s, *c);
        Transform tr = c->transform;
        bool any_static = false;
        struct Item { unsigned bit; const char* prop; float v; float* field; };
        const Item items[] = {{kX, "transform.x", x, &tr.x}, {kY, "transform.y", y, &tr.y},
                              {kScale, "transform.scale", scale, &tr.scale}, {kRot, "transform.rotation_deg", rot, &tr.rotation_deg}};
        bool ok = true;
        for (const Item& it : items) {
            if (!(mask & it.bit)) continue;
            const Clip* cc = p.FindClip(id);
            if (Keyed(*cc, it.prop)) {
                const Keyframe<float>* k = cc->keyframes.at(it.prop).Nearest(rel, s.FrameUs() / 2);
                ok &= ops::SetKeyframe(p, id, it.prop, rel, it.v, k ? k->ease : Ease::Linear);
            } else {
                *it.field = it.v;
                any_static = true;
            }
        }
        if (any_static) ok &= ops::SetTransform(p, id, tr);
        return ok;
    });
}

bool PointInQuad(ImVec2 p, const ImVec2 q[4]) {   // TL, TR, BL, BR (convex)
    const ImVec2 poly[4] = {q[0], q[1], q[3], q[2]};
    float sign = 0;
    for (int i = 0; i < 4; ++i) {
        const ImVec2 a = poly[i], b = poly[(i + 1) % 4];
        const float cr = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cr == 0) continue;
        if (sign == 0) sign = cr;
        else if ((cr > 0) != (sign > 0)) return false;
    }
    return true;
}

// Handle positions TL, T, TR, R, BR, B, BL, L from quad corners TL, TR, BL, BR.
void HandlePoints(const ImVec2 q[4], ImVec2 out[8]) {
    out[0] = q[0];
    out[1] = (q[0] + q[1]) * 0.5f;
    out[2] = q[1];
    out[3] = (q[1] + q[3]) * 0.5f;
    out[4] = q[3];
    out[5] = (q[3] + q[2]) * 0.5f;
    out[6] = q[2];
    out[7] = (q[2] + q[0]) * 0.5f;
}

void DrawHandle(ImDrawList* dl, ImVec2 c, bool hot) {
    const float h = Dp(4);
    dl->AddRectFilled(c - ImVec2(h, h), c + ImVec2(h, h), hot ? col::accent : Rgb(0xFFFFFF), Dp(1.5f));
    dl->AddRect(c - ImVec2(h, h), c + ImVec2(h, h), col::accent, Dp(1.5f), 0, Dp(1.5f));
}

void DashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thick) {
    const float len = Len(b - a);
    if (len < 1) return;
    const ImVec2 d = (b - a) * (1.0f / len);
    const float dash = Dp(6), gap = Dp(4);
    for (float s = 0; s < len; s += dash + gap) dl->AddLine(a + d * s, a + d * std::min(len, s + dash), col, thick);
}

ImGuiMouseCursor CursorForHandle(int h) {
    switch (h) {
    case 0: case 4: return ImGuiMouseCursor_ResizeNWSE;
    case 2: case 6: return ImGuiMouseCursor_ResizeNESW;
    case 1: case 5: return ImGuiMouseCursor_ResizeNS;
    default: return ImGuiMouseCursor_ResizeEW;
    }
}

bool IsVideoLayer(const Project& p, const Clip& c) {
    if (c.IsAnnotation() || c.media.empty()) return false;
    if (c.source == Clip::Source::Mic || c.source == Clip::Source::System) return false;
    const Track* t = p.FindTrack(c.track);
    return t && t->kind == Track::Kind::Video && !t->hidden;
}

} // namespace

// --- parsing ---------------------------------------------------------------------------------------------------------

bool ParseTimecode(const char* s, int fps_num, int fps_den, TimeUs* out) {
    if (!s) return false;
    while (*s == ' ') ++s;
    if (!*s) return false;
    const char* colon = std::strchr(s, ':');
    if (!colon) {
        char* end = nullptr;
        const double sec = std::strtod(s, &end);
        if (end == s) return false;
        while (*end == ' ') ++end;
        if (*end == 's' || *end == 'S') ++end;
        while (*end == ' ') ++end;
        if (*end || !(sec >= 0) || sec > 360000) return false;
        *out = static_cast<TimeUs>(std::llround(sec * 1e6));
        return true;
    }
    // [HH:]MM:SS[.FF]
    long parts[3] = {0, 0, 0};
    int n = 0;
    const char* p = s;
    long frames = 0;
    while (true) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || n >= 3) return false;
        parts[n++] = v;
        if (*end == ':') {
            p = end + 1;
            continue;
        }
        if (*end == '.' || *end == ';') {
            char* fend = nullptr;
            frames = std::strtol(end + 1, &fend, 10);
            if (fend == end + 1 || frames < 0) return false;
            end = fend;
        }
        while (*end == ' ') ++end;
        if (*end) return false;
        break;
    }
    if (n < 2) return false;
    const long h = n == 3 ? parts[0] : 0, m = n == 3 ? parts[1] : parts[0], sec = n == 3 ? parts[2] : parts[1];
    if (m >= 60 || sec >= 60) return false;
    TimeUs t = (static_cast<TimeUs>(h) * 3600 + m * 60 + sec) * kUsPerSecond;
    if (frames > 0 && fps_num > 0 && fps_den > 0) t += FrameTimeUs(frames, fps_num, fps_den);
    *out = t;
    return true;
}

// --- geometry --------------------------------------------------------------------------------------------------------

ImVec2 PreviewPanel::ScreenToCanvas(ImVec2 s) const {
    const float k = m_scale > 0 ? 1.0f / m_scale : 1.0f;
    return ImVec2((s.x - m_img_min.x) * k, (s.y - m_img_min.y) * k);
}

Mat3 PreviewPanel::Camera(const EditorState& state) const {
    if (!view.camera) return Mat3::Identity();
    return render::layer_math::CameraTransform(state.project.camera.At(state.Playhead()), state.project.canvas);
}

SizeI PreviewPanel::VideoSize(const EditorState& state, const Clip& c) const {
    if (video_size) {
        const SizeI s = video_size(c);
        if (!s.IsEmpty()) return s;
    }
    const Media* m = state.project.FindMedia(c.media);
    if (m && m->width > 0 && m->height > 0) {
        if (c.source == Clip::Source::Camera) return SizeI{1280, 720};   // camera size is not in the project
        return SizeI{m->width, m->height};
    }
    return SizeI{state.project.canvas.width, state.project.canvas.height};
}

const Clip* PreviewPanel::GizmoClip(const EditorState& state) const {
    if (state.selection.clips.size() != 1 || state.selection.camera_key) return nullptr;
    const Clip* c = state.project.FindClip(state.selection.clips[0]);
    if (!c || !IsVideoLayer(state.project, *c)) return nullptr;
    const TimeUs t = state.Playhead();
    if (t < c->start_us || t >= c->End()) return nullptr;
    const Track* tr = state.project.FindTrack(c->track);
    return tr && !tr->locked ? c : nullptr;
}

const Clip* PreviewPanel::SelectedAnnotation(const EditorState& state) const {
    if (state.selection.clips.size() != 1 || !view.annotations) return nullptr;
    const Clip* c = state.project.FindClip(state.selection.clips[0]);
    if (!c || !c->IsAnnotation()) return nullptr;
    const TimeUs t = state.Playhead();
    if (t < c->start_us || t >= c->End()) return nullptr;
    const Track* tr = state.project.FindTrack(c->track);
    return tr && !tr->locked && !tr->hidden ? c : nullptr;
}

bool PreviewPanel::ClipGizmoCorners(const EditorState& state, ImVec2 out[4]) const {
    const Clip* c = GizmoClip(state);
    if (!c) return false;
    const SizeI vs = VideoSize(state, *c);
    Mat3 m = m_geom.VideoToCanvas(state.project, *c, state.Playhead(), vs);
    if (!view.camera) m = render::layer_math::CameraTransform(state.project.camera.At(state.Playhead()), state.project.canvas).Inverse() * m;
    const RectF r{static_cast<float>(c->crop.left), static_cast<float>(c->crop.top),
                  static_cast<float>(vs.w - c->crop.left - c->crop.right), static_cast<float>(vs.h - c->crop.top - c->crop.bottom)};
    PointF q[4];
    render::TransformCorners(m, r, q);
    for (int i = 0; i < 4; ++i) out[i] = CanvasToScreen(q[i].x, q[i].y);
    return true;
}

bool PreviewPanel::AnnotationRect(const EditorState& state, const Clip& c, ImVec2* mn, ImVec2* mx) const {
    if (!c.annotation) return false;
    const Canvas& cv = state.project.canvas;
    const TimeUs t = state.Playhead();
    const float ox = render::layer_math::PropAt(c, "transform.x", t, c.transform.x) * cv.width;
    const float oy = render::layer_math::PropAt(c, "transform.y", t, c.transform.y) * cv.height;
    const RectF& r = c.annotation->rect;
    const Mat3 cam = Camera(state);
    const PointF a = cam.Apply(PointF{r.x * cv.width + ox, r.y * cv.height + oy});
    const PointF b = cam.Apply(PointF{(r.x + r.w) * cv.width + ox, (r.y + r.h) * cv.height + oy});
    *mn = CanvasToScreen(a.x, a.y);
    *mx = CanvasToScreen(b.x, b.y);
    return true;
}

const Clip* PreviewPanel::HitTest(const EditorState& state, ImVec2 mouse) const {
    const Project& p = state.project;
    const TimeUs t = state.Playhead();
    // Top track first.
    for (int ti = static_cast<int>(p.tracks.size()) - 1; ti >= 0; --ti) {
        const Track& tr = p.tracks[static_cast<size_t>(ti)];
        if (tr.hidden || tr.kind == Track::Kind::Audio) continue;
        for (const Clip& c : p.clips) {
            if (c.track != tr.id || t < c.start_us || t >= c.End()) continue;
            if (c.IsAnnotation()) {
                if (!view.annotations) continue;
                ImVec2 a, b;
                AnnotationRect(state, c, &a, &b);
                const ImVec2 lo(std::min(a.x, b.x) - Dp(4), std::min(a.y, b.y) - Dp(4)), hi(std::max(a.x, b.x) + Dp(4), std::max(a.y, b.y) + Dp(4));
                if (mouse.x >= lo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y) return &c;
            } else if (IsVideoLayer(p, c)) {
                const SizeI vs = VideoSize(state, c);
                Mat3 m = m_geom.VideoToCanvas(p, c, t, vs);
                if (!view.camera) m = render::layer_math::CameraTransform(p.camera.At(t), p.canvas).Inverse() * m;
                PointF q[4];
                render::TransformCorners(m, RectF{0, 0, static_cast<float>(vs.w), static_cast<float>(vs.h)}, q);
                ImVec2 sq[4];
                for (int i = 0; i < 4; ++i) sq[i] = CanvasToScreen(q[i].x, q[i].y);
                if (PointInQuad(mouse, sq)) return &c;
            }
        }
    }
    return nullptr;
}

// --- drawing ---------------------------------------------------------------------------------------------------------

void PreviewPanel::DrawPlaceholder(const EditorState& state, ImVec2 mn, ImVec2 mx) {
    const Canvas& cv = state.project.canvas;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = Dp(kRadiusCanvas);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(mn, mx, ToU32(cv.background.color_a), r);
    if (cv.background.kind == Background::Kind::Gradient) {
        const float ang = cv.background.angle_deg * kPi / 180.0f;
        const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
        const float half = 0.5f * (std::fabs((mx.x - mn.x) * std::sin(ang)) + std::fabs((mx.y - mn.y) * std::cos(ang)));
        const ImVec2 d(std::sin(ang) * half, -std::cos(ang) * half);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, c - d, c + d, ToU32(cv.background.color_a), ToU32(cv.background.color_b));
    }
    const float scale = (mx.x - mn.x) / std::max(1, cv.width);
    const float pad = cv.padding_px * scale;
    const ImVec2 sa(mn.x + pad, mn.y + pad), sb(mx.x - pad, mx.y - pad);
    if (sb.x > sa.x + 8 && sb.y > sa.y + 8) {
        const float sr = cv.corner_radius_px * scale;
        if (cv.shadow.enabled) {
            const float blur = std::max(2.0f, cv.shadow.blur_px * scale);
            const float off = cv.shadow.offset_y_px * scale;
            for (int i = 6; i >= 1; --i) {
                const float g = blur * i / 6.0f;
                dl->AddRectFilled(ImVec2(sa.x - g * 0.5f, sa.y - g * 0.5f + off), ImVec2(sb.x + g * 0.5f, sb.y + g * 0.5f + off),
                                  Rgb(0x000000, static_cast<uint32_t>(cv.shadow.opacity * 255 / 10)), sr + g);
            }
        }
        dl->AddRectFilled(sa, sb, Rgb(0x0B0D11), sr);
        const float bar = std::max(6.0f, (sb.y - sa.y) * 0.06f);
        dl->AddRectFilled(sa, ImVec2(sb.x, sa.y + bar), Rgb(0x1A1E26), sr, ImDrawFlags_RoundCornersTop);
        for (int i = 0; i < 3; ++i)
            dl->AddCircleFilled(ImVec2(sa.x + bar * (0.8f + 0.7f * i), sa.y + bar * 0.5f), bar * 0.18f,
                                i == 0 ? Rgb(0xFF5F57) : i == 1 ? Rgb(0xFEBC2E) : Rgb(0x28C840));
        const ImVec2 body(sa.x, sa.y + bar);
        const float bw = sb.x - sa.x;
        dl->AddRectFilled(ImVec2(body.x + bw * 0.04f, body.y + bar * 0.9f), ImVec2(body.x + bw * 0.24f, sb.y - bar * 0.9f), Rgb(0x151922), Dp(4));
        for (int i = 0; i < 4; ++i) {
            const float y = body.y + bar * (1.2f + 1.1f * i);
            if (y + bar * 0.4f > sb.y - bar) break;
            dl->AddRectFilled(ImVec2(body.x + bw * 0.28f, y), ImVec2(body.x + bw * (0.85f - 0.12f * (i % 3)), y + bar * 0.35f), Rgb(0x1C212B), Dp(3));
        }
    }
}

void PreviewPanel::DrawOverlays(ImDrawList* dl, ImVec2 mn, ImVec2 mx) {
    const ImVec2 sz = mx - mn;
    if (safe_areas) {
        // Action safe 90 %, title safe 80 %.
        dl->AddRect(mn + sz * 0.05f, mx - sz * 0.05f, WithAlpha(col::text, 0.45f), 0, 0, 1.0f);
        dl->AddRect(mn + sz * 0.10f, mx - sz * 0.10f, WithAlpha(col::warn, 0.45f), 0, 0, 1.0f);
        const ImVec2 c = (mn + mx) * 0.5f;
        dl->AddLine(c - Dp(8, 0), c + Dp(8, 0), WithAlpha(col::text, 0.45f));
        dl->AddLine(c - Dp(0, 8), c + Dp(0, 8), WithAlpha(col::text, 0.45f));
    }
    if (thirds) {
        for (int i = 1; i <= 2; ++i) {
            const float x = mn.x + sz.x * i / 3.0f, y = mn.y + sz.y * i / 3.0f;
            dl->AddLine(ImVec2(x, mn.y), ImVec2(x, mx.y), WithAlpha(col::text, 0.3f));
            dl->AddLine(ImVec2(mn.x, y), ImVec2(mx.x, y), WithAlpha(col::text, 0.3f));
        }
    }
}

void PreviewPanel::DrawDragReadout(ImDrawList* dl, const char* text) {
    if (!text || !*text) return;
    FontScope f(FontRole::Mono);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetIO().MousePos + Dp(16, 18);
    p.x = std::min(p.x, m_area_max.x - ts.x - Dp(16));
    p.y = std::min(p.y, m_area_max.y - ts.y - Dp(12));
    dl->AddRectFilled(p - Dp(8, 4), p + ts + Dp(8, 4), WithAlpha(col::bg0, 0.92f), Dp(kRadiusControl));
    dl->AddRect(p - Dp(8, 4), p + ts + Dp(8, 4), col::border, Dp(kRadiusControl));
    dl->AddText(p, col::text, text);
}

void PreviewPanel::DrawClipGizmo(EditorState& state, ImDrawList* dl) {
    ImVec2 q[4];
    if (!ClipGizmoCorners(state, q)) return;
    const ImU32 line = col::accent;
    dl->AddLine(q[0], q[1], line, Dp(1.5f));
    dl->AddLine(q[1], q[3], line, Dp(1.5f));
    dl->AddLine(q[3], q[2], line, Dp(1.5f));
    dl->AddLine(q[2], q[0], line, Dp(1.5f));
    ImVec2 h[8];
    HandlePoints(q, h);
    const ImVec2 centre = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
    ImVec2 up = h[1] - centre;
    const float ul = Len(up);
    up = ul > 0.5f ? up * (1.0f / ul) : ImVec2(0, -1);
    const ImVec2 rot = h[1] + up * Dp(24);
    dl->AddLine(h[1], rot, line, Dp(1.5f));
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    dl->AddCircleFilled(rot, Dp(5), Len(mouse - rot) < Dp(8) || m_gizmo == Gizmo::Rotate ? col::accent : Rgb(0xFFFFFF), 16);
    dl->AddCircle(rot, Dp(5), col::accent, 16, Dp(1.5f));
    for (int i = 0; i < 8; ++i) DrawHandle(dl, h[i], (m_gizmo == Gizmo::Scale && m_drag.handle == i) || Len(mouse - h[i]) < Dp(7));
}

void PreviewPanel::DrawAnnotationGizmo(EditorState& state, ImDrawList* dl, const Clip& c) {
    ImVec2 a, b;
    AnnotationRect(state, c, &a, &b);
    const Annotation& an = *c.annotation;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (an.type == Annotation::Type::Arrow) {
        dl->AddLine(a, b, WithAlpha(col::accent, 0.7f), Dp(1));
        DrawHandle(dl, a, Len(mouse - a) < Dp(7));
        DrawHandle(dl, b, Len(mouse - b) < Dp(7));
        return;
    }
    const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
    dl->AddRect(lo, hi, col::accent, 0, 0, Dp(1.5f));
    const ImVec2 q[4] = {lo, ImVec2(hi.x, lo.y), ImVec2(lo.x, hi.y), hi};
    ImVec2 h[8];
    HandlePoints(q, h);
    for (int i = 0; i < 8; ++i) DrawHandle(dl, h[i], (m_gizmo == Gizmo::AnnResize && m_drag.handle == i) || Len(mouse - h[i]) < Dp(7));
    if (an.type == Annotation::Type::Callout) {
        const Canvas& cv = state.project.canvas;
        const TimeUs t = state.Playhead();
        const float ox = render::layer_math::PropAt(c, "transform.x", t, c.transform.x) * cv.width;
        const float oy = render::layer_math::PropAt(c, "transform.y", t, c.transform.y) * cv.height;
        const PointF pc = Camera(state).Apply(PointF{an.pointer.x * cv.width + ox, an.pointer.y * cv.height + oy});
        const ImVec2 ps = CanvasToScreen(pc.x, pc.y);
        DashedLine(dl, (lo + hi) * 0.5f, ps, WithAlpha(col::accent, 0.7f), Dp(1));
        const bool hot = Len(mouse - ps) < Dp(8) || m_gizmo == Gizmo::AnnPointer;
        dl->AddCircleFilled(ps, Dp(5), hot ? col::accent : col::warn, 16);
        dl->AddCircle(ps, Dp(5), Rgb(0xFFFFFF), 16, Dp(1.5f));
    }
}

void PreviewPanel::DrawCameraGizmo(EditorState& state, ImDrawList* dl) {
    if (!state.selection.camera_key) return;
    const CameraKey* k = nullptr;
    for (const CameraKey& kk : state.project.camera.keys)
        if (kk.t_us == *state.selection.camera_key) k = &kk;
    if (!k) return;
    const Canvas& cv = state.project.canvas;
    const float z = std::max(1.0f, k->zoom);
    const float cx = ClampCentre(k->cx, z), cy = ClampCentre(k->cy, z);
    const ImVec2 a = CanvasToScreen((cx - 0.5f / z) * cv.width, (cy - 0.5f / z) * cv.height);
    const ImVec2 b = CanvasToScreen((cx + 0.5f / z) * cv.width, (cy + 0.5f / z) * cv.height);
    // Dim outside the window.
    const ImU32 dim = Rgb(0x000000, 110);
    dl->AddRectFilled(m_img_min, ImVec2(m_img_max.x, a.y), dim);
    dl->AddRectFilled(ImVec2(m_img_min.x, b.y), m_img_max, dim);
    dl->AddRectFilled(ImVec2(m_img_min.x, a.y), ImVec2(a.x, b.y), dim);
    dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(m_img_max.x, b.y), dim);
    const ImU32 c = k->generated ? WithAlpha(col::camera_kf, 0.7f) : col::camera_kf;
    DashedLine(dl, a, ImVec2(b.x, a.y), c, Dp(2));
    DashedLine(dl, ImVec2(b.x, a.y), b, c, Dp(2));
    DashedLine(dl, b, ImVec2(a.x, b.y), c, Dp(2));
    DashedLine(dl, ImVec2(a.x, b.y), a, c, Dp(2));
    const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b};
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const ImVec2& p : corners) DrawHandle(dl, p, Len(mouse - p) < Dp(8));
    char label[48];
    std::snprintf(label, sizeof(label), "%.2f\xC3\x97%s", z, k->generated ? "  auto" : "");
    FontScope f(FontRole::Label);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 lp(a.x + Dp(8), a.y + Dp(8));
    dl->AddRectFilled(lp - Dp(6, 3), lp + ts + Dp(6, 3), WithAlpha(col::bg0, 0.85f), Dp(kRadiusPill));
    dl->AddText(lp, col::text, label);
}

// --- gizmo interaction -----------------------------------------------------------------------------------------------

void PreviewPanel::HandleGizmos(EditorState& state, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const Canvas& cv = state.project.canvas;
    const Mat3 cam = Camera(state);
    const Mat3 cam_inv = cam.Inverse();
    auto pre = [&](ImVec2 screen) { return cam_inv.Apply(P(ScreenToCanvas(screen))); };
    const TimeUs t = state.Playhead();

    // Panning when zoomed: Space+drag or middle drag.
    if (m_gizmo == Gizmo::None && hovered && fit_mode != 0 &&
        ((ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseClicked(0)) || ImGui::IsMouseClicked(2))) {
        m_gizmo = Gizmo::Pan;
        m_drag.press_screen = mouse;
        m_drag.pan0 = m_pan;
        return;
    }
    if (m_gizmo == Gizmo::Pan) {
        if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(2)) {
            m_gizmo = Gizmo::None;
            return;
        }
        m_pan = m_drag.pan0 + (mouse - m_drag.press_screen);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        return;
    }

    const Clip* gclip = GizmoClip(state);
    const Clip* aclip = SelectedAnnotation(state);

    // --- press: pick a handle / body / another clip ---
    if (m_gizmo == Gizmo::None && hovered && ImGui::IsMouseClicked(0) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        m_drag = Drag{};
        m_drag.press_screen = mouse;
        m_drag.press_pre = pre(mouse);
        m_drag_started = false;
        bool picked = false;
        // Camera key window.
        if (state.selection.camera_key) {
            for (const CameraKey& k : state.project.camera.keys) {
                if (k.t_us != *state.selection.camera_key) continue;
                const float z = std::max(1.0f, k.zoom);
                const float cx = ClampCentre(k.cx, z), cy = ClampCentre(k.cy, z);
                const ImVec2 a = CanvasToScreen((cx - 0.5f / z) * cv.width, (cy - 0.5f / z) * cv.height);
                const ImVec2 b = CanvasToScreen((cx + 0.5f / z) * cv.width, (cy + 0.5f / z) * cv.height);
                const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), ImVec2(a.x, b.y), b};
                m_drag.key0 = k;
                for (const ImVec2& p : corners)
                    if (Len(mouse - p) < Dp(9)) {
                        m_gizmo = Gizmo::CamZoom;
                        picked = true;
                    }
                if (!picked && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) {
                    m_gizmo = Gizmo::CamPan;
                    picked = true;
                }
            }
        }
        if (!picked && gclip) {
            ImVec2 q[4];
            ClipGizmoCorners(state, q);
            ImVec2 h[8];
            HandlePoints(q, h);
            const ImVec2 centre = (q[0] + q[1] + q[2] + q[3]) * 0.25f;
            ImVec2 up = h[1] - centre;
            const float ul = Len(up);
            up = ul > 0.5f ? up * (1.0f / ul) : ImVec2(0, -1);
            m_drag.clip = gclip->id;
            m_drag.x0 = render::layer_math::PropAt(*gclip, "transform.x", t, gclip->transform.x);
            m_drag.y0 = render::layer_math::PropAt(*gclip, "transform.y", t, gclip->transform.y);
            m_drag.s0 = render::layer_math::PropAt(*gclip, "transform.scale", t, gclip->transform.scale);
            m_drag.r0 = render::layer_math::PropAt(*gclip, "transform.rotation_deg", t, gclip->transform.rotation_deg);
            m_drag.centre_pre = pre(centre);
            if (Len(mouse - (h[1] + up * Dp(24))) < Dp(9)) {
                m_gizmo = Gizmo::Rotate;
                picked = true;
            }
            for (int i = 0; i < 8 && !picked; ++i)
                if (Len(mouse - h[i]) < Dp(8)) {
                    m_gizmo = Gizmo::Scale;
                    m_drag.handle = i;
                    m_drag.anchor_pre = pre(h[(i + 4) % 8]);
                    picked = true;
                }
            if (!picked && PointInQuad(mouse, q)) {
                m_gizmo = Gizmo::Move;
                picked = true;
            }
        }
        if (!picked && aclip) {
            ImVec2 a, b;
            AnnotationRect(state, *aclip, &a, &b);
            const Annotation& an = *aclip->annotation;
            m_drag.clip = aclip->id;
            m_drag.rect0 = an.rect;
            m_drag.pointer0 = an.pointer;
            if (an.type == Annotation::Type::Arrow) {
                if (Len(mouse - a) < Dp(9)) m_gizmo = Gizmo::AnnArrowStart;
                else if (Len(mouse - b) < Dp(9)) m_gizmo = Gizmo::AnnArrowEnd;
            } else {
                if (an.type == Annotation::Type::Callout) {
                    const float ox = render::layer_math::PropAt(*aclip, "transform.x", t, aclip->transform.x) * cv.width;
                    const float oy = render::layer_math::PropAt(*aclip, "transform.y", t, aclip->transform.y) * cv.height;
                    const PointF pc = cam.Apply(PointF{an.pointer.x * cv.width + ox, an.pointer.y * cv.height + oy});
                    if (Len(mouse - CanvasToScreen(pc.x, pc.y)) < Dp(9)) m_gizmo = Gizmo::AnnPointer;
                }
                if (m_gizmo == Gizmo::None) {
                    const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
                    const ImVec2 q[4] = {lo, ImVec2(hi.x, lo.y), ImVec2(lo.x, hi.y), hi};
                    ImVec2 h[8];
                    HandlePoints(q, h);
                    for (int i = 0; i < 8; ++i)
                        if (Len(mouse - h[i]) < Dp(8)) {
                            m_gizmo = Gizmo::AnnResize;
                            m_drag.handle = i;
                            break;
                        }
                }
            }
            if (m_gizmo != Gizmo::None) picked = true;
            else {
                const ImVec2 lo(std::min(a.x, b.x) - Dp(4), std::min(a.y, b.y) - Dp(4)), hi(std::max(a.x, b.x) + Dp(4), std::max(a.y, b.y) + Dp(4));
                if (mouse.x >= lo.x && mouse.x <= hi.x && mouse.y >= lo.y && mouse.y <= hi.y) {
                    m_gizmo = Gizmo::AnnMove;
                    picked = true;
                }
            }
        }
        if (!picked) {
            // Click-select the topmost clip under the mouse (or clear the selection on empty canvas).
            if (const Clip* hit = HitTest(state, mouse)) {
                state.selection.Set(hit->id);
                if (hit->IsAnnotation()) {
                    m_drag.clip = hit->id;
                    m_drag.rect0 = hit->annotation->rect;
                    m_drag.pointer0 = hit->annotation->pointer;
                    m_gizmo = Gizmo::AnnMove;
                } else if (state.project.FindTrack(hit->track) && !state.project.FindTrack(hit->track)->locked) {
                    m_drag.clip = hit->id;
                    m_drag.x0 = render::layer_math::PropAt(*hit, "transform.x", t, hit->transform.x);
                    m_drag.y0 = render::layer_math::PropAt(*hit, "transform.y", t, hit->transform.y);
                    m_drag.s0 = render::layer_math::PropAt(*hit, "transform.scale", t, hit->transform.scale);
                    m_drag.r0 = render::layer_math::PropAt(*hit, "transform.rotation_deg", t, hit->transform.rotation_deg);
                    m_gizmo = Gizmo::Move;
                }
            } else if (!state.selection.camera_key) {
                state.selection.Clear();
            }
        }
    }

    // Double-click a text / callout annotation: inline edit.
    if (hovered && ImGui::IsMouseDoubleClicked(0)) {
        if (const Clip* hit = HitTest(state, mouse); hit && hit->IsAnnotation() &&
            (hit->annotation->type == Annotation::Type::Text || hit->annotation->type == Annotation::Type::Callout)) {
            m_gizmo = Gizmo::None;
            state.EndDrag();
            m_text_clip = hit->id;
            std::snprintf(m_text_buf, sizeof(m_text_buf), "%s", hit->annotation->text.c_str());
            m_text_focus = true;
            state.selection.Set(hit->id);
        }
    }

    if (m_gizmo == Gizmo::None) {
        // Hover cursors.
        if (hovered && gclip) {
            ImVec2 q[4];
            ClipGizmoCorners(state, q);
            ImVec2 h[8];
            HandlePoints(q, h);
            for (int i = 0; i < 8; ++i)
                if (Len(mouse - h[i]) < Dp(8)) ImGui::SetMouseCursor(CursorForHandle(i));
            if (PointInQuad(mouse, q) && ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        return;
    }

    // --- drag in progress ---
    if (!ImGui::IsMouseDown(0)) {
        if (m_drag_started) state.EndDrag();
        m_gizmo = Gizmo::None;
        m_drag_started = false;
        m_readout[0] = 0;
        return;
    }
    const ImVec2 dscreen = mouse - m_drag.press_screen;
    if (!m_drag_started) {
        if (std::fabs(dscreen.x) < Dp(kTlDragThreshold) && std::fabs(dscreen.y) < Dp(kTlDragThreshold)) return;
        const char* label = str::kUndoProperty;
        switch (m_gizmo) {
        case Gizmo::Move: label = "Move layer"; break;
        case Gizmo::Scale: label = "Scale layer"; break;
        case Gizmo::Rotate: label = "Rotate layer"; break;
        case Gizmo::CamPan: case Gizmo::CamZoom: label = str::kUndoCameraKey; break;
        default: label = "Edit annotation"; break;
        }
        state.BeginDrag(label);
        m_drag_started = true;
    }
    m_wants_redraw = true;
    const PointF mp = pre(mouse);
    const bool shift = io.KeyShift, alt = io.KeyAlt;
    const float W = static_cast<float>(std::max(1, cv.width)), H = static_cast<float>(std::max(1, cv.height));

    switch (m_gizmo) {
    case Gizmo::Move: {
        float dx = mp.x - m_drag.press_pre.x, dy = mp.y - m_drag.press_pre.y;
        if (shift) (std::fabs(dx) > std::fabs(dy) ? dy : dx) = 0;
        const float x = m_drag.x0 + dx / W, y = m_drag.y0 + dy / H;
        ApplyTransform(state, m_drag.clip, kX | kY, x, y, 0, 0);
        std::snprintf(m_readout, sizeof(m_readout), "x %.3f  y %.3f", x, y);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        break;
    }
    case Gizmo::Scale: {
        // Uniform scale (Transform has one scale): about the centre, or about the opposite handle with Alt.
        const PointF pivot = alt ? m_drag.anchor_pre : m_drag.centre_pre;
        const float d0 = LenP(m_drag.press_pre, pivot), d1 = LenP(mp, pivot);
        if (d0 < 1) break;
        float k = d1 / d0;
        const float s = std::clamp(m_drag.s0 * k, 0.02f, 20.0f);
        k = s / std::max(1e-4f, m_drag.s0);
        float x = m_drag.x0, y = m_drag.y0;
        unsigned mask = kScale;
        if (alt) {
            // The centre moves away from the pivot by the same factor; the pivot stays put.
            const float ncx = pivot.x + (m_drag.centre_pre.x - pivot.x) * k, ncy = pivot.y + (m_drag.centre_pre.y - pivot.y) * k;
            x += (ncx - m_drag.centre_pre.x) / W;
            y += (ncy - m_drag.centre_pre.y) / H;
            mask |= kX | kY;
        }
        ApplyTransform(state, m_drag.clip, mask, x, y, s, 0);
        std::snprintf(m_readout, sizeof(m_readout), "scale %.0f %%", s * 100.0f);
        ImGui::SetMouseCursor(CursorForHandle(m_drag.handle));
        break;
    }
    case Gizmo::Rotate: {
        const float a0 = std::atan2(m_drag.press_pre.y - m_drag.centre_pre.y, m_drag.press_pre.x - m_drag.centre_pre.x);
        const float a1 = std::atan2(mp.y - m_drag.centre_pre.y, mp.x - m_drag.centre_pre.x);
        float r = m_drag.r0 + (a1 - a0) * 180.0f / kPi;
        while (r > 180) r -= 360;
        while (r < -180) r += 360;
        if (shift) r = std::round(r / 15.0f) * 15.0f;
        ApplyTransform(state, m_drag.clip, kRot, 0, 0, 0, r);
        std::snprintf(m_readout, sizeof(m_readout), "rotation %.1f\xC2\xB0", r);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        break;
    }
    case Gizmo::AnnMove: case Gizmo::AnnResize: case Gizmo::AnnPointer: case Gizmo::AnnArrowStart: case Gizmo::AnnArrowEnd: {
        const Clip* c = state.project.FindClip(m_drag.clip);
        if (!c || !c->annotation) break;
        float dx = (mp.x - m_drag.press_pre.x) / W, dy = (mp.y - m_drag.press_pre.y) / H;
        Annotation an = *c->annotation;
        RectF r = m_drag.rect0;
        if (m_gizmo == Gizmo::AnnMove) {
            if (shift) (std::fabs(dx) > std::fabs(dy) ? dy : dx) = 0;
            r.x += dx;
            r.y += dy;
            an.pointer = PointF{m_drag.pointer0.x + dx, m_drag.pointer0.y + dy};
            std::snprintf(m_readout, sizeof(m_readout), "x %.3f  y %.3f", r.x, r.y);
        } else if (m_gizmo == Gizmo::AnnPointer) {
            an.pointer = PointF{m_drag.pointer0.x + dx, m_drag.pointer0.y + dy};
            std::snprintf(m_readout, sizeof(m_readout), "pointer %.3f, %.3f", an.pointer.x, an.pointer.y);
        } else if (m_gizmo == Gizmo::AnnArrowStart) {
            r.x += dx;
            r.y += dy;
            r.w -= dx;
            r.h -= dy;
        } else if (m_gizmo == Gizmo::AnnArrowEnd) {
            r.w += dx;
            r.h += dy;
        } else {
            const int h = m_drag.handle;
            const bool left = h == 0 || h == 6 || h == 7, right = h == 2 || h == 3 || h == 4;
            const bool top = h == 0 || h == 1 || h == 2, bottom = h == 4 || h == 5 || h == 6;
            if (left) { r.x += dx; r.w -= dx; }
            if (right) r.w += dx;
            if (top) { r.y += dy; r.h -= dy; }
            if (bottom) r.h += dy;
            if (shift && (left || right) && (top || bottom) && m_drag.rect0.w > 0 && m_drag.rect0.h > 0) {
                const float f = std::max(r.w / m_drag.rect0.w, r.h / m_drag.rect0.h);
                const float nw = m_drag.rect0.w * f, nh = m_drag.rect0.h * f;
                if (left) r.x = m_drag.rect0.x + m_drag.rect0.w - nw;
                if (top) r.y = m_drag.rect0.y + m_drag.rect0.h - nh;
                r.w = nw;
                r.h = nh;
            }
            const float min = 0.01f;
            if (r.w < min) {
                if (left) r.x = m_drag.rect0.x + m_drag.rect0.w - min;
                r.w = min;
            }
            if (r.h < min) {
                if (top) r.y = m_drag.rect0.y + m_drag.rect0.h - min;
                r.h = min;
            }
            std::snprintf(m_readout, sizeof(m_readout), "%.0f \xC3\x97 %.0f px", r.w * W, r.h * H);
            ImGui::SetMouseCursor(CursorForHandle(h));
        }
        an.rect = r;
        const Id id = m_drag.clip;
        state.Edit("Edit annotation", [&](Project& p) { return ops::SetAnnotation(p, id, an); });
        break;
    }
    case Gizmo::CamPan: case Gizmo::CamZoom: {
        CameraKey k = m_drag.key0;
        const float z0 = std::max(1.0f, k.zoom);
        const PointF press_c = P(ScreenToCanvas(m_drag.press_screen));
        const PointF now_c = P(ScreenToCanvas(mouse));
        if (m_gizmo == Gizmo::CamPan) {
            k.cx = ClampCentre(ClampCentre(k.cx, z0) + (now_c.x - press_c.x) / W, z0);
            k.cy = ClampCentre(ClampCentre(k.cy, z0) + (now_c.y - press_c.y) / H, z0);
            std::snprintf(m_readout, sizeof(m_readout), "centre %.3f, %.3f", k.cx, k.cy);
        } else {
            const PointF c{ClampCentre(k.cx, z0) * W, ClampCentre(k.cy, z0) * H};
            const float d0 = LenP(press_c, c), d1 = LenP(now_c, c);
            if (d0 < 1 || d1 < 1) break;
            k.zoom = std::clamp(z0 * d0 / d1, 1.0f, 8.0f);
            k.cx = ClampCentre(k.cx, k.zoom);
            k.cy = ClampCentre(k.cy, k.zoom);
            std::snprintf(m_readout, sizeof(m_readout), "zoom %.2f\xC3\x97", k.zoom);
        }
        k.generated = false;   // editing a generated key converts it to manual
        state.Edit(str::kUndoCameraKey, [&](Project& p) { return ops::SetCameraKey(p, k); });
        break;
    }
    default: break;
    }
}

void PreviewPanel::DrawInlineText(EditorState& state) {
    if (m_text_clip.empty()) return;
    const Clip* c = state.project.FindClip(m_text_clip);
    if (!c || !c->annotation) {
        m_text_clip.clear();
        return;
    }
    ImVec2 a, b;
    AnnotationRect(state, *c, &a, &b);
    const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
    const ImVec2 size(std::max(Dp(160), std::fabs(b.x - a.x)), std::max(Dp(56), std::fabs(b.y - a.y)));
    ImGui::SetCursorScreenPos(lo);
    if (m_text_focus) ImGui::SetKeyboardFocusHere();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ToVec4(WithAlpha(col::bg0, 0.92f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);
    ImGui::PushStyleColor(ImGuiCol_Border, ToVec4(col::accent));
    const bool enter = ImGui::InputTextMultiline("##inline_text", m_text_buf, sizeof(m_text_buf), size,
                                                 ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    const bool active = ImGui::IsItemActive();
    const bool cancel = active && ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool lost = !m_text_focus && !active;
    m_text_focus = false;
    if (cancel) {
        m_text_clip.clear();
        return;
    }
    if (enter || lost) {
        Annotation an = *c->annotation;
        if (an.text != m_text_buf) {
            an.text = m_text_buf;
            const Id id = m_text_clip;
            state.Edit("Edit text", [&](Project& p) { return ops::SetAnnotation(p, id, an); });
        }
        m_text_clip.clear();
    }
}

void PreviewPanel::DrawImage(EditorState& state, ImVec2 origin, ImVec2 area) {
    const Canvas& cv = state.project.canvas;
    const float margin = Dp(kPreviewMargin);
    const float aw = std::max(1.0f, area.x - margin * 2), ah = std::max(1.0f, area.y - margin * 2);
    const float cw = static_cast<float>(std::max(1, cv.width)), ch = static_cast<float>(std::max(1, cv.height));
    float w = aw, h = aw * ch / cw;
    if (h > ah) {
        h = ah;
        w = ah * cw / ch;
    }
    if (fit_mode == 0) m_pan = ImVec2(0, 0);
    else {
        const float z = fit_mode == 1 ? 0.5f : fit_mode == 2 ? 1.0f : 2.0f;
        w = cw * z;
        h = ch * z;
        // Keep at least a quarter of the image inside the area.
        const float mx = std::max(0.0f, (w + area.x) * 0.5f - std::min(w, area.x) * 0.25f);
        const float my = std::max(0.0f, (h + area.y) * 0.5f - std::min(h, area.y) * 0.25f);
        m_pan.x = std::clamp(m_pan.x, -mx, mx);
        m_pan.y = std::clamp(m_pan.y, -my, my);
    }
    m_img_min = ImVec2(std::floor(origin.x + (area.x - w) * 0.5f + m_pan.x), std::floor(origin.y + (area.y - h) * 0.5f + m_pan.y));
    m_img_max = m_img_min + ImVec2(w, h);
    m_scale = w / cw;
    m_area_min = origin;
    m_area_max = origin + area;

    // Input surface first (so the panel window never drags), drawing after.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##canvas", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, origin + area, true);
    // Soft drop shadow under the canvas.
    for (int i = 4; i >= 1; --i) {
        const float g = Dp(3.0f * i);
        dl->AddRectFilled(m_img_min - ImVec2(g, g) + Dp(0, 4), m_img_max + ImVec2(g, g) + Dp(0, 4), Rgb(0x000000, 14), Dp(kRadiusCanvas) + g);
    }
    view.camera = !state.selection.camera_key.has_value();
    PreviewFrame frame;
    if (render) frame = render(state, view);
    if (frame.tex != ImTextureID_Invalid) {
        dl->AddImageRounded(frame.tex, m_img_min, m_img_max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, Dp(kRadiusCanvas));
        if (!frame.exact) m_wants_redraw = true;
    } else {
        DrawPlaceholder(state, m_img_min, m_img_max);
    }
    dl->AddRect(m_img_min, m_img_max, WithAlpha(col::border, 0.8f), Dp(kRadiusCanvas));
    DrawOverlays(dl, m_img_min, m_img_max);

    HandleGizmos(state, hovered);
    if (m_text_clip.empty()) {
        DrawCameraGizmo(state, dl);
        DrawClipGizmo(state, dl);
        if (const Clip* a = SelectedAnnotation(state)) DrawAnnotationGizmo(state, dl, *a);
    }
    if (m_gizmo != Gizmo::None && m_gizmo != Gizmo::Pan) DrawDragReadout(dl, m_readout);
    if (!frame.exact && frame.tex != ImTextureID_Invalid && !state.playback->Playing()) {
        // Tiny decoding indicator in the corner while the exact frame is on its way.
        const float t = static_cast<float>(ImGui::GetTime());
        const ImVec2 c(m_img_max.x - Dp(14), m_img_min.y + Dp(14));
        dl->PathArcTo(c, Dp(5), t * 6.0f, t * 6.0f + 4.0f, 12);
        dl->PathStroke(WithAlpha(col::text, 0.8f), 0, Dp(1.5f));
    }
    dl->PopClipRect();
    DrawInlineText(state);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area.y));
}

// --- transport -------------------------------------------------------------------------------------------------------

void PreviewPanel::DrawTimecode(EditorState& state, float h) {
    const auto& cv = state.project.canvas;
    ImFont* mono = Fonts::Get(FontRole::Mono);
    const float fs = Dp(13);
    char a[32], b[40];
    FormatTimecode(a, sizeof(a), state.Playhead(), cv.fps_num, cv.fps_den);
    std::snprintf(b, sizeof(b), "  /  ");
    const size_t bl = std::strlen(b);
    FormatTimecode(b + bl, sizeof(b) - bl, state.project.Duration(), cv.fps_num, cv.fps_den);
    const ImVec2 sa = mono ? mono->CalcTextSizeA(fs, FLT_MAX, 0, a) : ImGui::CalcTextSize(a);
    const ImVec2 sb = mono ? mono->CalcTextSizeA(fs, FLT_MAX, 0, b) : ImGui::CalcTextSize(b);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (m_time_edit) {
        ImGui::SetNextItemWidth(sa.x + Dp(28));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetFrameHeight()) * 0.5f);
        if (m_time_focus) ImGui::SetKeyboardFocusHere();
        ImGui::PushFont(mono, fs);
        const bool enter = ImGui::InputText("##timeentry", m_time_buf, sizeof(m_time_buf),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopFont();
        const bool active = ImGui::IsItemActive();
        if (enter) {
            TimeUs t = 0;
            if (ParseTimecode(m_time_buf, cv.fps_num, cv.fps_den, &t)) {
                state.playback->Pause();
                state.playback->Seek(t);
            } else {
                state.last_error = "Type a time like 00:01:12.03 or 72.5";
            }
            m_time_edit = false;
        } else if (!m_time_focus && (!active || ImGui::IsKeyPressed(ImGuiKey_Escape))) {
            m_time_edit = false;
        }
        m_time_focus = false;
        ImGui::SameLine(0, 0);
        const ImVec2 q = ImGui::GetCursorScreenPos();
        dl->AddText(mono, fs, ImVec2(q.x, std::floor(p.y + (h - sb.y) * 0.5f)), col::text_faint, b);
        ImGui::Dummy(ImVec2(sb.x, h));
        return;
    }
    ImGui::InvisibleButton("##timecode", ImVec2(sa.x + sb.x, h));
    const bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) {
        m_time_edit = true;
        m_time_focus = true;
        std::snprintf(m_time_buf, sizeof(m_time_buf), "%s", a);
    }
    ItemTooltip("Click to type a time", nullptr);
    const float ty = std::floor(p.y + (h - sa.y) * 0.5f);
    if (hov) dl->AddRectFilled(ImVec2(p.x - Dp(6), p.y + Dp(3)), ImVec2(p.x + sa.x + Dp(6), p.y + h - Dp(3)), col::bg3, Dp(kRadiusControl));
    dl->AddText(mono, fs, ImVec2(p.x, ty), col::text, a);
    dl->AddText(mono, fs, ImVec2(p.x + sa.x, ty), col::text_faint, b);
}

void PreviewPanel::DrawTransport(EditorState& state) {
    PlaybackControl& pb = *state.playback;
    const float h = Dp(kControlDefault);
    const float total_w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Dp(12));
    if (IconButton("start", ICON_PREVIOUS, "Go to start", false, nullptr, ImVec2(h, h))) Run(Action::GoStart);
    ItemTooltip("Go to start", "Home");
    ImGui::SameLine(0, Dp(2));
    if (IconButton("prev", ICON_CHEVRON_LEFT, nullptr, false, nullptr, ImVec2(h, h))) Run(Action::PrevFrame);
    ItemTooltip("Previous frame", "Left");
    ImGui::SameLine(0, Dp(2));
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("play", ImVec2(h + Dp(4), h));
        const bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) Run(Action::TogglePlay);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + (h + Dp(4)) * 0.5f, p.y + h * 0.5f);
        dl->AddCircleFilled(c, h * 0.5f, hov ? col::accent_hover : col::accent, 32);
        FontScope f(FontRole::Icon);
        const char* icon = pb.Playing() ? ICON_PAUSE : ICON_PLAY;
        const ImVec2 ts = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(std::floor(c.x - ts.x * 0.5f + (pb.Playing() ? 0 : Dp(1))), std::floor(c.y - ts.y * 0.5f)), Rgb(0xFFFFFF), icon);
        ItemTooltip(pb.Playing() ? "Pause" : "Play", "Space");
    }
    ImGui::SameLine(0, Dp(2));
    if (IconButton("next", ICON_CHEVRON_RIGHT, nullptr, false, nullptr, ImVec2(h, h))) Run(Action::NextFrame);
    ItemTooltip("Next frame", "Right");
    ImGui::SameLine(0, Dp(2));
    if (IconButton("end", ICON_NEXT, nullptr, false, nullptr, ImVec2(h, h))) Run(Action::GoEnd);
    ItemTooltip("Go to end", "End");
    ImGui::SameLine(0, Dp(16));
    DrawTimecode(state, h);
    if (pb.Rate() != 0 && pb.Rate() != 1) {
        ImGui::SameLine(0, Dp(10));
        char r[16];
        std::snprintf(r, sizeof(r), "%.4g\xC3\x97", pb.Rate());
        FontScope f(FontRole::Strong);
        const ImVec2 ts = ImGui::CalcTextSize(r);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float py = p.y + (h - ts.y) * 0.5f;
        dl->AddRectFilled(ImVec2(p.x, py - Dp(2)), ImVec2(p.x + ts.x + Dp(12), py + ts.y + Dp(2)), WithAlpha(col::warn, 0.18f), Dp(kRadiusPill));
        dl->AddText(ImVec2(p.x + Dp(6), py), col::warn, r);
        ImGui::Dummy(ImVec2(ts.x + Dp(12), h));
    }
    // Right side: fit, loop, overlays, mute + volume.
    const float right_w = Dp(84) + h * 3 + Dp(4) + Dp(4) + Dp(2) * 2 + Dp(12);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + Dp(8), total_w - right_w));
    static const char* const kFit[] = {"Fit", "50 %", "100 %", "200 %"};
    char fit[32];
    std::snprintf(fit, sizeof(fit), "%s  %s", kFit[fit_mode], ICON_CHEVRON_DOWN);
    if (Button(fit, ButtonKind::Normal, ImVec2(Dp(84), h))) ImGui::OpenPopup("##fitmenu");
    ItemTooltip("Preview zoom (Space+drag pans)", nullptr);
    if (BeginPopover("##fitmenu")) {
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(kFit[i], fit_mode == i)) {
                fit_mode = i;
                m_pan = ImVec2(0, 0);
            }
        EndPopover();
    }
    ImGui::SameLine(0, Dp(4));
    const bool loop = state.project.ui.loop.has_value();
    if (IconButton("loop", ICON_REPEAT, nullptr, loop, nullptr, ImVec2(h, h))) {
        if (loop) state.project.ui.loop.reset();
        else state.project.ui.loop = std::make_pair(TimeUs(0), state.project.Duration());
        state.Touch();
    }
    ItemTooltip(str::kLoop, "I / O");
    ImGui::SameLine(0, Dp(2));
    const bool any_overlay = safe_areas || thirds || !view.cursor || !view.annotations;
    if (IconButton("overlays", ICON_VIEW, nullptr, any_overlay, nullptr, ImVec2(h, h))) ImGui::OpenPopup("##overlays");
    ItemTooltip("Overlays", nullptr);
    if (BeginPopover("##overlays")) {
        ImGui::TextDisabled("Guides");
        ImGui::Checkbox(str::kSafeAreas, &safe_areas);
        ImGui::Checkbox("Rule of thirds", &thirds);
        ImGui::Separator();
        ImGui::TextDisabled("Show in preview");
        bool changed = ImGui::Checkbox("Cursor", &view.cursor);
        changed |= ImGui::Checkbox("Annotations", &view.annotations);
        changed |= ImGui::Checkbox("Keystrokes", &view.keystrokes);
        if (changed) state.needs_render = true;
        EndPopover();
    }
    ImGui::SameLine(0, Dp(2));
    const bool muted = pb.Muted();
    if (IconButton("vol", muted ? ICON_MUTE : ICON_VOLUME, nullptr, muted, nullptr, ImVec2(h, h))) ImGui::OpenPopup("##volume");
    ItemTooltip(muted ? "Muted" : "Volume", nullptr);
    if (BeginPopover("##volume")) {
        bool m = muted;
        if (ImGui::Checkbox(str::kMute, &m)) pb.SetMuted(m);
        float v = pb.Volume() * 100.0f;
        ImGui::SetNextItemWidth(Dp(160));
        if (ImGui::SliderFloat("##volume_slider", &v, 0.0f, 200.0f, "%.0f %%")) pb.SetVolume(v / 100.0f);
        EndPopover();
    }
}

void PreviewPanel::Draw(EditorState& state) {
    m_wants_redraw = false;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float transport_h = Dp(48);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area(avail.x, std::max(1.0f, avail.y - transport_h));
    DrawImage(state, origin, area);
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(tp, ImVec2(tp.x + avail.x, tp.y), col::border);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Dp(8));
    DrawTransport(state);
    if (m_gizmo != Gizmo::None || m_time_edit || !m_text_clip.empty()) m_wants_redraw = true;
}

} // namespace ct::ui
