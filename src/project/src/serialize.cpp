#include <ct/project/serialize.h>

#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/version.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace ct::project {

namespace {

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
const char* EnumName(E e, const char* const (&names)[N]) {
    const size_t i = static_cast<size_t>(e);
    return i < N ? names[i] : names[0];
}

template <class E, size_t N>
E EnumFrom(const Json& v, const char* const (&names)[N], E def) {
    if (!v.IsString()) return def;
    for (size_t i = 0; i < N; ++i)
        if (v.AsString() == names[i]) return static_cast<E>(i);
    return def;
}

// ---- known keys per object (reader: what goes to `extra`; writer: extras never shadow these) -------------------

using Keys = std::initializer_list<std::string_view>;
const Keys kShadowKeys = {"enabled", "blur_px", "offset_y_px", "opacity"};
const Keys kBackgroundKeys = {"kind", "color_a", "color_b", "angle_deg", "image", "blur"};
const Keys kCanvasKeys = {"width", "height", "fps_num", "fps_den", "background", "padding_px", "corner_radius_px", "shadow"};
const Keys kMediaKeys = {"id", "kind", "path", "duration_us", "video", "audio_streams", "has_events", "has_camera"};
const Keys kTrackKeys = {"id", "kind", "name", "muted", "locked", "hidden", "height_px"};
const Keys kCursorKeys = {"visible", "scale", "smoothing", "click_effect", "click_color", "hide_when_idle_us", "spotlight", "shadow"};
const Keys kKeystrokeKeys = {"enabled", "position", "style", "hold_ms"};
const Keys kPipKeys = {"corner", "size", "margin_px", "mask", "radius_px", "shadow", "mirror"};
const Keys kAnnotationKeys = {"type", "text", "font", "size_px", "weight", "color", "fill", "stroke", "stroke_px", "radius_px",
                              "rect", "pointer", "arrow_head_px", "blur_px", "pixelate", "anim_in", "anim_out", "anim_ms"};
const Keys kTransformKeys = {"x", "y", "scale", "rotation_deg", "opacity", "anchor"};
const Keys kCropKeys = {"left", "top", "right", "bottom"};
const Keys kAudioKeys = {"gain_db", "muted", "fade_in_us", "fade_out_us", "normalize", "noise_gate_db"};
const Keys kClipKeys = {"id", "track", "media", "kind", "source", "start_us", "in_us", "out_us", "speed", "name", "color",
                        "transform", "crop", "corner_radius_px", "shadow", "audio", "keyframes", "cursor", "keystrokes",
                        "pip", "annotation"};
const Keys kTransitionKeys = {"track", "between", "kind", "duration_us", "color", "direction"};
const Keys kAutoKeys = {"zoom", "min_hold_us", "lead_us", "ease_ms", "follow", "follow_deadzone", "trigger_on"};
const Keys kCameraKeys = {"mode", "keyframes", "auto"};
const Keys kMarkerKeys = {"t_us", "label", "color"};
const Keys kUiKeys = {"playhead_us", "zoom", "scroll_us", "selected", "loop"};
const Keys kProjectKeys = {"format", "version", "app_version", "id", "name", "canvas", "media", "tracks", "clips",
                           "transitions", "camera", "markers", "ui"};

bool IsKnown(std::string_view key, const Keys& known) {
    for (std::string_view k : known)
        if (k == key) return true;
    return false;
}

// ---- streaming writer -----------------------------------------------------------------------------------------
// Produces exactly the layout of Json::Dump(2) without building a DOM (undo snapshots serialize the whole project on
// every edit, so this is on the interactive path).

void AppendEscaped(std::string& out, std::string_view s) {
    static const char kHex[] = "0123456789abcdef";
    out.push_back('"');
    size_t run = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        out.append(s.data() + run, i - run);
        run = i + 1;
        switch (c) {
        case '"': out.append("\\\""); break;
        case '\\': out.append("\\\\"); break;
        case '\b': out.append("\\b"); break;
        case '\f': out.append("\\f"); break;
        case '\n': out.append("\\n"); break;
        case '\r': out.append("\\r"); break;
        case '\t': out.append("\\t"); break;
        default:
            out.append("\\u00");
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
            break;
        }
    }
    out.append(s.data() + run, s.size() - run);
    out.push_back('"');
}

// Same rule as Json::Dump: the shortest of %.15g / %.16g / %.17g that reads back to the same bits.
void AppendDouble(std::string& out, double v) {
    if (!std::isfinite(v)) {
        out.append("null");
        return;
    }
    char buf[40];
    for (int precision = 15; precision <= 17; ++precision) {
        const auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general, precision);
        double back = 0;
        std::from_chars(buf, res.ptr, back);
        if (precision == 17 || back == v) {
            out.append(buf, res.ptr);
            return;
        }
    }
}

class Writer {
public:
    explicit Writer(std::string& out) : m_out(out) {}

    void BeginObject() { Open('{'); }
    void EndObject() { Close('}'); }
    void BeginArray() { Open('['); }
    void EndArray() { Close(']'); }
    Writer& Key(std::string_view k) {
        Element();
        AppendEscaped(m_out, k);
        m_out.append(": ");
        m_after_key = true;
        return *this;
    }
    void Null() { Prefix(); m_out.append("null"); }
    void Bool(bool v) { Prefix(); m_out.append(v ? "true" : "false"); }
    void Int(int64_t v) {
        Prefix();
        char buf[24];
        const auto res = std::to_chars(buf, buf + sizeof(buf), v);
        m_out.append(buf, res.ptr);
    }
    void Double(double v) { Prefix(); AppendDouble(m_out, v); }
    // %.6g of the float (C locale), -0 -> 0, non-finite -> 0. Reads back to a float that prints the same digits.
    void Float(float v) {
        Prefix();
        if (!std::isfinite(v) || v == 0.0f) {
            m_out.push_back('0');
            return;
        }
        char buf[32];
        const auto res = std::to_chars(buf, buf + sizeof(buf), static_cast<double>(v), std::chars_format::general, 6);
        m_out.append(buf, res.ptr);
    }
    void String(std::string_view s) { Prefix(); AppendEscaped(m_out, s); }
    void Color(const ColorF& c) { String(c.ToHex()); }
    // A preserved Json value: Json::Dump(2) re-indented to the current depth.
    void Raw(const Json& v) {
        Prefix();
        const std::string s = v.Dump(2);
        const size_t indent = m_counts.size() * 2;
        for (char ch : s) {
            m_out.push_back(ch);
            if (ch == '\n') m_out.append(indent, ' ');
        }
    }
    // Unknown keys of `extra` (an object) after the known ones.
    void Extra(const Json& extra, const Keys& known) {
        if (!extra.IsObject()) return;
        for (const auto& [k, v] : extra.Items())
            if (!IsKnown(k, known)) Key(k).Raw(v);
    }

    // Shorthands for "key": value.
    void KInt(std::string_view k, int64_t v) { Key(k).Int(v); }
    void KBool(std::string_view k, bool v) { Key(k).Bool(v); }
    void KFloat(std::string_view k, float v) { Key(k).Float(v); }
    void KDouble(std::string_view k, double v) { Key(k).Double(v); }
    void KString(std::string_view k, std::string_view v) { Key(k).String(v); }
    void KColor(std::string_view k, const ColorF& c) { Key(k).Color(c); }
    void KStringOrNull(std::string_view k, const std::string& v) {
        if (v.empty()) Key(k).Null();
        else Key(k).String(v);
    }
    void KStrings(std::string_view k, const std::vector<std::string>& v) {
        Key(k).BeginArray();
        for (const std::string& s : v) String(s);
        EndArray();
    }

private:
    void Indent(size_t level) {
        m_out.push_back('\n');
        m_out.append(level * 2, ' ');
    }
    void Element() {
        if (m_counts.empty()) return;
        if (m_counts.back()++ > 0) m_out.push_back(',');
        Indent(m_counts.size());
    }
    void Prefix() {
        if (m_after_key) m_after_key = false;
        else Element();
    }
    void Open(char c) {
        Prefix();
        m_out.push_back(c);
        m_counts.push_back(0);
    }
    void Close(char c) {
        const int n = m_counts.back();
        m_counts.pop_back();
        if (n > 0) Indent(m_counts.size());
        m_out.push_back(c);
    }

    std::string& m_out;
    std::vector<int> m_counts;   // elements written per open container
    bool m_after_key = false;
};

void WriteShadow(Writer& w, const Shadow& s) {
    w.BeginObject();
    w.KBool("enabled", s.enabled);
    w.KFloat("blur_px", s.blur_px);
    w.KFloat("offset_y_px", s.offset_y_px);
    w.KFloat("opacity", s.opacity);
    w.Extra(s.extra, kShadowKeys);
    w.EndObject();
}

void WriteCanvas(Writer& w, const Canvas& c) {
    w.BeginObject();
    w.KInt("width", c.width);
    w.KInt("height", c.height);
    w.KInt("fps_num", c.fps_num);
    w.KInt("fps_den", c.fps_den);
    w.Key("background").BeginObject();
    w.KString("kind", EnumName(c.background.kind, kBackgroundKinds));
    w.KColor("color_a", c.background.color_a);
    w.KColor("color_b", c.background.color_b);
    w.KFloat("angle_deg", c.background.angle_deg);
    w.KStringOrNull("image", c.background.image);
    w.KFloat("blur", c.background.blur);
    w.Extra(c.background.extra, kBackgroundKeys);
    w.EndObject();
    w.KInt("padding_px", c.padding_px);
    w.KInt("corner_radius_px", c.corner_radius_px);
    w.Key("shadow");
    WriteShadow(w, c.shadow);
    w.Extra(c.extra, kCanvasKeys);
    w.EndObject();
}

void WriteMedia(Writer& w, const Media& m, const std::string& path) {
    w.BeginObject();
    w.KString("id", m.id);
    w.KString("kind", EnumName(m.kind, kMediaKinds));
    w.KString("path", path);
    w.KInt("duration_us", m.duration_us);
    if (m.kind != Media::Kind::Audio) {
        w.Key("video").BeginObject();
        w.KInt("width", m.width);
        w.KInt("height", m.height);
        w.KInt("fps_num", m.fps_num);
        w.KInt("fps_den", m.fps_den);
        w.EndObject();
    }
    w.KStrings("audio_streams", m.audio_streams);
    w.KBool("has_events", m.has_events);
    w.KBool("has_camera", m.has_camera);
    w.Extra(m.extra, kMediaKeys);
    w.EndObject();
}

void WriteTrack(Writer& w, const Track& t) {
    w.BeginObject();
    w.KString("id", t.id);
    w.KString("kind", EnumName(t.kind, kTrackKinds));
    w.KString("name", t.name);
    w.KBool("muted", t.muted);
    w.KBool("locked", t.locked);
    w.KBool("hidden", t.hidden);
    w.KInt("height_px", t.height_px);
    w.Extra(t.extra, kTrackKeys);
    w.EndObject();
}

void WriteCursor(Writer& w, const CursorStyle& c) {
    w.BeginObject();
    w.KBool("visible", c.visible);
    w.KFloat("scale", c.scale);
    w.KFloat("smoothing", c.smoothing);
    w.KString("click_effect", EnumName(c.click, kClickEffects));
    w.KColor("click_color", c.click_color);
    w.KInt("hide_when_idle_us", c.hide_when_idle_us);
    w.KBool("spotlight", c.spotlight);
    w.KBool("shadow", c.shadow);
    w.Extra(c.extra, kCursorKeys);
    w.EndObject();
}

void WriteKeystrokes(Writer& w, const KeystrokeStyle& k) {
    w.BeginObject();
    w.KBool("enabled", k.enabled);
    w.KString("position", EnumName(k.pos, kKeyPositions));
    w.KString("style", EnumName(k.style, kKeyStyles));
    w.KInt("hold_ms", k.hold_ms);
    w.Extra(k.extra, kKeystrokeKeys);
    w.EndObject();
}

void WritePip(Writer& w, const Pip& p) {
    w.BeginObject();
    w.KString("corner", EnumName(p.corner, kPipCorners));
    w.KFloat("size", p.size);
    w.KInt("margin_px", p.margin_px);
    w.KString("mask", EnumName(p.mask, kPipMasks));
    w.KInt("radius_px", p.radius_px);
    w.KBool("shadow", p.shadow);
    w.KBool("mirror", p.mirror);
    w.Extra(p.extra, kPipKeys);
    w.EndObject();
}

void WriteAnnotation(Writer& w, const Annotation& a) {
    w.BeginObject();
    w.KString("type", EnumName(a.type, kAnnotationTypes));
    w.KString("text", a.text);
    w.KString("font", a.font);
    w.KInt("size_px", a.size_px);
    w.KInt("weight", a.weight);
    w.KColor("color", a.color);
    w.KColor("fill", a.fill);
    w.KColor("stroke", a.stroke);
    w.KInt("stroke_px", a.stroke_px);
    w.KInt("radius_px", a.radius_px);
    w.Key("rect").BeginObject();
    w.KFloat("x", a.rect.x);
    w.KFloat("y", a.rect.y);
    w.KFloat("w", a.rect.w);
    w.KFloat("h", a.rect.h);
    w.EndObject();
    w.Key("pointer").BeginObject();
    w.KFloat("x", a.pointer.x);
    w.KFloat("y", a.pointer.y);
    w.EndObject();
    w.KFloat("arrow_head_px", a.arrow_head_px);
    w.KInt("blur_px", a.blur_px);
    w.KBool("pixelate", a.pixelate);
    w.KString("anim_in", EnumName(a.anim_in, kAnims));
    w.KString("anim_out", EnumName(a.anim_out, kAnims));
    w.KInt("anim_ms", a.anim_ms);
    w.Extra(a.extra, kAnnotationKeys);
    w.EndObject();
}

void WriteClip(Writer& w, const Clip& c) {
    w.BeginObject();
    w.KString("id", c.id);
    w.KString("track", c.track);
    w.KStringOrNull("media", c.media);
    w.KString("source", EnumName(c.source, kSources));
    w.KInt("start_us", c.start_us);
    w.KInt("in_us", c.in_us);
    w.KInt("out_us", c.out_us);
    w.KDouble("speed", c.speed);
    w.KString("name", c.name);
    if (std::lround(std::clamp(c.color.a, 0.0f, 1.0f) * 255.0f) > 0) w.KColor("color", c.color);
    else w.Key("color").Null();
    w.Key("transform").BeginObject();
    w.KFloat("x", c.transform.x);
    w.KFloat("y", c.transform.y);
    w.KFloat("scale", c.transform.scale);
    w.KFloat("rotation_deg", c.transform.rotation_deg);
    w.KFloat("opacity", c.transform.opacity);
    w.KString("anchor", "center");
    w.Extra(c.transform.extra, kTransformKeys);
    w.EndObject();
    w.Key("crop").BeginObject();
    w.KInt("left", c.crop.left);
    w.KInt("top", c.crop.top);
    w.KInt("right", c.crop.right);
    w.KInt("bottom", c.crop.bottom);
    w.Extra(c.crop.extra, kCropKeys);
    w.EndObject();
    if (c.corner_radius_px) w.KInt("corner_radius_px", *c.corner_radius_px);
    else w.Key("corner_radius_px").Null();
    w.Key("shadow");
    if (c.shadow) WriteShadow(w, *c.shadow);
    else w.Null();
    w.Key("audio").BeginObject();
    w.KFloat("gain_db", c.audio.gain_db);
    w.KBool("muted", c.audio.muted);
    w.KInt("fade_in_us", c.audio.fade_in_us);
    w.KInt("fade_out_us", c.audio.fade_out_us);
    w.KBool("normalize", c.audio.normalize);
    if (c.audio.noise_gate_db) w.KFloat("noise_gate_db", *c.audio.noise_gate_db);
    else w.Key("noise_gate_db").Null();
    w.Extra(c.audio.extra, kAudioKeys);
    w.EndObject();
    w.Key("keyframes").BeginObject();
    for (const auto& [prop, track] : c.keyframes) {
        if (track.Empty()) continue;
        w.Key(prop).BeginArray();
        for (const Keyframe<float>& k : track.keys) {
            w.BeginObject();
            w.KInt("t_us", k.t_us);
            w.KFloat("v", k.v);
            w.KString("ease", EaseName(k.ease));
            w.EndObject();
        }
        w.EndArray();
    }
    w.EndObject();
    if (c.source == Clip::Source::Screen) {
        w.Key("cursor");
        WriteCursor(w, c.cursor);
        w.Key("keystrokes");
        WriteKeystrokes(w, c.keystrokes);
    }
    if (c.pip) {
        w.Key("pip");
        WritePip(w, *c.pip);
    }
    if (c.annotation) {
        w.Key("annotation");
        WriteAnnotation(w, *c.annotation);
    }
    w.Extra(c.extra, kClipKeys);
    w.EndObject();
}

void WriteTransition(Writer& w, const Transition& t) {
    w.BeginObject();
    w.KString("track", t.track);
    w.Key("between").BeginArray();
    w.String(t.a);
    w.String(t.b);
    w.EndArray();
    w.KString("kind", EnumName(t.kind, kTransitionKinds));
    w.KInt("duration_us", t.duration_us);
    w.KColor("color", t.color);
    w.KString("direction", EnumName(t.dir, kDirections));
    w.Extra(t.extra, kTransitionKeys);
    w.EndObject();
}

void WriteCamera(Writer& w, const Camera& c) {
    w.BeginObject();
    w.KString("mode", EnumName(c.mode, kCameraModes));
    w.Key("keyframes").BeginArray();
    for (const CameraKey& k : c.keys) {
        w.BeginObject();
        w.KInt("t_us", k.t_us);
        w.KFloat("zoom", k.zoom);
        w.KFloat("cx", k.cx);
        w.KFloat("cy", k.cy);
        w.KString("ease", EaseName(k.ease));
        if (k.generated) w.KBool("generated", true);
        w.EndObject();
    }
    w.EndArray();
    const AutoZoomParams& a = c.auto_params;
    w.Key("auto").BeginObject();
    w.KFloat("zoom", a.zoom);
    w.KInt("min_hold_us", a.min_hold_us);
    w.KInt("lead_us", a.lead_us);
    w.KInt("ease_ms", a.ease_ms);
    w.KBool("follow", a.follow);
    w.KFloat("follow_deadzone", a.follow_deadzone);
    w.Key("trigger_on").BeginArray();
    if (a.on_click) w.String("click");
    if (a.on_typing) w.String("typing");
    w.EndArray();
    w.Extra(a.extra, kAutoKeys);
    w.EndObject();
    w.Extra(c.extra, kCameraKeys);
    w.EndObject();
}

void WriteMarker(Writer& w, const Marker& m) {
    w.BeginObject();
    w.KInt("t_us", m.t_us);
    w.KString("label", m.label);
    w.KColor("color", m.color);
    w.Extra(m.extra, kMarkerKeys);
    w.EndObject();
}

void WriteUi(Writer& w, const UiState& u) {
    w.BeginObject();
    w.KInt("playhead_us", u.playhead_us);
    w.KDouble("zoom", u.zoom);
    w.KInt("scroll_us", u.scroll_us);
    w.KStrings("selected", u.selected);
    if (u.loop) {
        w.Key("loop").BeginArray();
        w.Int(u.loop->first);
        w.Int(u.loop->second);
        w.EndArray();
    } else {
        w.Key("loop").Null();
    }
    w.Extra(u.extra, kUiKeys);
    w.EndObject();
}

// project_dir non-empty: media paths are written relative to it where possible (Save).
std::string WriteProject(const Project& p, const std::string& project_dir);

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

std::string WriteProject(const Project& p, const std::string& project_dir) {
    std::string out;
    out.reserve(4096 + p.clips.size() * 1400 + p.media.size() * 300);
    Writer w(out);
    w.BeginObject();
    w.KString("format", "ctproj");
    w.KInt("version", p.version);
    w.KString("app_version", CT_VERSION_STRING);
    w.KString("id", p.id);
    w.KString("name", p.name);
    w.Key("canvas");
    WriteCanvas(w, p.canvas);
    w.Key("media").BeginArray();
    for (const Media& m : p.media) WriteMedia(w, m, project_dir.empty() ? m.path : StoredPath(project_dir, m.path));
    w.EndArray();
    w.Key("tracks").BeginArray();
    for (const Track& t : p.tracks) WriteTrack(w, t);
    w.EndArray();
    w.Key("clips").BeginArray();
    for (const Clip& c : p.clips) WriteClip(w, c);
    w.EndArray();
    w.Key("transitions").BeginArray();
    for (const Transition& t : p.transitions) WriteTransition(w, t);
    w.EndArray();
    w.Key("camera");
    WriteCamera(w, p.camera);
    w.Key("markers").BeginArray();
    for (const Marker& m : p.markers) WriteMarker(w, m);
    w.EndArray();
    w.Key("ui");
    WriteUi(w, p.ui);
    w.Extra(p.extra, kProjectKeys);
    w.EndObject();
    out.push_back('\n');
    return out;
}

} // namespace

double RoundFloat(float v) {
    if (!std::isfinite(v) || v == 0.0f) return 0.0;   // also turns -0 into 0
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), static_cast<double>(v), std::chars_format::general, 6);
    double back = 0;
    std::from_chars(buf, res.ptr, back);
    return back;
}

std::string ToJson(const Project& p) { return WriteProject(p, std::string()); }

Json ToJsonValue(const Project& p) {
    Json j;
    Json::Parse(ToJson(p), j);
    return j;
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
    const std::string text = WriteProject(p, std::string(PathParent(abs)));
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
