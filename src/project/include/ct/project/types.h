#pragma once
// ct/project/types.h
// Value types of the project model (docs/04 section 3). Names are the contract with the compositor (P08) and the
// editor (P09). The camera types are in camera.h and `Project` itself in project.h; include project.h for all.
// Every struct that is a JSON object keeps the keys this version does not know in `extra` (re-emitted on save).
// Thread model: plain values; one owner (the UI thread) mutates a Project, others work on copies.

#include <ct/core/json.h>
#include <ct/core/types.h>
#include <ct/project/easing.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ct::project {

using Id = std::string;   // "m1", "t3", "c42": prefix + base-36 counter (Project::NewId)

struct PointF { float x = 0, y = 0; };

// Keyframeable clip properties. These strings are used verbatim by the UI and the compositor.
constexpr const char* kKeyframeProps[] = {
    "transform.x", "transform.y", "transform.scale", "transform.rotation_deg", "transform.opacity", "audio.gain_db",
};
bool IsKeyframeProp(std::string_view prop);

template <class T>
struct Keyframe {
    TimeUs t_us = 0;
    T v{};
    Ease ease = Ease::Linear;   // easing of the segment that starts at this key
};

template <class T>
struct KeyTrack {
    std::vector<Keyframe<T>> keys;   // sorted by t_us, unique times

    bool Empty() const { return keys.empty(); }
    // Before the first key: first.v; after the last: last.v; between: lerp with ApplyEase(prev.ease, u).
    T At(TimeUs t, const T& fallback) const {
        if (keys.empty()) return fallback;
        if (t <= keys.front().t_us) return keys.front().v;
        if (t >= keys.back().t_us) return keys.back().v;
        auto it = std::upper_bound(keys.begin(), keys.end(), t, [](TimeUs tt, const Keyframe<T>& k) { return tt < k.t_us; });
        const Keyframe<T>& b = *it;
        const Keyframe<T>& a = *(it - 1);
        const float u = static_cast<float>(double(t - a.t_us) / double(b.t_us - a.t_us));
        const float e = ApplyEase(a.ease, u);
        return static_cast<T>(a.v + (b.v - a.v) * e);
    }
    // Inserts or replaces the key at t.
    void Set(TimeUs t, T v, Ease e) {
        auto it = std::lower_bound(keys.begin(), keys.end(), t, [](const Keyframe<T>& k, TimeUs tt) { return k.t_us < tt; });
        if (it != keys.end() && it->t_us == t) {
            it->v = v;
            it->ease = e;
        } else {
            keys.insert(it, Keyframe<T>{t, v, e});
        }
    }
    bool Remove(TimeUs t) {
        auto it = std::lower_bound(keys.begin(), keys.end(), t, [](const Keyframe<T>& k, TimeUs tt) { return k.t_us < tt; });
        if (it == keys.end() || it->t_us != t) return false;
        keys.erase(it);
        return true;
    }
    // Closest key with |key.t - t| <= tolerance (the earlier one on a tie); nullptr when none.
    const Keyframe<T>* Nearest(TimeUs t, TimeUs tolerance) const {
        const Keyframe<T>* best = nullptr;
        TimeUs best_d = tolerance;
        for (const Keyframe<T>& k : keys) {
            const TimeUs d = k.t_us > t ? k.t_us - t : t - k.t_us;
            if (d <= best_d && (!best || d < best_d)) {
                best = &k;
                best_d = d;
            }
        }
        return best;
    }
    bool IsSorted() const {
        for (size_t i = 1; i < keys.size(); ++i)
            if (keys[i - 1].t_us >= keys[i].t_us) return false;
        return true;
    }
};

struct Transform { float x = 0, y = 0, scale = 1, rotation_deg = 0, opacity = 1; Json extra; };   // x,y normalized canvas offsets
struct Crop { int left = 0, top = 0, right = 0, bottom = 0; Json extra; };                       // source pixels
struct Shadow { bool enabled = true; float blur_px = 48, offset_y_px = 24, opacity = 0.45f; Json extra; };
struct Background {
    enum class Kind { Solid, Gradient, Image } kind = Kind::Gradient;
    ColorF color_a{0.106f, 0.153f, 0.208f, 1}, color_b{0.035f, 0.039f, 0.059f, 1};
    float angle_deg = 135;
    std::string image;   // empty = none (JSON null)
    float blur = 0;
    Json extra;
};
struct Canvas {
    int width = 1920, height = 1080, fps_num = 60, fps_den = 1;
    Background background;
    int padding_px = 96;
    int corner_radius_px = 18;
    Shadow shadow;
    Json extra;
};
struct AudioProps {
    float gain_db = 0;
    bool muted = false;
    TimeUs fade_in_us = 0, fade_out_us = 0;
    bool normalize = false;
    std::optional<float> noise_gate_db;
    Json extra;
};
struct CursorStyle {
    bool visible = true;
    float scale = 1.5f;
    float smoothing = 0.35f;
    enum class Click { None, Ripple, Highlight, Both } click = Click::Ripple;
    ColorF click_color{0.31f, 0.55f, 1, 1};
    TimeUs hide_when_idle_us = 3'000'000;
    bool spotlight = false;
    bool shadow = true;
    Json extra;
};
struct KeystrokeStyle {
    bool enabled = true;
    enum class Pos { BottomCenter, BottomLeft, BottomRight, TopCenter } pos = Pos::BottomCenter;
    enum class Style { Pill, Keycap } style = Style::Pill;
    int hold_ms = 1200;
    Json extra;
};
struct Pip {
    enum class Corner { TL, TR, BL, BR } corner = Corner::BR;
    float size = 0.22f;
    int margin_px = 32;
    enum class Mask { None, Rounded, Circle } mask = Mask::Rounded;
    int radius_px = 24;
    bool shadow = true;
    bool mirror = false;
    Json extra;
};
struct Annotation {
    enum class Type { Text, Callout, Arrow, Rect, Ellipse, Highlight, Blur, Keystrokes } type = Type::Text;
    std::string text;
    std::string font = "Segoe UI";
    int size_px = 48;
    int weight = 600;
    ColorF color{1, 1, 1, 1}, fill{0.31f, 0.55f, 1, 1}, stroke{0, 0, 0, 1};
    int stroke_px = 0;
    int radius_px = 12;
    RectF rect{0.4f, 0.4f, 0.2f, 0.1f};
    PointF pointer{0.5f, 0.6f};
    float arrow_head_px = 24;
    int blur_px = 24;
    bool pixelate = false;
    enum class Anim { None, Fade, Pop, Slide } anim_in = Anim::Pop, anim_out = Anim::Fade;
    int anim_ms = 220;
    Json extra;
};
struct Media {
    Id id;
    enum class Kind { Recording, Video, Audio, Image } kind = Kind::Video;
    std::string path;   // absolute in memory; Save() writes it relative to the project when on the same volume
    TimeUs duration_us = 0;
    int width = 0, height = 0, fps_num = 0, fps_den = 1;
    std::vector<std::string> audio_streams;   // "mic", "system", "default" in MP4 stream order
    bool has_events = false, has_camera = false;
    bool missing = false;   // not serialized: set by Load() when the file cannot be found
    Json extra;
};
struct Track {
    Id id;
    enum class Kind { Video, Audio, Annotations } kind = Kind::Video;
    std::string name;
    bool muted = false, locked = false, hidden = false;
    int height_px = 0;   // 0 = UI default
    Json extra;
};
struct Clip {
    Id id, track, media;   // media is empty for annotation clips
    enum class Source { Screen, Camera, Mic, System, Default } source = Source::Default;
    TimeUs start_us = 0, in_us = 0, out_us = 0;   // timeline start; media in/out
    double speed = 1.0;
    std::string name;
    ColorF color{0, 0, 0, 0};   // alpha 0 = track default
    Transform transform;
    Crop crop;
    std::optional<int> corner_radius_px;   // null = canvas default
    std::optional<Shadow> shadow;          // null = canvas default
    AudioProps audio;
    // Keyframe times are relative to the clip start on the timeline (t - start_us). Keys: kKeyframeProps.
    std::map<std::string, KeyTrack<float>> keyframes;
    CursorStyle cursor;           // used by Source::Screen clips
    KeystrokeStyle keystrokes;    // used by Source::Screen clips
    std::optional<Pip> pip;       // camera clips shown picture-in-picture
    std::optional<Annotation> annotation;   // set on clips of annotation tracks
    Json extra;

    TimeUs Duration() const { return TimeUs((out_us - in_us) / speed); }
    TimeUs End() const { return start_us + Duration(); }
    TimeUs MediaTimeAt(TimeUs timeline_t) const { return in_us + TimeUs((timeline_t - start_us) * speed); }
    // Keyframed value at timeline time t when the property has keys, else static_value.
    float Prop(const char* key, TimeUs t, float static_value) const;
    bool IsAnnotation() const { return annotation.has_value(); }
};
struct Transition {
    Id track, a, b;   // a ends where b starts
    enum class Kind { Crossfade, Dip, Slide } kind = Kind::Crossfade;
    TimeUs duration_us = 500'000;
    ColorF color{0, 0, 0, 1};
    enum class Dir { Left, Right, Up, Down } dir = Dir::Left;
    Json extra;
};
struct Marker {
    TimeUs t_us = 0;
    std::string label;
    ColorF color{1, 0.69f, 0.125f, 1};
    Json extra;
};
struct UiState {
    TimeUs playhead_us = 0;
    double zoom = 1;
    TimeUs scroll_us = 0;
    std::vector<Id> selected;
    std::optional<std::pair<TimeUs, TimeUs>> loop;
    Json extra;
};

} // namespace ct::project
