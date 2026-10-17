#include <ct/project/edit_ops.h>

#include <ct/core/strings.h>
#include <ct/core/time.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ct::project::ops {

namespace {

bool Fail(Err* err, std::string msg) {
    if (err) err->Set(msg);
    return false;
}

bool Finite(float v) { return std::isfinite(v); }

// Any clip on `track` except `ignore` intersecting [start, end).
const Clip* FindOverlap(const Project& p, const Id& track, TimeUs start, TimeUs end, const Id& ignore) {
    for (const Clip& c : p.clips)
        if (c.track == track && c.id != ignore && start < c.End() && c.start_us < end) return &c;
    return nullptr;
}

bool CheckFree(const Project& p, const Id& track, TimeUs start, TimeUs end, const Id& ignore, Err* err) {
    if (const Clip* o = FindOverlap(p, track, start, end, ignore))
        return Fail(err, "That would overlap clip " + (o->name.empty() ? o->id : o->name) + ".");
    return true;
}

bool EditableTrack(const Project& p, const Id& track, Err* err) {
    const Track* t = p.FindTrack(track);
    if (!t) return Fail(err, "The track does not exist.");
    if (t->locked) return Fail(err, "The track \"" + t->name + "\" is locked.");
    return true;
}

Clip* EditableClip(Project& p, const Id& id, Err* err) {
    Clip* c = p.FindClip(id);
    if (!c) {
        Fail(err, "The clip does not exist.");
        return nullptr;
    }
    if (!EditableTrack(p, c->track, err)) return nullptr;
    return c;
}

TimeUs DurationOf(TimeUs in_us, TimeUs out_us, double speed) { return TimeUs((out_us - in_us) / speed); }

bool TransitionValid(const Project& p, const Transition& t) {
    const Clip* a = p.FindClip(t.a);
    const Clip* b = p.FindClip(t.b);
    if (!a || !b || a->track != t.track || b->track != t.track) return false;
    const TimeUs gap = b->start_us - a->End();
    if (gap < 0 || gap > FrameTimeUs(1, p.canvas.fps_num, p.canvas.fps_den)) return false;
    return t.duration_us > 0 && t.duration_us <= std::min(a->Duration(), b->Duration()) / 2;
}

void PruneTransitions(Project& p) {
    p.transitions.erase(std::remove_if(p.transitions.begin(), p.transitions.end(),
                                       [&](const Transition& t) { return !TransitionValid(p, t); }),
                        p.transitions.end());
}

void ShiftKeyframes(Clip& c, TimeUs delta) {
    if (delta == 0) return;
    for (auto& [prop, track] : c.keyframes)
        for (Keyframe<float>& k : track.keys) k.t_us += delta;
}

// Checks a clip as AddClip would, without the overlap test.
bool CheckClip(const Project& p, const Clip& c, Err* err) {
    const Track* t = p.FindTrack(c.track);
    if (!t) return Fail(err, "The track does not exist.");
    std::string why;
    if (!p.ClipFits(c, *t, &why)) return Fail(err, why);
    if (!(c.speed >= kMinSpeed && c.speed <= kMaxSpeed)) return Fail(err, Format("Speed must be between %gx and %gx.", kMinSpeed, kMaxSpeed));
    if (c.start_us < 0) return Fail(err, "A clip cannot start before 0.");
    if (c.in_us < 0 || c.in_us >= c.out_us) return Fail(err, "The clip has no content (in must be before out).");
    if (c.out_us > p.MaxOutUs(c)) return Fail(err, "The clip extends past the end of its media.");
    if (c.Duration() <= 0) return Fail(err, "The clip is too short.");
    for (const auto& [prop, track] : c.keyframes) {
        if (!IsKeyframeProp(prop)) return Fail(err, "Unknown keyframe property " + prop + ".");
        if (!track.IsSorted()) return Fail(err, "Keyframes must be sorted.");
    }
    return true;
}

// Left / right halves of a keyframe track split at clip-relative time `rel`.
void SplitTrack(const KeyTrack<float>& src, TimeUs rel, KeyTrack<float>& left, KeyTrack<float>& right) {
    const float v = src.At(rel, 0.0f);
    Ease ease_at = Ease::Linear;   // easing of the segment running through `rel`
    bool any_before_or_at = false, any_after_or_at = false;
    for (const Keyframe<float>& k : src.keys) {
        if (k.t_us <= rel) {
            ease_at = k.ease;
            any_before_or_at = true;
        }
        if (k.t_us >= rel) any_after_or_at = true;
    }
    for (const Keyframe<float>& k : src.keys)
        if (k.t_us < rel) left.keys.push_back(k);
    if (any_after_or_at) left.keys.push_back({rel, v, ease_at});
    if (any_before_or_at) right.keys.push_back({0, v, ease_at});
    for (const Keyframe<float>& k : src.keys)
        if (k.t_us > rel) right.keys.push_back({k.t_us - rel, k.v, k.ease});
}

void EraseClips(Project& p, const std::vector<Id>& ids) {
    auto listed = [&](const Id& id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    p.clips.erase(std::remove_if(p.clips.begin(), p.clips.end(), [&](const Clip& c) { return listed(c.id); }), p.clips.end());
    p.transitions.erase(std::remove_if(p.transitions.begin(), p.transitions.end(),
                                       [&](const Transition& t) { return listed(t.a) || listed(t.b); }),
                        p.transitions.end());
}

} // namespace

// ---- media / tracks -------------------------------------------------------------------------------------------

bool AddMedia(Project& p, Media m, Id* out) {
    if (m.id.empty() || p.FindMedia(m.id) || p.FindTrack(m.id) || p.FindClip(m.id)) m.id = p.NewId("m");
    if (out) *out = m.id;
    p.media.push_back(std::move(m));
    return true;
}

bool RemoveMedia(Project& p, const Id& media, bool remove_clips, Err* err) {
    if (!p.FindMedia(media)) return Fail(err, "The media does not exist.");
    std::vector<Id> users;
    for (const Clip& c : p.clips)
        if (c.media == media) users.push_back(c.id);
    if (!users.empty() && !remove_clips) return Fail(err, Format("The media is used by %zu clip(s).", users.size()));
    EraseClips(p, users);
    p.media.erase(std::remove_if(p.media.begin(), p.media.end(), [&](const Media& m) { return m.id == media; }), p.media.end());
    return true;
}

bool AddTrack(Project& p, Track::Kind kind, std::string name, Id* out, int at_index) {
    Track t;
    t.id = p.NewId("t");
    t.kind = kind;
    t.name = std::move(name);
    if (out) *out = t.id;
    const int n = static_cast<int>(p.tracks.size());
    const int at = (at_index < 0 || at_index > n) ? n : at_index;
    p.tracks.insert(p.tracks.begin() + at, std::move(t));
    return true;
}

bool RemoveTrack(Project& p, const Id& track, Err* err) {
    if (!p.FindTrack(track)) return Fail(err, "The track does not exist.");
    std::vector<Id> on;
    for (const Clip& c : p.clips)
        if (c.track == track) on.push_back(c.id);
    EraseClips(p, on);
    p.tracks.erase(std::remove_if(p.tracks.begin(), p.tracks.end(), [&](const Track& t) { return t.id == track; }), p.tracks.end());
    return true;
}

bool MoveTrack(Project& p, const Id& track, int new_index) {
    const int from = p.TrackIndex(track);
    if (from < 0 || new_index < 0 || new_index >= static_cast<int>(p.tracks.size())) return false;
    if (from == new_index) return true;
    Track t = std::move(p.tracks[static_cast<size_t>(from)]);
    p.tracks.erase(p.tracks.begin() + from);
    p.tracks.insert(p.tracks.begin() + new_index, std::move(t));
    return true;
}

// ---- clips ----------------------------------------------------------------------------------------------------

bool AddClip(Project& p, Clip c, Id* out, Err* err) {
    if (!EditableTrack(p, c.track, err)) return false;
    if (!CheckClip(p, c, err)) return false;
    if (!CheckFree(p, c.track, c.start_us, c.End(), Id(), err)) return false;
    if (c.id.empty() || p.FindMedia(c.id) || p.FindTrack(c.id) || p.FindClip(c.id)) c.id = p.NewId("c");
    if (out) *out = c.id;
    p.clips.push_back(std::move(c));
    return true;
}

bool MoveClip(Project& p, const Id& clip, TimeUs new_start, const Id& new_track, Err* err) {
    Clip* c = EditableClip(p, clip, err);
    if (!c) return false;
    Clip moved = *c;
    moved.start_us = new_start;
    if (!new_track.empty()) moved.track = new_track;
    if (!EditableTrack(p, moved.track, err)) return false;
    if (!CheckClip(p, moved, err)) return false;
    if (!CheckFree(p, moved.track, moved.start_us, moved.End(), clip, err)) return false;
    *c = std::move(moved);
    PruneTransitions(p);
    return true;
}

bool TrimIn(Project& p, const Id& clip, TimeUs new_start_us, Err* err) {
    Clip* c = EditableClip(p, clip, err);
    if (!c) return false;
    const TimeUs end = c->End();
    if (new_start_us >= end) return Fail(err, "The clip would become empty.");
    const TimeUs new_in = c->MediaTimeAt(new_start_us);
    if (new_in < 0) return Fail(err, "The clip already starts at the beginning of its media.");
    if (new_in >= c->out_us) return Fail(err, "The clip would become empty.");
    const TimeUs dur = DurationOf(new_in, c->out_us, c->speed);
    if (dur <= 0) return Fail(err, "The clip would become empty.");
    const TimeUs start = end - dur;   // exactly keeps End()
    if (start < 0) return Fail(err, "A clip cannot start before 0.");
    if (!CheckFree(p, c->track, start, end, clip, err)) return false;
    ShiftKeyframes(*c, c->start_us - start);   // keyframes stay with the content
    c->in_us = new_in;
    c->start_us = start;
    PruneTransitions(p);
    return true;
}

bool TrimOut(Project& p, const Id& clip, TimeUs new_end_us, Err* err) {
    Clip* c = EditableClip(p, clip, err);
    if (!c) return false;
    if (new_end_us <= c->start_us) return Fail(err, "The clip would become empty.");
    const TimeUs new_out = c->MediaTimeAt(new_end_us);
    if (new_out <= c->in_us) return Fail(err, "The clip would become empty.");
    if (new_out > p.MaxOutUs(*c)) return Fail(err, "The clip already ends at the end of its media.");
    const TimeUs end = c->start_us + DurationOf(c->in_us, new_out, c->speed);
    if (end <= c->start_us) return Fail(err, "The clip would become empty.");
    if (!CheckFree(p, c->track, c->start_us, end, clip, err)) return false;
    c->out_us = new_out;
    PruneTransitions(p);
    return true;
}

bool Slip(Project& p, const Id& clip, TimeUs delta_us, Err* err) {
    Clip* c = EditableClip(p, clip, err);
    if (!c) return false;
    const TimeUs in = c->in_us + delta_us;
    const TimeUs out = c->out_us + delta_us;
    if (in < 0) return Fail(err, "Cannot slip before the start of the media.");
    if (out > p.MaxOutUs(*c)) return Fail(err, "Cannot slip past the end of the media.");
    c->in_us = in;
    c->out_us = out;
    return true;
}

bool Split(Project& p, const Id& clip, TimeUs at_us, Id* left, Id* right, Err* err) {
    Clip* c = EditableClip(p, clip, err);
    if (!c) return false;
    if (at_us <= c->start_us || at_us >= c->End()) return Fail(err, "The split point must be inside the clip.");
    const TimeUs m = c->MediaTimeAt(at_us);
    if (m <= c->in_us || m >= c->out_us) return Fail(err, "The split point is too close to the clip edge.");
    const TimeUs orig_end = c->End();
    Clip r = *c;
    r.id = p.NewId("c");
    r.start_us = at_us;
    r.in_us = m;
    // Truncation can make the right part 1-2 us longer than the original; never let it grow into the next clip.
    while (r.End() > orig_end && r.out_us - 1 > r.in_us) --r.out_us;
    if (r.Duration() <= 0) return Fail(err, "The split point is too close to the clip edge.");
    c = p.FindClip(clip);   // NewId does not reallocate, but stay safe
    const TimeUs rel = at_us - c->start_us;
    r.keyframes.clear();
    std::map<std::string, KeyTrack<float>> left_keys;
    for (const auto& [prop, track] : c->keyframes) {
        KeyTrack<float> l, rt;
        SplitTrack(track, rel, l, rt);
        if (!l.Empty()) left_keys[prop] = std::move(l);
        if (!rt.Empty()) r.keyframes[prop] = std::move(rt);
    }
    c->keyframes = std::move(left_keys);
    c->out_us = m;
    for (Transition& t : p.transitions)
        if (t.a == clip) t.a = r.id;   // the outgoing transition now starts at the right part
    if (left) *left = clip;
    if (right) *right = r.id;
    const auto pos = std::find_if(p.clips.begin(), p.clips.end(), [&](const Clip& x) { return x.id == clip; });
    p.clips.insert(pos + 1, std::move(r));
    PruneTransitions(p);
    return true;
}

bool Delete(Project& p, const std::vector<Id>& clips, bool ripple, Err* err) {
    std::vector<Id> ids;
    for (const Id& id : clips) {
        const Clip* c = p.FindClip(id);
        if (!c) return Fail(err, "The clip does not exist.");
        if (!EditableTrack(p, c->track, err)) return false;
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    if (ids.empty()) return Fail(err, "Nothing is selected.");
    struct Removed { Id track; TimeUs start, duration; };
    std::vector<Removed> removed;
    for (const Id& id : ids) {
        const Clip* c = p.FindClip(id);
        removed.push_back({c->track, c->start_us, c->Duration()});
    }
    EraseClips(p, ids);
    if (ripple) {
        for (Clip& c : p.clips) {
            TimeUs shift = 0;
            for (const Removed& r : removed)
                if (r.track == c.track && r.start < c.start_us) shift += r.duration;
            c.start_us -= shift;
        }
    }
    PruneTransitions(p);
    return true;
}

bool Duplicate(Project& p, const Id& clip, Id* out, Err* err) {
    Clip* c = EditableClip(p, clip, err);
    if (!c) return false;
    Clip d = *c;
    d.start_us = c->End();
    if (!CheckFree(p, d.track, d.start_us, d.End(), Id(), err)) return false;
    d.id = p.NewId("c");
    if (out) *out = d.id;
    p.clips.push_back(std::move(d));
    return true;
}

bool SetSpeed(Project& p, const Id& clip, double speed, Err* err) {
    Clip* c = EditableClip(p, clip, err);
    if (!c) return false;
    if (!(speed >= kMinSpeed && speed <= kMaxSpeed)) return Fail(err, Format("Speed must be between %gx and %gx.", kMinSpeed, kMaxSpeed));
    const TimeUs dur = DurationOf(c->in_us, c->out_us, speed);
    if (dur <= 0) return Fail(err, "The clip would become empty.");
    if (!CheckFree(p, c->track, c->start_us, c->start_us + dur, clip, err)) return false;
    c->speed = speed;
    PruneTransitions(p);
    return true;
}

// ---- properties -----------------------------------------------------------------------------------------------

bool SetTransform(Project& p, const Id& clip, const Transform& t) {
    Clip* c = p.FindClip(clip);
    if (!c) return false;
    if (!Finite(t.x) || !Finite(t.y) || !Finite(t.rotation_deg) || !(t.scale > 0) || !Finite(t.scale) ||
        !(t.opacity >= 0 && t.opacity <= 1))
        return false;
    c->transform = t;
    return true;
}

bool SetCrop(Project& p, const Id& clip, const Crop& cr) {
    Clip* c = p.FindClip(clip);
    if (!c || cr.left < 0 || cr.top < 0 || cr.right < 0 || cr.bottom < 0) return false;
    if (const Media* m = p.FindMedia(c->media); m && m->width > 0 && m->height > 0)
        if (cr.left + cr.right >= m->width || cr.top + cr.bottom >= m->height) return false;
    c->crop = cr;
    return true;
}

bool SetAudio(Project& p, const Id& clip, const AudioProps& a) {
    Clip* c = p.FindClip(clip);
    if (!c || !Finite(a.gain_db) || a.fade_in_us < 0 || a.fade_out_us < 0) return false;
    if (a.noise_gate_db && !Finite(*a.noise_gate_db)) return false;
    c->audio = a;
    return true;
}

bool SetCursorStyle(Project& p, const Id& clip, const CursorStyle& s) {
    Clip* c = p.FindClip(clip);
    if (!c || !(s.scale > 0) || !Finite(s.scale) || !(s.smoothing >= 0 && s.smoothing <= 1) || s.hide_when_idle_us < 0) return false;
    c->cursor = s;
    return true;
}

bool SetKeystrokeStyle(Project& p, const Id& clip, const KeystrokeStyle& s) {
    Clip* c = p.FindClip(clip);
    if (!c || s.hold_ms < 0) return false;
    c->keystrokes = s;
    return true;
}

bool SetPip(Project& p, const Id& clip, const std::optional<Pip>& pip) {
    Clip* c = p.FindClip(clip);
    if (!c) return false;
    if (pip && (!(pip->size > 0 && pip->size <= 1) || pip->margin_px < 0 || pip->radius_px < 0)) return false;
    c->pip = pip;
    return true;
}

bool SetAnnotation(Project& p, const Id& clip, const Annotation& a) {
    Clip* c = p.FindClip(clip);
    if (!c || !c->annotation) return false;
    if (a.size_px <= 0 || a.stroke_px < 0 || a.radius_px < 0 || a.anim_ms < 0 || a.blur_px < 0) return false;
    c->annotation = a;
    return true;
}

bool SetKeyframe(Project& p, const Id& clip, const std::string& prop, TimeUs t, float v, Ease ease) {
    Clip* c = p.FindClip(clip);
    if (!c || !IsKeyframeProp(prop) || !Finite(v)) return false;
    c->keyframes[prop].Set(t, v, ease);
    return true;
}

bool RemoveKeyframe(Project& p, const Id& clip, const std::string& prop, TimeUs t) {
    Clip* c = p.FindClip(clip);
    if (!c) return false;
    auto it = c->keyframes.find(prop);
    if (it == c->keyframes.end() || !it->second.Remove(t)) return false;
    if (it->second.Empty()) c->keyframes.erase(it);
    return true;
}

// ---- transitions, camera, markers, canvas ---------------------------------------------------------------------

bool AddTransition(Project& p, Transition t, Err* err) {
    const Clip* a = p.FindClip(t.a);
    const Clip* b = p.FindClip(t.b);
    if (!a || !b) return Fail(err, "The transition needs two clips.");
    if (t.track.empty()) t.track = a->track;
    if (a->track != t.track || b->track != t.track) return Fail(err, "Both clips must be on the same track.");
    for (const Transition& x : p.transitions)
        if (x.a == t.a && x.b == t.b) return Fail(err, "There already is a transition between these clips.");
    const TimeUs gap = b->start_us - a->End();
    if (gap < 0 || gap > FrameTimeUs(1, p.canvas.fps_num, p.canvas.fps_den))
        return Fail(err, "A transition needs two clips that touch.");
    if (t.duration_us <= 0 || t.duration_us > std::min(a->Duration(), b->Duration()) / 2)
        return Fail(err, "The transition is longer than half of the shorter clip.");
    p.transitions.push_back(std::move(t));
    return true;
}

bool RemoveTransition(Project& p, const Id& a, const Id& b) {
    const auto it = std::find_if(p.transitions.begin(), p.transitions.end(),
                                 [&](const Transition& t) { return t.a == a && t.b == b; });
    if (it == p.transitions.end()) return false;
    p.transitions.erase(it);
    return true;
}

bool SetCameraKey(Project& p, CameraKey key) {
    if (!(key.zoom > 0) || !Finite(key.zoom) || !Finite(key.cx) || !Finite(key.cy)) return false;
    p.camera.SetKey(key);
    return true;
}

bool RemoveCameraKey(Project& p, TimeUs t) { return p.camera.RemoveKey(t); }

bool SetCameraMode(Project& p, Camera::Mode mode) {
    p.camera.mode = mode;
    return true;
}

bool AddMarker(Project& p, Marker m) {
    auto it = std::lower_bound(p.markers.begin(), p.markers.end(), m.t_us,
                               [](const Marker& x, TimeUs t) { return x.t_us < t; });
    if (it != p.markers.end() && it->t_us == m.t_us) return false;
    p.markers.insert(it, std::move(m));
    return true;
}

bool RemoveMarker(Project& p, TimeUs t) {
    auto it = std::find_if(p.markers.begin(), p.markers.end(), [&](const Marker& m) { return m.t_us == t; });
    if (it == p.markers.end()) return false;
    p.markers.erase(it);
    return true;
}

bool MoveMarker(Project& p, TimeUs from, TimeUs to) {
    auto it = std::find_if(p.markers.begin(), p.markers.end(), [&](const Marker& m) { return m.t_us == from; });
    if (it == p.markers.end()) return false;
    if (from == to) return true;
    if (std::any_of(p.markers.begin(), p.markers.end(), [&](const Marker& m) { return m.t_us == to; })) return false;
    Marker m = std::move(*it);
    p.markers.erase(it);
    m.t_us = to;
    return AddMarker(p, std::move(m));
}

bool SetCanvas(Project& p, const Canvas& c, Err* err) {
    if (c.width < kMinCanvas || c.width > kMaxCanvas || c.height < kMinCanvas || c.height > kMaxCanvas || (c.width & 1) || (c.height & 1))
        return Fail(err, Format("The canvas must be an even size between %d and %d pixels.", kMinCanvas, kMaxCanvas));
    if (c.fps_num <= 0 || c.fps_den <= 0) return Fail(err, "The frame rate must be positive.");
    if (c.padding_px < 0 || c.corner_radius_px < 0) return Fail(err, "Padding and corner radius cannot be negative.");
    p.canvas = c;
    PruneTransitions(p);   // the 1-frame adjacency tolerance depends on the frame rate
    return true;
}

TimeUs Snap(const Project& p, TimeUs t, TimeUs tolerance_us, const std::vector<Id>& ignore) {
    TimeUs best = t;
    TimeUs best_d = tolerance_us + 1;
    auto consider = [&](TimeUs cand) {
        const TimeUs d = cand > t ? cand - t : t - cand;
        if (d <= tolerance_us && d < best_d) {
            best = cand;
            best_d = d;
        }
    };
    consider(0);
    for (const Clip& c : p.clips) {
        if (std::find(ignore.begin(), ignore.end(), c.id) != ignore.end()) continue;
        consider(c.start_us);
        consider(c.End());
    }
    for (const Marker& m : p.markers) consider(m.t_us);
    for (const Id& id : ignore)
        if (StartsWith(id, "playhead:")) consider(std::strtoll(id.c_str() + 9, nullptr, 10));
    return best;
}

} // namespace ct::project::ops
