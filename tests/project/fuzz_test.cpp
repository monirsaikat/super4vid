// Random edit sequences on a seeded project: after every operation Project::Validate() holds, a failed operation
// leaves the project untouched (byte-identical JSON), and the JSON round-trips.
#include "test_util.h"

#include <ct/core/time.h>
#include <ct/project/edit_ops.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::project;
using namespace ct::project::testutil;

namespace {

struct Rng {
    uint64_t s;
    uint64_t Next() {   // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int Int(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint64_t>(hi - lo + 1)); }
    TimeUs Time(TimeUs lo, TimeUs hi) { return lo + static_cast<TimeUs>(Next() % static_cast<uint64_t>(hi - lo + 1)); }
    float Float(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Next() % 1000001) / 1000000.0f; }
    bool Chance(int percent) { return Int(0, 99) < percent; }
};

const Clip* RandomClip(const Project& p, Rng& r) {
    if (p.clips.empty()) return nullptr;
    return &p.clips[static_cast<size_t>(r.Int(0, static_cast<int>(p.clips.size()) - 1))];
}

Id RandomClipId(const Project& p, Rng& r) {
    const Clip* c = RandomClip(p, r);
    return c ? c->id : Id("c-none");
}

// Builds a clip that fits a random track (it may still overlap).
Clip RandomNewClip(const Project& p, Rng& r) {
    Clip c;
    const Track& t = p.tracks[static_cast<size_t>(r.Int(0, static_cast<int>(p.tracks.size()) - 1))];
    c.track = t.id;
    c.start_us = r.Time(0, 120'000'000);
    if (t.kind == Track::Kind::Annotations) {
        c.out_us = r.Time(500'000, 8'000'000);
        c.annotation = Annotation{};
        return c;
    }
    const bool video = t.kind == Track::Kind::Video;
    const int pick = r.Int(0, 2);
    if (video) {
        c.media = pick == 0 ? "m1" : pick == 1 ? "m2" : "m4";
        c.source = pick == 0 ? (r.Chance(70) ? Clip::Source::Screen : Clip::Source::Camera) : Clip::Source::Default;
    } else {
        c.media = pick == 0 ? "m1" : pick == 1 ? "m2" : "m3";
        c.source = pick == 0 ? (r.Chance(50) ? Clip::Source::Mic : Clip::Source::System) : Clip::Source::Default;
    }
    const Media* m = p.FindMedia(c.media);
    const TimeUs dur = m && m->duration_us > 0 ? m->duration_us : 10'000'000;
    c.in_us = r.Time(0, dur / 2);
    c.out_us = r.Time(c.in_us + 100'000, dur);
    if (r.Chance(20)) c.speed = r.Chance(50) ? 2.0 : 0.5;
    return c;
}

// One random operation. Returns whether it reported success.
bool RandomOp(Project& p, Rng& r, int* kind_out) {
    Err err;
    const int kind = p.clips.size() > 40 ? 6 : r.Int(0, 19);
    *kind_out = kind;
    switch (kind) {
    case 0: return ops::AddClip(p, RandomNewClip(p, r), nullptr, &err);
    case 1: {
        const Id id = RandomClipId(p, r);
        const Id track = r.Chance(30) ? p.tracks[static_cast<size_t>(r.Int(0, static_cast<int>(p.tracks.size()) - 1))].id : Id();
        return ops::MoveClip(p, id, r.Time(0, 120'000'000), track, &err);
    }
    case 2: {
        const Clip* c = RandomClip(p, r);
        if (!c) return false;
        const Id id = c->id;
        return ops::TrimIn(p, id, c->start_us + r.Time(-5'000'000, 5'000'000), &err);
    }
    case 3: {
        const Clip* c = RandomClip(p, r);
        if (!c) return false;
        const Id id = c->id;
        return ops::TrimOut(p, id, c->End() + r.Time(-5'000'000, 5'000'000), &err);
    }
    case 4: return ops::Slip(p, RandomClipId(p, r), r.Time(-3'000'000, 3'000'000), &err);
    case 5: {
        const Clip* c = RandomClip(p, r);
        if (!c) return false;
        const Id id = c->id;
        return ops::Split(p, id, r.Time(c->start_us - 1000, c->End() + 1000), nullptr, nullptr, &err);
    }
    case 6: {
        std::vector<Id> ids{RandomClipId(p, r)};
        if (r.Chance(30)) ids.push_back(RandomClipId(p, r));
        return ops::Delete(p, ids, r.Chance(50), &err);
    }
    case 7: return ops::Duplicate(p, RandomClipId(p, r), nullptr, &err);
    case 8: {
        static const double kSpeeds[] = {0.25, 0.5, 0.7, 1.0, 1.0 / 3.0, 1.5, 2.0, 3.0, 0.05, 20.0};
        return ops::SetSpeed(p, RandomClipId(p, r), kSpeeds[r.Int(0, 9)], &err);
    }
    case 9: {
        const char* prop = kKeyframeProps[r.Int(0, 5)];
        return ops::SetKeyframe(p, RandomClipId(p, r), prop, r.Time(-1'000'000, 20'000'000), r.Float(-2, 2),
                                static_cast<Ease>(r.Int(0, 4)));
    }
    case 10: {
        const Clip* c = RandomClip(p, r);
        if (!c || c->keyframes.empty()) return false;
        const auto& [prop, track] = *c->keyframes.begin();
        const std::string prop_copy = prop;
        const TimeUs t = track.keys[static_cast<size_t>(r.Int(0, static_cast<int>(track.keys.size()) - 1))].t_us;
        return ops::RemoveKeyframe(p, c->id, prop_copy, t);
    }
    case 11: {
        const Clip* a = RandomClip(p, r);
        if (!a) return false;
        const Clip* b = nullptr;
        for (const Clip& c : p.clips)
            if (c.track == a->track && c.start_us >= a->End() && (!b || c.start_us < b->start_us)) b = &c;
        if (!b) return false;
        Transition t;
        t.a = a->id;
        t.b = b->id;
        t.kind = static_cast<Transition::Kind>(r.Int(0, 2));
        t.duration_us = r.Time(100'000, 3'000'000);
        return ops::AddTransition(p, t, &err);
    }
    case 12: {
        if (r.Chance(30) && !p.camera.keys.empty())
            return ops::RemoveCameraKey(p, p.camera.keys[static_cast<size_t>(r.Int(0, static_cast<int>(p.camera.keys.size()) - 1))].t_us);
        CameraKey k;
        k.t_us = r.Time(0, 100'000'000);
        k.zoom = r.Float(0.5f, 4);
        k.cx = r.Float(0, 1);
        k.cy = r.Float(0, 1);
        k.ease = static_cast<Ease>(r.Int(0, 4));
        return ops::SetCameraKey(p, k);
    }
    case 13: {
        const int sub = r.Int(0, 2);
        if (sub == 0) return ops::AddMarker(p, Marker{r.Time(0, 100'000'000), "m"});
        if (p.markers.empty()) return false;
        const TimeUs t = p.markers[static_cast<size_t>(r.Int(0, static_cast<int>(p.markers.size()) - 1))].t_us;
        if (sub == 1) return ops::MoveMarker(p, t, r.Time(0, 100'000'000));
        return ops::RemoveMarker(p, t);
    }
    case 14: {
        Transform t;
        t.x = r.Float(-1, 1);
        t.y = r.Float(-1, 1);
        t.scale = r.Float(-0.5f, 3);
        t.opacity = r.Float(0, 1.2f);
        t.rotation_deg = r.Float(-180, 180);
        return ops::SetTransform(p, RandomClipId(p, r), t);
    }
    case 15: {
        const int sub = r.Int(0, 9);
        if (sub == 0 && p.tracks.size() > 4)
            return ops::RemoveTrack(p, p.tracks[static_cast<size_t>(r.Int(0, static_cast<int>(p.tracks.size()) - 1))].id, &err);
        if (sub <= 2 && p.tracks.size() < 10)
            return ops::AddTrack(p, static_cast<Track::Kind>(r.Int(0, 2)), "T", nullptr, r.Int(-1, 5));
        return ops::MoveTrack(p, p.tracks[static_cast<size_t>(r.Int(0, static_cast<int>(p.tracks.size()) - 1))].id,
                              r.Int(0, static_cast<int>(p.tracks.size())));
    }
    case 16: {
        Canvas c = p.canvas;
        static const int kFps[] = {24, 30, 60};
        c.fps_num = kFps[r.Int(0, 2)];
        c.width = r.Chance(90) ? 1920 : 1921;
        return ops::SetCanvas(p, c, &err);
    }
    case 17: {
        Crop cr{r.Int(-10, 900), r.Int(0, 500), r.Int(0, 900), r.Int(0, 500), {}};
        return ops::SetCrop(p, RandomClipId(p, r), cr);
    }
    case 18: {
        AudioProps a;
        a.gain_db = r.Float(-30, 6);
        a.fade_in_us = r.Time(-100, 2'000'000);
        a.muted = r.Chance(10);
        return ops::SetAudio(p, RandomClipId(p, r), a);
    }
    default: {
        if (p.tracks.empty()) return false;
        // Toggle a lock now and then so locked-track refusals are exercised.
        Track& t = p.tracks[static_cast<size_t>(r.Int(0, static_cast<int>(p.tracks.size()) - 1))];
        t.locked = r.Chance(15);
        return true;
    }
    }
}

Project SeededProject() {
    Project p = MakeSampleProject();
    Err err;
    ops::AddClip(p, MakeClip("", "t1", "m2", Clip::Source::Default, 70'000'000, 0, 20'000'000), nullptr, &err);
    ops::AddClip(p, MakeClip("", "t4", "m1", Clip::Source::System, 0, 0, 60'000'000), nullptr, &err);
    ops::SetKeyframe(p, "c1", "transform.scale", 0, 1, Ease::InOut);
    ops::SetKeyframe(p, "c1", "transform.scale", 3'000'000, 2, Ease::Linear);
    return p;
}

} // namespace

CT_TEST(fuzz_random_edits_keep_invariants) {
    Project p = SeededProject();
    std::string why;
    CT_REQUIRE(p.Validate(&why));
    Rng rng{20261017};
#ifdef _DEBUG
    constexpr int kOps = 2'000;   // /Od + checked iterators are ~10x slower; Release runs the full 10,000 (PROGRESS 7.7)
#else
    constexpr int kOps = 10'000;
#endif
    int ok_count = 0, fail_count = 0;
    int kind = 0;
    Stopwatch sw;
    sw.Start();
    for (int i = 0; i < kOps; ++i) {
        const std::string before = ToJson(p);
        const bool ok = RandomOp(p, rng, &kind);
        if (ok) {
            ++ok_count;
            if (!p.Validate(&why)) {
                CT_FAIL("op " + std::to_string(i) + " kind " + std::to_string(kind) + " broke an invariant: " + why);
                return;
            }
        } else {
            ++fail_count;
            if (ToJson(p) != before) {
                CT_FAIL("op " + std::to_string(i) + " kind " + std::to_string(kind) + " failed but changed the project");
                return;
            }
        }
    }
    const double ms = sw.ElapsedMs();
    std::printf("  fuzz: %d ops (%d applied, %d refused) in %.0f ms, %zu clips at the end\n", kOps, ok_count, fail_count,
                ms, p.clips.size());
    CT_CHECK(ok_count > kOps / 5);
    // The final state still round-trips.
    Project q;
    CT_REQUIRE(FromJson(ToJson(p), q, nullptr));
    CT_CHECK_EQ(ToJson(q), ToJson(p));
}
