#include <ct/project/auto_zoom.h>

#include <algorithm>
#include <cmath>

namespace ct::project {

namespace {

struct Segment {
    TimeUs first_t, last_t;   // first / last event
    TimeUs t_in = 0, t_out = 0;
    double cx, cy;            // running mean of the event positions
    int n = 1;
    bool pan_to_next = false;   // t_out is where the pan to the next segment starts
};

double Dist(double ax, double ay, double bx, double by) { return std::hypot(ax - bx, ay - by); }

float Clamp01(float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.5f; }

class KeyList {
public:
    // Keys must be strictly increasing. A key at the same time as the previous one is only expected when it shows the
    // same view (zoom-out end meeting the next zoom-in start, segments exactly 2 * ease apart): the two merge into one
    // key with the later key's ease, which describes the segment that follows (the earlier Hold would jump).
    void Push(TimeUs t, float zoom, float cx, float cy, Ease ease) {
        if (!keys.empty() && t == keys.back().t_us) {
            CameraKey& last = keys.back();
            if (last.zoom == zoom && last.cx == cx && last.cy == cy) last.ease = ease;
            return;
        }
        if (!keys.empty() && t < keys.back().t_us) return;
        keys.push_back(CameraKey{t, zoom, cx, cy, ease, true});
    }
    std::vector<CameraKey> keys;
};

std::vector<CameraKey> GenerateImpl(const std::vector<InterestEvent>& input, TimeUs duration_us, const AutoZoomParams& params,
                                    const events::EventLog* log, TimeUs log_offset_us) {
    const float zoom = params.zoom;
    if (input.empty() || duration_us <= 0 || !(zoom > 1.0f) || !std::isfinite(zoom)) return {};
    const TimeUs ease = std::max<TimeUs>(TimeUs(params.ease_ms) * 1000, 1000);
    const TimeUs hold = std::max<TimeUs>(params.min_hold_us, 1);
    const TimeUs lead = std::max<TimeUs>(params.lead_us, 0);

    std::vector<InterestEvent> events = input;
    std::stable_sort(events.begin(), events.end(), [](const InterestEvent& a, const InterestEvent& b) { return a.t_us < b.t_us; });

    // 1. Segments.
    std::vector<Segment> segs;
    for (const InterestEvent& e : events) {
        const double x = Clamp01(e.x), y = Clamp01(e.y);
        if (!segs.empty()) {
            Segment& s = segs.back();
            if (e.t_us - s.last_t <= hold * 3 / 2 && Dist(x, y, s.cx, s.cy) <= AutoZoom::kAbsorbRadius) {
                ++s.n;
                s.cx += (x - s.cx) / s.n;
                s.cy += (y - s.cy) / s.n;
                s.last_t = e.t_us;
                continue;
            }
        }
        segs.push_back(Segment{e.t_us, e.t_us, 0, 0, x, y});
    }

    // 2. Clamp (leaving room for the ramps), drop short ones, merge or link neighbours.
    std::vector<Segment> kept;
    for (Segment s : segs) {
        s.t_in = std::max(s.first_t - lead, ease);
        s.t_out = std::min(s.last_t + hold, duration_us - ease);
        if (s.t_out - s.t_in >= hold) kept.push_back(s);
    }
    std::vector<Segment> out;
    for (const Segment& s : kept) {
        if (!out.empty()) {
            Segment& prev = out.back();
            if (s.t_in - prev.t_out < 2 * ease) {
                const bool close = Dist(s.cx, s.cy, prev.cx, prev.cy) <= AutoZoom::kAbsorbRadius;
                const bool pan_fits = s.t_in - ease > prev.t_in;
                if (close || !pan_fits) {
                    const int n = prev.n + s.n;
                    prev.cx = (prev.cx * prev.n + s.cx * s.n) / n;
                    prev.cy = (prev.cy * prev.n + s.cy * s.n) / n;
                    prev.n = n;
                    prev.last_t = std::max(prev.last_t, s.last_t);
                    prev.t_out = std::max(prev.t_out, s.t_out);
                    continue;
                }
                prev.pan_to_next = true;
                prev.t_out = s.t_in - ease;   // the pan takes exactly ease_ms and ends where s is fully zoomed
            }
        }
        out.push_back(s);
    }

    // 3 + 4. Keys, with follow moves inside each segment.
    const SizeI region = log ? log->RegionSize() : SizeI{};
    const bool follow = params.follow && log && !region.IsEmpty();
    const float deadzone = std::max(params.follow_deadzone, 0.0f) / zoom;
    KeyList keys;
    for (size_t i = 0; i < out.size(); ++i) {
        const Segment& s = out[i];
        float cx = ClampCentre(static_cast<float>(s.cx), zoom);
        float cy = ClampCentre(static_cast<float>(s.cy), zoom);
        if (i == 0 || !out[i - 1].pan_to_next) keys.Push(s.t_in - ease, 1.0f, 0.5f, 0.5f, Ease::InOut);
        keys.Push(s.t_in, zoom, cx, cy, Ease::Hold);
        if (follow) {
            TimeUs ts = s.t_in + AutoZoom::kFollowSampleUs;
            while (ts + AutoZoom::kFollowPanUs < s.t_out) {
                const events::CursorState cur = log->CursorAt(ts + log_offset_us);
                bool moved = false;
                if (cur.valid) {
                    const float px = Clamp01(cur.x / float(region.w));
                    const float py = Clamp01(cur.y / float(region.h));
                    auto pull = [&](float c, float p) {
                        const float d = p - c;
                        const float over = std::fabs(d) - deadzone;
                        return over > 0 ? c + (d > 0 ? over : -over) : c;
                    };
                    const float nx = ClampCentre(pull(cx, px), zoom);
                    const float ny = ClampCentre(pull(cy, py), zoom);
                    if (std::fabs(nx - cx) > 1e-3f || std::fabs(ny - cy) > 1e-3f) {
                        keys.Push(ts, zoom, cx, cy, Ease::Out);
                        keys.Push(ts + AutoZoom::kFollowPanUs, zoom, nx, ny, Ease::Hold);
                        cx = nx;
                        cy = ny;
                        moved = true;
                    }
                }
                ts += moved ? AutoZoom::kFollowMinGapUs : AutoZoom::kFollowSampleUs;
            }
        }
        keys.Push(s.t_out, zoom, cx, cy, Ease::InOut);
        if (!s.pan_to_next) keys.Push(s.t_out + ease, 1.0f, 0.5f, 0.5f, Ease::Hold);
    }
    return std::move(keys.keys);
}

} // namespace

std::vector<InterestEvent> AutoZoom::ExtractInterest(const events::EventLog& log, SizeI video_size, const AutoZoomParams& params) {
    std::vector<InterestEvent> out;
    if (video_size.IsEmpty()) video_size = log.RegionSize();
    if (video_size.IsEmpty()) return out;
    const float w = static_cast<float>(video_size.w), h = static_cast<float>(video_size.h);
    if (params.on_click) {
        for (const events::ButtonEvent& b : log.Buttons()) {
            if (!b.down || (b.button != events::Button::Left && b.button != events::Button::Right)) continue;
            out.push_back({b.t_us, Clamp01(b.x / w), Clamp01(b.y / h), InterestEvent::Kind::Click});
        }
    }
    if (params.on_typing) {
        for (const events::TypingBurst& burst : log.TypingBursts()) {
            const events::CursorState c = log.CursorAt(burst.start_us);
            const float x = c.valid ? Clamp01(c.x / w) : 0.5f;
            const float y = c.valid ? Clamp01(c.y / h) : 0.5f;
            out.push_back({burst.start_us, x, y, InterestEvent::Kind::Typing});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const InterestEvent& a, const InterestEvent& b) { return a.t_us < b.t_us; });
    return out;
}

std::vector<CameraKey> AutoZoom::Generate(const std::vector<InterestEvent>& events, TimeUs duration_us,
                                          const AutoZoomParams& params, const events::EventLog* for_follow) {
    return GenerateImpl(events, duration_us, params, for_follow, 0);
}

std::vector<CameraKey> AutoZoom::ForClip(const Clip& clip, const events::EventLog& log, SizeI video_size,
                                         const AutoZoomParams& params) {
    std::vector<InterestEvent> events;
    for (InterestEvent e : ExtractInterest(log, video_size, params)) {
        if (e.t_us < clip.in_us || e.t_us >= clip.out_us) continue;
        e.t_us -= clip.in_us;
        events.push_back(e);
    }
    std::vector<CameraKey> keys = GenerateImpl(events, clip.out_us - clip.in_us, params, &log, clip.in_us);
    std::vector<CameraKey> mapped;
    mapped.reserve(keys.size());
    for (CameraKey k : keys) {
        k.t_us = clip.start_us + TimeUs(double(k.t_us) / clip.speed);
        if (!mapped.empty() && k.t_us <= mapped.back().t_us) continue;
        mapped.push_back(k);
    }
    return mapped;
}

void AutoZoom::AppendKeys(Camera& camera, const std::vector<CameraKey>& keys) {
    for (CameraKey k : keys) {
        auto it = std::lower_bound(camera.keys.begin(), camera.keys.end(), k.t_us,
                                   [](const CameraKey& a, TimeUs t) { return a.t_us < t; });
        if (it != camera.keys.end() && it->t_us == k.t_us) continue;
        k.generated = true;
        camera.keys.insert(it, k);
    }
}

std::vector<CameraKey> AutoZoom::ForProject(const Project& p, const LogLookup& log_for, int* clips) {
    std::vector<const Clip*> screen;
    for (const Clip& c : p.clips)
        if (c.source == Clip::Source::Screen && !c.IsAnnotation()) screen.push_back(&c);
    std::stable_sort(screen.begin(), screen.end(), [](const Clip* a, const Clip* b) { return a->start_us < b->start_us; });
    std::vector<CameraKey> keys;
    int used = 0;
    for (const Clip* c : screen) {
        const Media* m = p.FindMedia(c->media);
        if (!m || m->kind != Media::Kind::Recording) continue;
        const events::EventLog* log = log_for ? log_for(*m) : nullptr;
        if (!log) continue;
        const SizeI vs{m->width > 0 ? m->width : p.canvas.width, m->height > 0 ? m->height : p.canvas.height};
        const std::vector<CameraKey> k = ForClip(*c, *log, vs, p.camera.auto_params);
        keys.insert(keys.end(), k.begin(), k.end());
        ++used;
    }
    // Stable: on equal times the key of the earlier clip (inserted first) stays.
    std::stable_sort(keys.begin(), keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.t_us < b.t_us; });
    keys.erase(std::unique(keys.begin(), keys.end(), [](const CameraKey& a, const CameraKey& b) { return a.t_us == b.t_us; }),
               keys.end());
    if (clips) *clips = used;
    return keys;
}

bool AutoZoom::Regenerate(Project& p, const LogLookup& log_for, bool* changed, int* clips) {
    int used = 0;
    const std::vector<CameraKey> keys = ForProject(p, log_for, &used);
    if (clips) *clips = used;
    if (changed) *changed = false;
    if (!used) return false;
    std::vector<CameraKey> before = p.camera.keys;
    p.camera.ReplaceGenerated(keys);
    if (changed) {
        const auto same = [](const CameraKey& a, const CameraKey& b) {
            return a.t_us == b.t_us && a.zoom == b.zoom && a.cx == b.cx && a.cy == b.cy && a.ease == b.ease &&
                   a.generated == b.generated;
        };
        *changed = before.size() != p.camera.keys.size() ||
                   !std::equal(before.begin(), before.end(), p.camera.keys.begin(), same);
    }
    return true;
}

} // namespace ct::project
