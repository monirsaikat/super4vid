#include <ct/project/serialize.h>

#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/version.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <locale.h>

namespace ct::project {

namespace {

_locale_t CLocale() {
    static _locale_t loc = _create_locale(LC_NUMERIC, "C");
    return loc;
}

// ---- enum names (docs/04 section 3) ---------------------------------------------------------------------------

constexpr const char* kBackgroundKinds[] = {"solid", "gradient", "image"};
constexpr const char* kMediaKinds[] = {"recording", "video", "audio", "image"};
constexpr const char* kTrackKinds[] = {"video", "audio", "annotations"};
constexpr const char* kSources[] = {"screen", "camera", "mic", "system", "default"};
constexpr const char* kClickEffects[] = {"none", "ripple", "highlight", "both"};
constexpr const char* kKeyPositions[] = {"bottom-center", "bottom-left", "bottom-right", "top-center"};
constexpr const char* kKeyStyles[] = {"pill", "keycap"};
constexpr const char* kPipCorners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
constexpr const char* kPipMasks[] = {"none", "rounded", "circle"};
constexpr const char* kAnnotationTypes[] = {"text", "callout", "arrow", "rect", "ellipse", "highlight", "blur", "keystrokes"};
constexpr const char* kAnims[] = {"none", "fade", "pop", "slide"};
constexpr const char* kTransitionKinds[] = {"crossfade", "dip", "slide"};
constexpr const char* kDirections[] = {"left", "right", "up", "down"};
constexpr const char* kCameraModes[] = {"manual", "auto"};

template <class E, size_t N>
Json EnumJson(E e, const char* const (&names)[N]) {
    const size_t i = static_cast<size_t>(e);
    return Json(i < N ? names[i] : names[0]);
}

template <class E, size_t N>
E EnumFrom(const Json& v, const char* const (&names)[N], E def) {
    if (!v.IsString()) return def;
    for (size_t i = 0; i < N; ++i)
        if (v.AsString() == names[i]) return static_cast<E>(i);
    return def;
}

// ---- writer helpers -------------------------------------------------------------------------------------------

Json F(float v) { return Json(RoundFloat(v)); }
Json T(TimeUs v) { return Json(static_cast<int64_t>(v)); }
Json Color(const ColorF& c) { return Json(c.ToHex()); }

// Appends the preserved unknown keys after the known ones (a key the writer already emitted wins).
void AppendExtra(Json& o, const Json& extra) {
    if (!extra.IsObject()) return;
    for (const auto& [k, v] : extra.Items())
        if (!o.Has(k)) o.Set(k, v);
}

Json StringArray(const std::vector<std::string>& v) {
    Json a = Json::Array();
    for (const std::string& s : v) a.Push(Json(s));
    return a;
}

Json ShadowJson(const Shadow& s) {
    Json o = Json::Object();
    o.Set("enabled", s.enabled);
    o.Set("blur_px", F(s.blur_px));
    o.Set("offset_y_px", F(s.offset_y_px));
    o.Set("opacity", F(s.opacity));
    AppendExtra(o, s.extra);
    return o;
}

Json CanvasJson(const Canvas& c) {
    Json o = Json::Object();
    o.Set("width", c.width);
    o.Set("height", c.height);
    o.Set("fps_num", c.fps_num);
    o.Set("fps_den", c.fps_den);
    Json bg = Json::Object();
    bg.Set("kind", EnumJson(c.background.kind, kBackgroundKinds));
    bg.Set("color_a", Color(c.background.color_a));
    bg.Set("color_b", Color(c.background.color_b));
    bg.Set("angle_deg", F(c.background.angle_deg));
    bg.Set("image", c.background.image.empty() ? Json(nullptr) : Json(c.background.image));
    bg.Set("blur", F(c.background.blur));
    AppendExtra(bg, c.background.extra);
    o.Set("background", std::move(bg));
    o.Set("padding_px", c.padding_px);
    o.Set("corner_radius_px", c.corner_radius_px);
    o.Set("shadow", ShadowJson(c.shadow));
    AppendExtra(o, c.extra);
    return o;
}

Json MediaJson(const Media& m) {
    Json o = Json::Object();
    o.Set("id", m.id);
    o.Set("kind", EnumJson(m.kind, kMediaKinds));
    o.Set("path", m.path);
    o.Set("duration_us", T(m.duration_us));
    if (m.kind != Media::Kind::Audio) {
        Json v = Json::Object();
        v.Set("width", m.width);
        v.Set("height", m.height);
        v.Set("fps_num", m.fps_num);
        v.Set("fps_den", m.fps_den);
        o.Set("video", std::move(v));
    }
    o.Set("audio_streams", StringArray(m.audio_streams));
    o.Set("has_events", m.has_events);
    o.Set("has_camera", m.has_camera);
    AppendExtra(o, m.extra);
    return o;
}

Json TrackJson(const Track& t) {
    Json o = Json::Object();
    o.Set("id", t.id);
    o.Set("kind", EnumJson(t.kind, kTrackKinds));
    o.Set("name", t.name);
    o.Set("muted", t.muted);
    o.Set("locked", t.locked);
    o.Set("hidden", t.hidden);
    o.Set("height_px", t.height_px);
    AppendExtra(o, t.extra);
    return o;
}

Json CursorJson(const CursorStyle& c) {
    Json o = Json::Object();
    o.Set("visible", c.visible);
    o.Set("scale", F(c.scale));
    o.Set("smoothing", F(c.smoothing));
    o.Set("click_effect", EnumJson(c.click, kClickEffects));
    o.Set("click_color", Color(c.click_color));
    o.Set("hide_when_idle_us", T(c.hide_when_idle_us));
    o.Set("spotlight", c.spotlight);
    o.Set("shadow", c.shadow);
    AppendExtra(o, c.extra);
    return o;
}

Json KeystrokesJson(const KeystrokeStyle& k) {
    Json o = Json::Object();
    o.Set("enabled", k.enabled);
    o.Set("position", EnumJson(k.pos, kKeyPositions));
    o.Set("style", EnumJson(k.style, kKeyStyles));
    o.Set("hold_ms", k.hold_ms);
    AppendExtra(o, k.extra);
    return o;
}

Json PipJson(const Pip& p) {
    Json o = Json::Object();
    o.Set("corner", EnumJson(p.corner, kPipCorners));
    o.Set("size", F(p.size));
    o.Set("margin_px", p.margin_px);
    o.Set("mask", EnumJson(p.mask, kPipMasks));
    o.Set("radius_px", p.radius_px);
    o.Set("shadow", p.shadow);
    o.Set("mirror", p.mirror);
    AppendExtra(o, p.extra);
    return o;
}

Json AnnotationJson(const Annotation& a) {
    Json o = Json::Object();
    o.Set("type", EnumJson(a.type, kAnnotationTypes));
    o.Set("text", a.text);
    o.Set("font", a.font);
    o.Set("size_px", a.size_px);
    o.Set("weight", a.weight);
    o.Set("color", Color(a.color));
    o.Set("fill", Color(a.fill));
    o.Set("stroke", Color(a.stroke));
    o.Set("stroke_px", a.stroke_px);
    o.Set("radius_px", a.radius_px);
    Json r = Json::Object();
    r.Set("x", F(a.rect.x));
    r.Set("y", F(a.rect.y));
    r.Set("w", F(a.rect.w));
    r.Set("h", F(a.rect.h));
    o.Set("rect", std::move(r));
    Json pt = Json::Object();
    pt.Set("x", F(a.pointer.x));
    pt.Set("y", F(a.pointer.y));
    o.Set("pointer", std::move(pt));
    o.Set("arrow_head_px", F(a.arrow_head_px));
    o.Set("blur_px", a.blur_px);
    o.Set("pixelate", a.pixelate);
    o.Set("anim_in", EnumJson(a.anim_in, kAnims));
    o.Set("anim_out", EnumJson(a.anim_out, kAnims));
    o.Set("anim_ms", a.anim_ms);
    AppendExtra(o, a.extra);
    return o;
}

Json ClipJson(const Clip& c) {
    Json o = Json::Object();
    o.Set("id", c.id);
    o.Set("track", c.track);
    o.Set("media", c.media.empty() ? Json(nullptr) : Json(c.media));
    o.Set("source", EnumJson(c.source, kSources));
    o.Set("start_us", T(c.start_us));
    o.Set("in_us", T(c.in_us));
    o.Set("out_us", T(c.out_us));
    o.Set("speed", Json(c.speed));
    o.Set("name", c.name);
    const bool has_color = std::lround(std::clamp(c.color.a, 0.0f, 1.0f) * 255.0f) > 0;
    o.Set("color", has_color ? Color(c.color) : Json(nullptr));
    Json tr = Json::Object();
    tr.Set("x", F(c.transform.x));
    tr.Set("y", F(c.transform.y));
    tr.Set("scale", F(c.transform.scale));
    tr.Set("rotation_deg", F(c.transform.rotation_deg));
    tr.Set("opacity", F(c.transform.opacity));
    tr.Set("anchor", "center");
    AppendExtra(tr, c.transform.extra);
    o.Set("transform", std::move(tr));
    Json cr = Json::Object();
    cr.Set("left", c.crop.left);
    cr.Set("top", c.crop.top);
    cr.Set("right", c.crop.right);
    cr.Set("bottom", c.crop.bottom);
    AppendExtra(cr, c.crop.extra);
    o.Set("crop", std::move(cr));
    o.Set("corner_radius_px", c.corner_radius_px ? Json(*c.corner_radius_px) : Json(nullptr));
    o.Set("shadow", c.shadow ? ShadowJson(*c.shadow) : Json(nullptr));
    Json au = Json::Object();
    au.Set("gain_db", F(c.audio.gain_db));
    au.Set("muted", c.audio.muted);
    au.Set("fade_in_us", T(c.audio.fade_in_us));
    au.Set("fade_out_us", T(c.audio.fade_out_us));
    au.Set("normalize", c.audio.normalize);
    au.Set("noise_gate_db", c.audio.noise_gate_db ? F(*c.audio.noise_gate_db) : Json(nullptr));
    AppendExtra(au, c.audio.extra);
    o.Set("audio", std::move(au));
    Json kf = Json::Object();
    for (const auto& [prop, track] : c.keyframes) {
        if (track.Empty()) continue;
        Json arr = Json::Array();
        for (const Keyframe<float>& k : track.keys) {
            Json e = Json::Object();
            e.Set("t_us", T(k.t_us));
            e.Set("v", F(k.v));
            e.Set("ease", EaseName(k.ease));
            arr.Push(std::move(e));
        }
        kf.Set(prop, std::move(arr));
    }
    o.Set("keyframes", std::move(kf));
    if (c.source == Clip::Source::Screen) {
        o.Set("cursor", CursorJson(c.cursor));
        o.Set("keystrokes", KeystrokesJson(c.keystrokes));
    }
    if (c.pip) o.Set("pip", PipJson(*c.pip));
    if (c.annotation) o.Set("annotation", AnnotationJson(*c.annotation));
    AppendExtra(o, c.extra);
    return o;
}

Json TransitionJson(const Transition& t) {
    Json o = Json::Object();
    o.Set("track", t.track);
    Json between = Json::Array();
    between.Push(Json(t.a));
    between.Push(Json(t.b));
    o.Set("between", std::move(between));
    o.Set("kind", EnumJson(t.kind, kTransitionKinds));
    o.Set("duration_us", T(t.duration_us));
    o.Set("color", Color(t.color));
    o.Set("direction", EnumJson(t.dir, kDirections));
    AppendExtra(o, t.extra);
    return o;
}

Json CameraJson(const Camera& c) {
    Json o = Json::Object();
    o.Set("mode", EnumJson(c.mode, kCameraModes));
    Json keys = Json::Array();
    for (const CameraKey& k : c.keys) {
        Json e = Json::Object();
        e.Set("t_us", T(k.t_us));
        e.Set("zoom", F(k.zoom));
        e.Set("cx", F(k.cx));
        e.Set("cy", F(k.cy));
        e.Set("ease", EaseName(k.ease));
        if (k.generated) e.Set("generated", true);
        keys.Push(std::move(e));
    }
    o.Set("keyframes", std::move(keys));
    const AutoZoomParams& a = c.auto_params;
    Json ap = Json::Object();
    ap.Set("zoom", F(a.zoom));
    ap.Set("min_hold_us", T(a.min_hold_us));
    ap.Set("lead_us", T(a.lead_us));
    ap.Set("ease_ms", a.ease_ms);
    ap.Set("follow", a.follow);
    ap.Set("follow_deadzone", F(a.follow_deadzone));
    Json trig = Json::Array();
    if (a.on_click) trig.Push(Json("click"));
    if (a.on_typing) trig.Push(Json("typing"));
    ap.Set("trigger_on", std::move(trig));
    AppendExtra(ap, a.extra);
    o.Set("auto", std::move(ap));
    AppendExtra(o, c.extra);
    return o;
}

Json MarkerJson(const Marker& m) {
    Json o = Json::Object();
    o.Set("t_us", T(m.t_us));
    o.Set("label", m.label);
    o.Set("color", Color(m.color));
    AppendExtra(o, m.extra);
    return o;
}

Json UiJson(const UiState& u) {
    Json o = Json::Object();
    o.Set("playhead_us", T(u.playhead_us));
    o.Set("zoom", Json(u.zoom));
    o.Set("scroll_us", T(u.scroll_us));
    o.Set("selected", StringArray(u.selected));
    if (u.loop) {
        Json l = Json::Array();
        l.Push(T(u.loop->first));
        l.Push(T(u.loop->second));
        o.Set("loop", std::move(l));
    } else {
        o.Set("loop", Json(nullptr));
    }
    AppendExtra(o, u.extra);
    return o;
}

// ---- reader helpers -------------------------------------------------------------------------------------------

Json ExtraOf(const Json& o, std::initializer_list<std::string_view> known) {
    Json extra;
    if (!o.IsObject()) return extra;
    for (const auto& [k, v] : o.Items()) {
        bool is_known = false;
        for (std::string_view kk : known)
            if (k == kk) {
                is_known = true;
                break;
            }
        if (!is_known && !extra.Has(k)) extra.Set(k, v);
    }
    return extra;
}

float GetF(const Json& o, std::string_view key, float def) {
    const Json& v = o[key];
    return v.IsNumber() ? static_cast<float>(v.AsNumber()) : def;
}

TimeUs GetT(const Json& o, std::string_view key, TimeUs def) { return o.Get<int64_t>(key, def); }

ColorF GetColor(const Json& o, std::string_view key, const ColorF& def) {
    const Json& v = o[key];
    if (!v.IsString()) return def;
    const std::string& s = v.AsString();
    if (s.size() != 7 && s.size() != 9) return def;
    if (s[0] != '#') return def;
    for (size_t i = 1; i < s.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return def;
    return ColorF::FromHex(s);
}

std::vector<std::string> GetStrings(const Json& v) {
    std::vector<std::string> out;
    for (const Json& e : v.Elements())
        if (e.IsString()) out.push_back(e.AsString());
    return out;
}

Shadow ShadowFrom(const Json& o) {
    Shadow s;
    s.enabled = o.Get("enabled", s.enabled);
    s.blur_px = GetF(o, "blur_px", s.blur_px);
    s.offset_y_px = GetF(o, "offset_y_px", s.offset_y_px);
    s.opacity = GetF(o, "opacity", s.opacity);
    s.extra = ExtraOf(o, {"enabled", "blur_px", "offset_y_px", "opacity"});
    return s;
}

Canvas CanvasFrom(const Json& o) {
    Canvas c;
    c.width = o.Get("width", c.width);
    c.height = o.Get("height", c.height);
    c.fps_num = o.Get("fps_num", c.fps_num);
    c.fps_den = o.Get("fps_den", c.fps_den);
    const Json& bg = o["background"];
    if (bg.IsObject()) {
        Background& b = c.background;
        b.kind = EnumFrom(bg["kind"], kBackgroundKinds, b.kind);
        b.color_a = GetColor(bg, "color_a", b.color_a);
        b.color_b = GetColor(bg, "color_b", b.color_b);
        b.angle_deg = GetF(bg, "angle_deg", b.angle_deg);
        b.image = bg.Get<std::string>("image", "");
        b.blur = GetF(bg, "blur", b.blur);
        b.extra = ExtraOf(bg, {"kind", "color_a", "color_b", "angle_deg", "image", "blur"});
    }
    c.padding_px = o.Get("padding_px", c.padding_px);
    c.corner_radius_px = o.Get("corner_radius_px", c.corner_radius_px);
    if (o["shadow"].IsObject()) c.shadow = ShadowFrom(o["shadow"]);
    c.extra = ExtraOf(o, {"width", "height", "fps_num", "fps_den", "background", "padding_px", "corner_radius_px", "shadow"});
    return c;
}

Media MediaFrom(const Json& o) {
    Media m;
    m.id = o.Get<std::string>("id", "");
    m.kind = EnumFrom(o["kind"], kMediaKinds, Media::Kind::Video);
    m.path = o.Get<std::string>("path", "");
    m.duration_us = GetT(o, "duration_us", 0);
    const Json& v = o["video"];
    m.width = v.Get("width", 0);
    m.height = v.Get("height", 0);
    m.fps_num = v.Get("fps_num", 0);
    m.fps_den = v.Get("fps_den", 1);
    m.audio_streams = GetStrings(o["audio_streams"]);
    m.has_events = o.Get("has_events", false);
    m.has_camera = o.Get("has_camera", false);
    m.extra = ExtraOf(o, {"id", "kind", "path", "duration_us", "video", "audio_streams", "has_events", "has_camera"});
    return m;
}

Track TrackFrom(const Json& o) {
    Track t;
    t.id = o.Get<std::string>("id", "");
    t.kind = EnumFrom(o["kind"], kTrackKinds, Track::Kind::Video);
    t.name = o.Get<std::string>("name", "");
    t.muted = o.Get("muted", false);
    t.locked = o.Get("locked", false);
    t.hidden = o.Get("hidden", false);
    t.height_px = o.Get("height_px", 0);
    t.extra = ExtraOf(o, {"id", "kind", "name", "muted", "locked", "hidden", "height_px"});
    return t;
}

CursorStyle CursorFrom(const Json& o) {
    CursorStyle c;
    c.visible = o.Get("visible", c.visible);
    c.scale = GetF(o, "scale", c.scale);
    c.smoothing = GetF(o, "smoothing", c.smoothing);
    c.click = EnumFrom(o["click_effect"], kClickEffects, c.click);
    c.click_color = GetColor(o, "click_color", c.click_color);
    c.hide_when_idle_us = GetT(o, "hide_when_idle_us", c.hide_when_idle_us);
    c.spotlight = o.Get("spotlight", c.spotlight);
    c.shadow = o.Get("shadow", c.shadow);
    c.extra = ExtraOf(o, {"visible", "scale", "smoothing", "click_effect", "click_color", "hide_when_idle_us", "spotlight", "shadow"});
    return c;
}

KeystrokeStyle KeystrokesFrom(const Json& o) {
    KeystrokeStyle k;
    k.enabled = o.Get("enabled", k.enabled);
    k.pos = EnumFrom(o["position"], kKeyPositions, k.pos);
    k.style = EnumFrom(o["style"], kKeyStyles, k.style);
    k.hold_ms = o.Get("hold_ms", k.hold_ms);
    k.extra = ExtraOf(o, {"enabled", "position", "style", "hold_ms"});
    return k;
}

Pip PipFrom(const Json& o) {
    Pip p;
    p.corner = EnumFrom(o["corner"], kPipCorners, p.corner);
    p.size = GetF(o, "size", p.size);
    p.margin_px = o.Get("margin_px", p.margin_px);
    p.mask = EnumFrom(o["mask"], kPipMasks, p.mask);
    p.radius_px = o.Get("radius_px", p.radius_px);
    p.shadow = o.Get("shadow", p.shadow);
    p.mirror = o.Get("mirror", p.mirror);
    p.extra = ExtraOf(o, {"corner", "size", "margin_px", "mask", "radius_px", "shadow", "mirror"});
    return p;
}

Annotation AnnotationFrom(const Json& o) {
    Annotation a;
    a.type = EnumFrom(o["type"], kAnnotationTypes, a.type);
    a.text = o.Get<std::string>("text", a.text);
    a.font = o.Get<std::string>("font", a.font);
    a.size_px = o.Get("size_px", a.size_px);
    a.weight = o.Get("weight", a.weight);
    a.color = GetColor(o, "color", a.color);
    a.fill = GetColor(o, "fill", a.fill);
    a.stroke = GetColor(o, "stroke", a.stroke);
    a.stroke_px = o.Get("stroke_px", a.stroke_px);
    a.radius_px = o.Get("radius_px", a.radius_px);
    const Json& r = o["rect"];
    a.rect = RectF{GetF(r, "x", a.rect.x), GetF(r, "y", a.rect.y), GetF(r, "w", a.rect.w), GetF(r, "h", a.rect.h)};
    const Json& pt = o["pointer"];
    a.pointer = PointF{GetF(pt, "x", a.pointer.x), GetF(pt, "y", a.pointer.y)};
    a.arrow_head_px = GetF(o, "arrow_head_px", a.arrow_head_px);
    a.blur_px = o.Get("blur_px", a.blur_px);
    a.pixelate = o.Get("pixelate", a.pixelate);
    a.anim_in = EnumFrom(o["anim_in"], kAnims, a.anim_in);
    a.anim_out = EnumFrom(o["anim_out"], kAnims, a.anim_out);
    a.anim_ms = o.Get("anim_ms", a.anim_ms);
    a.extra = ExtraOf(o, {"type", "text", "font", "size_px", "weight", "color", "fill", "stroke", "stroke_px", "radius_px",
                          "rect", "pointer", "arrow_head_px", "blur_px", "pixelate", "anim_in", "anim_out", "anim_ms"});
    return a;
}

Clip ClipFrom(const Json& o) {
    Clip c;
    c.id = o.Get<std::string>("id", "");
    c.track = o.Get<std::string>("track", "");
    c.media = o.Get<std::string>("media", "");
    c.source = EnumFrom(o["source"], kSources, Clip::Source::Default);
    c.start_us = GetT(o, "start_us", 0);
    c.in_us = GetT(o, "in_us", 0);
    c.out_us = GetT(o, "out_us", 0);
    c.speed = o.Get("speed", 1.0);
    c.name = o.Get<std::string>("name", "");
    c.color = GetColor(o, "color", ColorF{0, 0, 0, 0});
    const Json& tr = o["transform"];
    c.transform.x = GetF(tr, "x", 0);
    c.transform.y = GetF(tr, "y", 0);
    c.transform.scale = GetF(tr, "scale", 1);
    c.transform.rotation_deg = GetF(tr, "rotation_deg", 0);
    c.transform.opacity = GetF(tr, "opacity", 1);
    c.transform.extra = ExtraOf(tr, {"x", "y", "scale", "rotation_deg", "opacity", "anchor"});
    const Json& cr = o["crop"];
    c.crop.left = cr.Get("left", 0);
    c.crop.top = cr.Get("top", 0);
    c.crop.right = cr.Get("right", 0);
    c.crop.bottom = cr.Get("bottom", 0);
    c.crop.extra = ExtraOf(cr, {"left", "top", "right", "bottom"});
    if (o["corner_radius_px"].IsNumber()) c.corner_radius_px = o["corner_radius_px"].AsInt();
    if (o["shadow"].IsObject()) c.shadow = ShadowFrom(o["shadow"]);
    const Json& au = o["audio"];
    c.audio.gain_db = GetF(au, "gain_db", 0);
    c.audio.muted = au.Get("muted", false);
    c.audio.fade_in_us = GetT(au, "fade_in_us", 0);
    c.audio.fade_out_us = GetT(au, "fade_out_us", 0);
    c.audio.normalize = au.Get("normalize", false);
    if (au["noise_gate_db"].IsNumber()) c.audio.noise_gate_db = static_cast<float>(au["noise_gate_db"].AsNumber());
    c.audio.extra = ExtraOf(au, {"gain_db", "muted", "fade_in_us", "fade_out_us", "normalize", "noise_gate_db"});
    const Json& kf = o["keyframes"];
    if (kf.IsObject()) {
        for (const auto& [prop, arr] : kf.Items()) {
            KeyTrack<float> track;
            for (const Json& e : arr.Elements()) {
                if (!e["t_us"].IsNumber() || !e["v"].IsNumber()) continue;
                Ease ease = Ease::Linear;
                ParseEase(e["ease"].AsString(), &ease);
                track.Set(GetT(e, "t_us", 0), GetF(e, "v", 0), ease);   // sorts and de-duplicates
            }
            if (!track.Empty()) c.keyframes[prop] = std::move(track);
        }
    }
    if (o["cursor"].IsObject()) c.cursor = CursorFrom(o["cursor"]);
    if (o["keystrokes"].IsObject()) c.keystrokes = KeystrokesFrom(o["keystrokes"]);
    if (o["pip"].IsObject()) c.pip = PipFrom(o["pip"]);
    if (o["annotation"].IsObject()) c.annotation = AnnotationFrom(o["annotation"]);
    c.extra = ExtraOf(o, {"id", "track", "media", "kind", "source", "start_us", "in_us", "out_us", "speed", "name", "color",
                          "transform", "crop", "corner_radius_px", "shadow", "audio", "keyframes", "cursor", "keystrokes",
                          "pip", "annotation"});
    return c;
}

Transition TransitionFrom(const Json& o) {
    Transition t;
    t.track = o.Get<std::string>("track", "");
    t.a = o["between"][size_t(0)].AsString();
    t.b = o["between"][size_t(1)].AsString();
    t.kind = EnumFrom(o["kind"], kTransitionKinds, t.kind);
    t.duration_us = GetT(o, "duration_us", t.duration_us);
    t.color = GetColor(o, "color", t.color);
    t.dir = EnumFrom(o["direction"], kDirections, t.dir);
    t.extra = ExtraOf(o, {"track", "between", "kind", "duration_us", "color", "direction"});
    return t;
}

Camera CameraFrom(const Json& o) {
    Camera c;
    c.mode = EnumFrom(o["mode"], kCameraModes, c.mode);
    for (const Json& e : o["keyframes"].Elements()) {
        if (!e["t_us"].IsNumber()) continue;
        CameraKey k;
        k.t_us = GetT(e, "t_us", 0);
        k.zoom = GetF(e, "zoom", 1);
        k.cx = GetF(e, "cx", 0.5f);
        k.cy = GetF(e, "cy", 0.5f);
        ParseEase(e["ease"].AsString(), &k.ease);
        k.generated = e.Get("generated", false);
        c.SetKey(k);   // sorts and de-duplicates
    }
    const Json& a = o["auto"];
    AutoZoomParams& p = c.auto_params;
    p.zoom = GetF(a, "zoom", p.zoom);
    p.min_hold_us = GetT(a, "min_hold_us", p.min_hold_us);
    p.lead_us = GetT(a, "lead_us", p.lead_us);
    p.ease_ms = a.Get("ease_ms", p.ease_ms);
    p.follow = a.Get("follow", p.follow);
    p.follow_deadzone = GetF(a, "follow_deadzone", p.follow_deadzone);
    if (a["trigger_on"].IsArray()) {
        const std::vector<std::string> trig = GetStrings(a["trigger_on"]);
        p.on_click = std::find(trig.begin(), trig.end(), "click") != trig.end();
        p.on_typing = std::find(trig.begin(), trig.end(), "typing") != trig.end();
    }
    p.extra = ExtraOf(a, {"zoom", "min_hold_us", "lead_us", "ease_ms", "follow", "follow_deadzone", "trigger_on"});
    c.extra = ExtraOf(o, {"mode", "keyframes", "auto"});
    return c;
}

Marker MarkerFrom(const Json& o) {
    Marker m;
    m.t_us = GetT(o, "t_us", 0);
    m.label = o.Get<std::string>("label", "");
    m.color = GetColor(o, "color", m.color);
    m.extra = ExtraOf(o, {"t_us", "label", "color"});
    return m;
}

UiState UiFrom(const Json& o) {
    UiState u;
    u.playhead_us = GetT(o, "playhead_us", 0);
    u.zoom = o.Get("zoom", 1.0);
    u.scroll_us = GetT(o, "scroll_us", 0);
    u.selected = GetStrings(o["selected"]);
    const Json& l = o["loop"];
    if (l.IsArray() && l.Size() == 2 && l[size_t(0)].IsNumber() && l[size_t(1)].IsNumber())
        u.loop = std::make_pair(l[size_t(0)].AsInt64(), l[size_t(1)].AsInt64());
    u.extra = ExtraOf(o, {"playhead_us", "zoom", "scroll_us", "selected", "loop"});
    return u;
}

bool IsAbsolutePath(std::string_view s) {
    if (s.size() >= 2 && s[1] == ':') return true;
    return s.size() >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/');
}

std::string StoredPath(const std::string& project_dir, const std::string& media_path) {
    if (media_path.empty() || !IsAbsolutePath(media_path)) return media_path;
    const std::string abs = fs::Absolute(media_path);
    std::string rel = fs::Relative(project_dir, abs);
    if (IsAbsolutePath(rel)) return media_path;   // other drive/share: keep it as the user gave it
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

} // namespace

double RoundFloat(float v) {
    if (!std::isfinite(v)) return 0.0;
    if (v == 0.0f) return 0.0;   // also turns -0 into 0
    char buf[32];
    _snprintf_l(buf, sizeof(buf), "%.6g", CLocale(), static_cast<double>(v));
    return _strtod_l(buf, nullptr, CLocale());
}

Json ToJsonValue(const Project& p) {
    Json o = Json::Object();
    o.Set("format", "ctproj");
    o.Set("version", p.version);
    o.Set("app_version", CT_VERSION_STRING);
    o.Set("id", p.id);
    o.Set("name", p.name);
    o.Set("canvas", CanvasJson(p.canvas));
    Json media = Json::Array();
    for (const Media& m : p.media) media.Push(MediaJson(m));
    o.Set("media", std::move(media));
    Json tracks = Json::Array();
    for (const Track& t : p.tracks) tracks.Push(TrackJson(t));
    o.Set("tracks", std::move(tracks));
    Json clips = Json::Array();
    for (const Clip& c : p.clips) clips.Push(ClipJson(c));
    o.Set("clips", std::move(clips));
    Json transitions = Json::Array();
    for (const Transition& t : p.transitions) transitions.Push(TransitionJson(t));
    o.Set("transitions", std::move(transitions));
    o.Set("camera", CameraJson(p.camera));
    Json markers = Json::Array();
    for (const Marker& m : p.markers) markers.Push(MarkerJson(m));
    o.Set("markers", std::move(markers));
    o.Set("ui", UiJson(p.ui));
    AppendExtra(o, p.extra);
    return o;
}

std::string ToJson(const Project& p) {
    std::string s = ToJsonValue(p).Dump(2);
    s.push_back('\n');
    return s;
}

bool Migrate(Json& j, int from_version, Err* err) {
    if (from_version < 0) {
        if (err) err->Set("Invalid project version");
        return false;
    }
    if (from_version < 1) {
        // v0 -> v1
        Json& clips = j["clips"];
        for (size_t i = 0; i < clips.Size(); ++i) {
            Json& c = clips[i];
            if (c.Has("duration_us") && !c.Has("out_us")) {
                const double speed = c.Get("speed", 1.0);
                const int64_t in = c.Get<int64_t>("in_us", 0);
                const int64_t dur = c.Get<int64_t>("duration_us", 0);
                c.Set("out_us", Json(static_cast<int64_t>(in + std::llround(static_cast<double>(dur) * speed))));
            }
            c.Erase("duration_us");
        }
        Json& canvas = j["canvas"];
        if (canvas["background"].IsString()) {
            const std::string color = canvas["background"].AsString();
            Json bg = Json::Object();
            bg.Set("kind", "solid");
            bg.Set("color_a", color);
            bg.Set("color_b", color);
            canvas.Set("background", std::move(bg));
        }
        Json& camera = j["camera"];
        if (camera.Has("keys") && !camera.Has("keyframes")) {
            Json keys = camera["keys"];
            camera.Erase("keys");
            camera.Set("keyframes", std::move(keys));
        }
    }
    j.Set("version", kProjectVersion);
    return true;
}

bool FromJsonValue(Json j, Project& p, Err* err) {
    if (!j.IsObject()) {
        if (err) err->Set("The project file does not contain a JSON object");
        return false;
    }
    if (j.Has("format") && j["format"].AsString() != "ctproj") {
        if (err) err->Set("Not a Camtesia project (format is '" + j["format"].AsString() + "')");
        return false;
    }
    const int version = j.Has("version") ? j.Get("version", 0) : 0;
    if (version < kProjectVersion && !Migrate(j, version, err)) return false;

    const std::string keep_path = p.path;
    p = Project{};
    p.path = keep_path;
    p.version = std::max(version, kProjectVersion);   // newer files keep their number (best-effort load)
    p.id = j.Get<std::string>("id", "");
    p.name = j.Get<std::string>("name", "");
    if (j["canvas"].IsObject()) p.canvas = CanvasFrom(j["canvas"]);
    for (const Json& m : j["media"].Elements())
        if (m.IsObject()) p.media.push_back(MediaFrom(m));
    for (const Json& t : j["tracks"].Elements())
        if (t.IsObject()) p.tracks.push_back(TrackFrom(t));
    for (const Json& c : j["clips"].Elements())
        if (c.IsObject()) p.clips.push_back(ClipFrom(c));
    for (const Json& t : j["transitions"].Elements())
        if (t.IsObject()) p.transitions.push_back(TransitionFrom(t));
    if (j["camera"].IsObject()) p.camera = CameraFrom(j["camera"]);
    for (const Json& m : j["markers"].Elements())
        if (m.IsObject()) p.markers.push_back(MarkerFrom(m));
    std::stable_sort(p.markers.begin(), p.markers.end(), [](const Marker& a, const Marker& b) { return a.t_us < b.t_us; });
    if (j["ui"].IsObject()) p.ui = UiFrom(j["ui"]);
    p.extra = ExtraOf(j, {"format", "version", "app_version", "id", "name", "canvas", "media", "tracks", "clips",
                          "transitions", "camera", "markers", "ui"});
    if (p.id.empty()) p.id = Uuid4();
    p.RecomputeNextId();
    return true;
}

bool FromJson(std::string_view text, Project& p, Err* err) {
    Json j;
    std::string perr;
    if (!Json::Parse(text, j, &perr)) {
        if (err) err->Set("The project file is damaged (" + perr + ")");
        return false;
    }
    return FromJsonValue(std::move(j), p, err);
}

bool Save(const Project& p, std::string_view path, Err* err) {
    const std::string abs = fs::Absolute(path);
    const std::string dir(PathParent(abs));
    Json j = ToJsonValue(p);
    Json& media = j["media"];
    for (size_t i = 0; i < media.Size() && i < p.media.size(); ++i)
        media[i].Set("path", StoredPath(dir, p.media[i].path));
    std::string text = j.Dump(2);
    text.push_back('\n');
    if (!fs::WriteAtomic(abs, text)) {
        if (err) err->Set("Could not save the project to " + abs);
        return false;
    }
    return true;
}

bool Load(std::string_view path, Project& p, Err* err) {
    const std::string abs = fs::Absolute(path);
    std::string text;
    if (!fs::ReadAll(abs, text)) {
        if (err) err->Set("Could not open " + abs);
        return false;
    }
    Project loaded;
    if (!FromJson(text, loaded, err)) return false;
    const std::string dir(PathParent(abs));
    for (Media& m : loaded.media) {
        if (m.path.empty()) {
            m.missing = true;
            continue;
        }
        const std::string resolved = IsAbsolutePath(m.path) ? m.path : fs::Absolute(PathJoin(dir, m.path));
        if (fs::Exists(resolved)) {
            m.path = resolved;
            continue;
        }
        // Moved together with the project folder: look next to the project file.
        const std::string beside = PathJoin(dir, PathName(m.path));
        if (!PathName(m.path).empty() && fs::Exists(beside)) {
            m.path = fs::Absolute(beside);
            continue;
        }
        m.path = resolved;
        m.missing = true;
    }
    std::string why;
    if (!loaded.Validate(&why)) {
        if (err) err->Set("The project file is damaged: " + why);
        return false;
    }
    loaded.path = abs;
    p = std::move(loaded);
    return true;
}

} // namespace ct::project
