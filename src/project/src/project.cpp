#include <ct/project/project.h>

#include <ct/core/strings.h>
#include <ct/core/time.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace ct::project {

namespace {

bool Fail(std::string* why, std::string msg) {
    if (why) *why = std::move(msg);
    return false;
}

std::string Base36(uint64_t v) {
    if (v == 0) return "0";
    char buf[16];
    int n = 0;
    while (v) {
        const int d = static_cast<int>(v % 36);
        buf[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        v /= 36;
    }
    std::string s(static_cast<size_t>(n), '0');
    for (int i = 0; i < n; ++i) s[static_cast<size_t>(i)] = buf[n - 1 - i];
    return s;
}

// Counter part of "c1z" (base 36 after a one-letter prefix); 0 when the id does not follow the scheme.
uint64_t IdCounter(const Id& id) {
    if (id.size() < 2 || id.size() > 13) return 0;
    uint64_t v = 0;
    for (size_t i = 1; i < id.size(); ++i) {
        const char c = id[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else return 0;
        v = v * 36 + static_cast<uint64_t>(d);
    }
    return v;
}

} // namespace

bool IsKeyframeProp(std::string_view prop) {
    for (const char* p : kKeyframeProps)
        if (prop == p) return true;
    return false;
}

float Clip::Prop(const char* key, TimeUs t, float static_value) const {
    auto it = keyframes.find(key);
    if (it == keyframes.end() || it->second.Empty()) return static_value;
    return it->second.At(t - start_us, static_value);
}

TimeUs Project::Duration() const {
    TimeUs d = 0;
    for (const Clip& c : clips) d = std::max(d, c.End());
    return d;
}

Id Project::NewId(const char* prefix) {
    for (;;) {
        Id candidate = std::string(prefix) + Base36(next_id++);
        if (!FindMedia(candidate) && !FindTrack(candidate) && !FindClip(candidate)) return candidate;
    }
}

void Project::RecomputeNextId() {
    uint64_t mx = 0;
    for (const Media& m : media) mx = std::max(mx, IdCounter(m.id));
    for (const Track& t : tracks) mx = std::max(mx, IdCounter(t.id));
    for (const Clip& c : clips) mx = std::max(mx, IdCounter(c.id));
    next_id = mx + 1;
}

Media* Project::FindMedia(const Id& want) {
    for (Media& m : media)
        if (m.id == want) return &m;
    return nullptr;
}
Track* Project::FindTrack(const Id& want) {
    for (Track& t : tracks)
        if (t.id == want) return &t;
    return nullptr;
}
Clip* Project::FindClip(const Id& want) {
    for (Clip& c : clips)
        if (c.id == want) return &c;
    return nullptr;
}
const Media* Project::FindMedia(const Id& want) const { return const_cast<Project*>(this)->FindMedia(want); }
const Track* Project::FindTrack(const Id& want) const { return const_cast<Project*>(this)->FindTrack(want); }
const Clip* Project::FindClip(const Id& want) const { return const_cast<Project*>(this)->FindClip(want); }

int Project::TrackIndex(const Id& want) const {
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == want) return static_cast<int>(i);
    return -1;
}

std::vector<Clip*> Project::ClipsOnTrack(const Id& track) {
    std::vector<Clip*> out;
    for (Clip& c : clips)
        if (c.track == track) out.push_back(&c);
    std::stable_sort(out.begin(), out.end(), [](const Clip* a, const Clip* b) { return a->start_us < b->start_us; });
    return out;
}

std::vector<const Clip*> Project::ClipsOnTrack(const Id& track) const {
    std::vector<const Clip*> out;
    for (const Clip& c : clips)
        if (c.track == track) out.push_back(&c);
    std::stable_sort(out.begin(), out.end(), [](const Clip* a, const Clip* b) { return a->start_us < b->start_us; });
    return out;
}

std::vector<const Clip*> Project::ClipsAt(TimeUs t) const {
    std::vector<const Clip*> out;
    for (const Track& tr : tracks) {
        if (tr.hidden) continue;
        for (const Clip& c : clips)
            if (c.track == tr.id && c.start_us <= t && t < c.End()) out.push_back(&c);
    }
    return out;
}

const Clip* Project::PrimaryScreenClip() const {
    for (const Track& tr : tracks) {
        const Clip* best = nullptr;
        for (const Clip& c : clips)
            if (c.track == tr.id && c.source == Clip::Source::Screen && (!best || c.start_us < best->start_us)) best = &c;
        if (best) return best;
    }
    return nullptr;
}

TimeUs Project::MaxOutUs(const Clip& clip) const {
    constexpr TimeUs kUnbounded = (std::numeric_limits<TimeUs>::max)();
    if (clip.media.empty()) return kUnbounded;
    const Media* m = FindMedia(clip.media);
    if (!m || m->kind == Media::Kind::Image || m->duration_us <= 0) return kUnbounded;
    return m->duration_us;
}

bool Project::ClipFits(const Clip& clip, const Track& track, std::string* why) const {
    using S = Clip::Source;
    if (track.kind == Track::Kind::Annotations) {
        if (!clip.annotation) return Fail(why, "only annotations can go on an annotation track");
        if (!clip.media.empty()) return Fail(why, "annotation clip " + clip.id + " references media");
        return true;
    }
    if (clip.annotation) return Fail(why, "annotations need an annotation track");
    const Media* m = FindMedia(clip.media);
    if (!m) return Fail(why, "clip " + clip.id + " references missing media '" + clip.media + "'");
    if (track.kind == Track::Kind::Video) {
        switch (m->kind) {
        case Media::Kind::Recording:
            if (clip.source != S::Screen && clip.source != S::Camera)
                return Fail(why, "a recording on a video track must use the screen or camera source");
            if (clip.source == S::Camera && !m->has_camera) return Fail(why, "the recording has no camera");
            return true;
        case Media::Kind::Video:
        case Media::Kind::Image:
            if (clip.source != S::Default) return Fail(why, "video/image clips use the default source");
            return true;
        case Media::Kind::Audio: return Fail(why, "audio media cannot go on a video track");
        }
    } else {
        switch (m->kind) {
        case Media::Kind::Recording: {
            if (clip.source != S::Mic && clip.source != S::System)
                return Fail(why, "a recording on an audio track must use the mic or system source");
            const char* stream = clip.source == S::Mic ? "mic" : "system";
            if (std::find(m->audio_streams.begin(), m->audio_streams.end(), stream) == m->audio_streams.end())
                return Fail(why, std::string("the recording has no ") + stream + " audio");
            return true;
        }
        case Media::Kind::Video:
        case Media::Kind::Audio:
            if (clip.source != S::Default) return Fail(why, "video/audio clips use the default source");
            return true;
        case Media::Kind::Image: return Fail(why, "images cannot go on an audio track");
        }
    }
    return Fail(why, "unknown media kind");
}

bool Project::Validate(std::string* why) const {
    const Canvas& cv = canvas;
    if (cv.width < kMinCanvas || cv.width > kMaxCanvas || cv.height < kMinCanvas || cv.height > kMaxCanvas ||
        (cv.width & 1) || (cv.height & 1))
        return Fail(why, Format("canvas %dx%d must be even and within %d..%d", cv.width, cv.height, kMinCanvas, kMaxCanvas));
    if (cv.fps_num <= 0 || cv.fps_den <= 0) return Fail(why, "canvas frame rate must be positive");

    std::unordered_set<std::string> ids;
    auto unique = [&](const Id& id, const char* what) {
        if (id.empty()) return Fail(why, std::string(what) + " without id");
        if (!ids.insert(id).second) return Fail(why, "duplicate id '" + id + "'");
        return true;
    };
    for (const Media& m : media)
        if (!unique(m.id, "media")) return false;
    for (const Track& t : tracks)
        if (!unique(t.id, "track")) return false;
    for (const Clip& c : clips)
        if (!unique(c.id, "clip")) return false;

    for (const Clip& c : clips) {
        const Track* t = FindTrack(c.track);
        if (!t) return Fail(why, "clip " + c.id + " references missing track '" + c.track + "'");
        if (!ClipFits(c, *t, why)) return false;
        if (!(c.speed >= kMinSpeed && c.speed <= kMaxSpeed)) return Fail(why, Format("clip %s speed %g out of range", c.id.c_str(), c.speed));
        if (c.start_us < 0) return Fail(why, "clip " + c.id + " starts before 0");
        if (c.in_us < 0 || c.in_us >= c.out_us) return Fail(why, "clip " + c.id + " needs 0 <= in_us < out_us");
        if (c.out_us > MaxOutUs(c)) return Fail(why, "clip " + c.id + " out_us is past the end of its media");
        if (c.Duration() <= 0) return Fail(why, "clip " + c.id + " has zero duration");
        for (const auto& [prop, track] : c.keyframes) {
            if (!IsKeyframeProp(prop)) return Fail(why, "clip " + c.id + " has unknown keyframe property '" + prop + "'");
            if (!track.IsSorted()) return Fail(why, "clip " + c.id + " keyframes of " + prop + " are not sorted");
        }
    }
    for (const Track& t : tracks) {
        const std::vector<const Clip*> on = ClipsOnTrack(t.id);
        for (size_t i = 1; i < on.size(); ++i)
            if (on[i - 1]->End() > on[i]->start_us)
                return Fail(why, "clips " + on[i - 1]->id + " and " + on[i]->id + " overlap on track " + t.id);
    }
    for (const Transition& tr : transitions) {
        const Clip* a = FindClip(tr.a);
        const Clip* b = FindClip(tr.b);
        if (!a || !b) return Fail(why, "transition references a missing clip");
        if (a->track != tr.track || b->track != tr.track) return Fail(why, "transition clips are not on its track");
        const TimeUs frame = FrameTimeUs(1, canvas.fps_num, canvas.fps_den);
        const TimeUs gap = b->start_us - a->End();
        if (gap < 0 || gap > frame) return Fail(why, "transition between " + tr.a + " and " + tr.b + " needs adjacent clips");
        if (tr.duration_us <= 0 || tr.duration_us > std::min(a->Duration(), b->Duration()) / 2)
            return Fail(why, "transition between " + tr.a + " and " + tr.b + " is too long");
    }
    for (size_t i = 0; i < transitions.size(); ++i)
        for (size_t j = i + 1; j < transitions.size(); ++j)
            if (transitions[i].a == transitions[j].a && transitions[i].b == transitions[j].b)
                return Fail(why, "duplicate transition");
    if (!camera.IsSorted()) return Fail(why, "camera keys are not sorted");
    for (const CameraKey& k : camera.keys)
        if (!(k.zoom > 0)) return Fail(why, "camera zoom must be positive");
    for (size_t i = 1; i < markers.size(); ++i)
        if (markers[i - 1].t_us >= markers[i].t_us) return Fail(why, "markers are not sorted");
    return true;
}

} // namespace ct::project
