// ct/project/edit_ops.h: happy path and documented failures of every operation.
#include "test_util.h"

#include <ct/project/edit_ops.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>

#include <string>
#include <vector>

using namespace ct;
using namespace ct::project;
using namespace ct::project::testutil;

namespace {

bool Valid(const Project& p) {
    std::string why;
    const bool ok = p.Validate(&why);
    if (!ok) CT_FAIL(why);
    return ok;
}

} // namespace

CT_TEST(ops_media_and_tracks) {
    Project p = MakeSampleProject();
    Id m;
    CT_CHECK(ops::AddMedia(p, MakeMedia("m1", Media::Kind::Video, 1'000'000, "x.mp4"), &m));   // id taken -> new
    CT_CHECK_EQ(m, Id("m8"));
    Err err;
    CT_CHECK(!ops::RemoveMedia(p, "m1", false, &err));   // in use
    CT_CHECK(!err.Ok());
    Transition t;
    t.track = "t1";
    t.a = "c1";
    t.b = "c2";
    CT_REQUIRE(ops::AddTransition(p, t, &err));
    CT_CHECK(ops::RemoveMedia(p, "m1", true, &err));
    CT_CHECK(p.FindClip("c1") == nullptr && p.FindClip("c3") == nullptr && p.FindClip("c4") == nullptr);
    CT_CHECK(p.transitions.empty());
    CT_CHECK(p.FindMedia("m1") == nullptr);
    CT_CHECK(!ops::RemoveMedia(p, "m1", true, &err));
    Valid(p);

    Id t_new;
    CT_CHECK(ops::AddTrack(p, Track::Kind::Video, "Overlay", &t_new, 1));
    CT_CHECK_EQ(p.TrackIndex(t_new), 1);
    CT_CHECK(ops::AddTrack(p, Track::Kind::Audio, "Top", nullptr));
    CT_CHECK_EQ(p.tracks.back().name, std::string("Top"));
    CT_CHECK(ops::MoveTrack(p, t_new, 0));
    CT_CHECK_EQ(p.tracks[0].id, t_new);
    CT_CHECK(!ops::MoveTrack(p, t_new, 99));
    CT_CHECK(!ops::MoveTrack(p, "t99", 0));
    CT_CHECK(ops::RemoveTrack(p, "t5", &err));   // removes c5
    CT_CHECK(p.FindClip("c5") == nullptr);
    CT_CHECK(!ops::RemoveTrack(p, "t5", &err));
    Valid(p);
}

CT_TEST(ops_add_clip_rules) {
    Project p = MakeSampleProject();
    Err err;
    Id id;
    Clip c = MakeClip("", "t1", "m2", Clip::Source::Default, 65'000'000, 0, 10'000'000);
    CT_CHECK(ops::AddClip(p, c, &id, &err));
    CT_CHECK_EQ(id, Id("c8"));
    c.start_us = 70'000'000;   // overlaps the clip just added (65..75)
    CT_CHECK(!ops::AddClip(p, c, &id, &err));
    CT_CHECK(err.message.find("overlap") != std::string::npos);
    c.start_us = 80'000'000;
    c.out_us = 21'000'000;   // past the 20 s media
    CT_CHECK(!ops::AddClip(p, c, &id, &err));
    c.out_us = 10'000'000;
    c.track = "t3";
    c.media = "m4";   // image on an audio track
    CT_CHECK(!ops::AddClip(p, c, &id, &err));
    c.track = "t1";
    c.source = Clip::Source::Mic;   // video media with a mic source
    c.media = "m2";
    CT_CHECK(!ops::AddClip(p, c, &id, &err));
    c.source = Clip::Source::Default;
    p.FindTrack("t1")->locked = true;
    CT_CHECK(!ops::AddClip(p, c, &id, &err));
    CT_CHECK(err.message.find("locked") != std::string::npos);
    p.FindTrack("t1")->locked = false;
    c.speed = 0.01;
    CT_CHECK(!ops::AddClip(p, c, &id, &err));
    c.speed = 1;
    c.media = "m4";   // image: out is unbounded
    c.out_us = 3'600'000'000;
    CT_CHECK(ops::AddClip(p, c, &id, &err));
    Valid(p);
}

CT_TEST(ops_move_clip) {
    Project p = MakeSampleProject();
    Err err;
    CT_CHECK(ops::MoveClip(p, "c5", 40'000'000, "", &err));
    CT_CHECK_EQ(p.FindClip("c5")->start_us, TimeUs(40'000'000));
    CT_CHECK(ops::MoveClip(p, "c5", 0, "t4", &err));   // audio media onto another (empty) audio track
    CT_CHECK_EQ(p.FindClip("c5")->track, Id("t4"));
    CT_CHECK(ops::MoveClip(p, "c5", 0, "t5", &err));
    CT_CHECK(!ops::MoveClip(p, "c2", 10'000'000, "", &err));   // overlaps c1
    CT_CHECK(!ops::MoveClip(p, "c5", 0, "t1", &err));          // audio media on a video track
    CT_CHECK(!ops::MoveClip(p, "c5", -1, "", &err));
    CT_CHECK(!ops::MoveClip(p, "nope", 0, "", &err));
    CT_CHECK(!ops::MoveClip(p, "c5", 0, "t99", &err));
    Valid(p);
}

CT_TEST(ops_move_audio_clip_to_other_audio_track) {
    Project p = MakeSampleProject();
    Err err;
    // c4 is the mic stream of m1; t4 ("System") is just another audio track, the source stays "mic".
    CT_CHECK(ops::MoveClip(p, "c4", 0, "t4", &err));
    CT_CHECK_EQ(p.FindClip("c4")->track, Id("t4"));
    Valid(p);
}

CT_TEST(ops_trim_in_out_slip) {
    Project p = MakeSampleProject();
    Err err;
    // c2: t1, start 30 s, in 30 s, out 60 s.
    CT_CHECK(!ops::TrimIn(p, "c2", 29'000'000, &err));   // would overlap c1 (ends at 30 s)
    CT_CHECK(ops::TrimIn(p, "c2", 35'000'000, &err));
    const Clip* c2 = p.FindClip("c2");
    CT_CHECK_EQ(c2->start_us, TimeUs(35'000'000));
    CT_CHECK_EQ(c2->in_us, TimeUs(35'000'000));
    CT_CHECK_EQ(c2->End(), TimeUs(60'000'000));
    CT_CHECK(ops::TrimIn(p, "c2", 31'000'000, &err));   // extend back left
    CT_CHECK_EQ(p.FindClip("c2")->in_us, TimeUs(31'000'000));
    // c1 at 0 with in 0: cannot extend further left.
    CT_CHECK(!ops::TrimIn(p, "c1", -1'000'000, &err));
    CT_CHECK(!ops::TrimIn(p, "c1", 30'000'000, &err));   // empty
    CT_CHECK(ops::TrimOut(p, "c1", 20'000'000, &err));
    CT_CHECK_EQ(p.FindClip("c1")->out_us, TimeUs(20'000'000));
    CT_CHECK(!ops::TrimOut(p, "c1", 31'000'001, &err));   // overlaps c2 (now starting at 31 s)
    CT_CHECK(ops::TrimOut(p, "c1", 31'000'000, &err));     // touching is fine
    CT_CHECK(ops::TrimOut(p, "c1", 30'000'000, &err));
    CT_CHECK(!ops::TrimOut(p, "c2", 61'000'000, &err));   // past the media
    CT_CHECK(!ops::TrimOut(p, "c2", 31'000'000, &err));   // empty
    CT_CHECK(ops::TrimOut(p, "c7", 600'000'000, &err));   // image: unbounded
    // Slip c1: in 0 -> cannot go negative; slipping +5 s works (out 30 -> 35 within 60).
    CT_CHECK(!ops::Slip(p, "c1", -1, &err));
    CT_CHECK(ops::Slip(p, "c1", 5'000'000, &err));
    CT_CHECK_EQ(p.FindClip("c1")->in_us, TimeUs(5'000'000));
    CT_CHECK_EQ(p.FindClip("c1")->Duration(), TimeUs(30'000'000));
    CT_CHECK(!ops::Slip(p, "c2", 1'000'000, &err));   // out already at the media end
    Valid(p);
}

CT_TEST(ops_trim_with_speed_keeps_end_and_rebases_keyframes) {
    Project p = MakeSampleProject();
    Err err;
    Clip* c5 = p.FindClip("c5");   // t5, start 5 s, 0..30 s of a 30 s audio file
    CT_REQUIRE(ops::SetSpeed(p, "c5", 3.0, &err));
    c5 = p.FindClip("c5");
    CT_CHECK_EQ(c5->Duration(), TimeUs(10'000'000));
    const TimeUs end = c5->End();
    CT_REQUIRE(ops::SetKeyframe(p, "c5", "audio.gain_db", 2'000'000, -6.0f, Ease::Linear));
    CT_REQUIRE(ops::TrimIn(p, "c5", 6'333'333, &err));
    c5 = p.FindClip("c5");
    CT_CHECK_EQ(c5->End(), end);
    CT_CHECK_NEAR(double(c5->in_us), 3'999'999.0, 1.0);
    // The keyframe stays at the same timeline time (7 s).
    CT_CHECK_EQ(c5->start_us + c5->keyframes["audio.gain_db"].keys[0].t_us, TimeUs(7'000'000));
    Valid(p);
}

CT_TEST(ops_split_conserves_duration_and_keyframes) {
    Project p = MakeSampleProject();
    Err err;
    Transition into;
    into.track = "t1";
    into.a = "c1";
    into.b = "c2";
    CT_REQUIRE(ops::AddTransition(p, into, &err));
    CT_REQUIRE(ops::SetKeyframe(p, "c2", "transform.opacity", 0, 0.0f, Ease::Linear));
    CT_REQUIRE(ops::SetKeyframe(p, "c2", "transform.opacity", 10'000'000, 1.0f, Ease::InOut));
    CT_REQUIRE(ops::SetKeyframe(p, "c2", "transform.opacity", 20'000'000, 0.5f, Ease::Linear));
    CT_REQUIRE(ops::SetKeyframe(p, "c2", "transform.scale", 1'000'000, 2.0f, Ease::Linear));   // only before the split
    const TimeUs dur = p.FindClip("c2")->Duration();
    const TimeUs at = 30'000'000 + 4'000'000;
    const float before = p.FindClip("c2")->Prop("transform.opacity", at, 1);
    Id l, r;
    CT_REQUIRE(ops::Split(p, "c2", at, &l, &r, &err));
    CT_CHECK_EQ(l, Id("c2"));
    const Clip* L = p.FindClip(l);
    const Clip* R = p.FindClip(r);
    CT_REQUIRE(L && R);
    CT_CHECK_EQ(L->End(), at);
    CT_CHECK_EQ(R->start_us, at);
    CT_CHECK_EQ(R->in_us, L->out_us);
    CT_CHECK(L->Duration() + R->Duration() <= dur + 1);
    CT_CHECK(L->Duration() + R->Duration() >= dur - 1);
    CT_CHECK_NEAR(L->Prop("transform.opacity", at, 1), before, 1e-6);
    CT_CHECK_NEAR(R->Prop("transform.opacity", at, 1), before, 1e-6);
    CT_CHECK_NEAR(R->Prop("transform.opacity", at + 6'000'000, 1), 1.0, 1e-6);    // old key at 10 s
    CT_CHECK_NEAR(R->Prop("transform.opacity", at + 16'000'000, 1), 0.5, 1e-6);   // old key at 20 s
    CT_CHECK_NEAR(R->Prop("transform.scale", at + 1, 1), 2.0, 1e-6);   // constant after its only key
    CT_CHECK_EQ(L->keyframes.at("transform.scale").keys.size(), size_t(1));
    // The incoming transition stays on the left part.
    CT_REQUIRE(p.transitions.size() == 1);
    CT_CHECK_EQ(p.transitions[0].b, Id("c2"));
    CT_CHECK(!ops::Split(p, r, R->start_us, nullptr, nullptr, &err));   // at the edge
    CT_CHECK(!ops::Split(p, r, R->End(), nullptr, nullptr, &err));
    Valid(p);
}

CT_TEST(ops_split_with_odd_speed_never_overlaps_next) {
    Project p = MakeSampleProject();
    Err err;
    // Make c1 end exactly where c2 begins at a speed whose durations truncate.
    CT_REQUIRE(ops::SetSpeed(p, "c1", 0.7, &err) == false);   // 30 s / 0.7 would overlap c2
    CT_REQUIRE(ops::MoveClip(p, "c2", 50'000'000, "", &err));
    CT_REQUIRE(ops::SetSpeed(p, "c1", 0.7, &err));
    const TimeUs end = p.FindClip("c1")->End();
    CT_REQUIRE(ops::MoveClip(p, "c2", end, "", &err));
    for (TimeUs at = 1'234'567; at < end; at += 3'333'331) {
        const TimeUs total = [&] {
            TimeUs s = 0;
            for (const Clip* c : p.ClipsOnTrack("t1")) s += c->Duration();
            return s;
        }();
        Id l, r;
        const Clip* host = nullptr;
        for (const Clip* c : p.ClipsOnTrack("t1"))
            if (c->start_us < at && at < c->End()) host = c;
        if (!host) continue;
        const Id host_id = host->id;
        CT_REQUIRE(ops::Split(p, host_id, at, &l, &r, &err));
        TimeUs after = 0;
        for (const Clip* c : p.ClipsOnTrack("t1")) after += c->Duration();
        CT_CHECK(after <= total + 1);
        CT_CHECK(after >= total - 3);   // a few us may be lost to truncation, never gained
        Valid(p);
    }
}

CT_TEST(ops_delete_with_and_without_ripple) {
    Project p = MakeSampleProject();
    Err err;
    Id d;
    CT_REQUIRE(ops::Duplicate(p, "c2", &d, &err));   // c8 at 60..90 s
    CT_CHECK_EQ(p.FindClip(d)->start_us, TimeUs(60'000'000));
    CT_CHECK(!ops::Duplicate(p, "c1", nullptr, &err));   // 30..60 is taken by c2
    CT_CHECK(!ops::Delete(p, {"c2", "nope"}, true, &err));
    CT_CHECK(p.FindClip("c2") != nullptr);   // unchanged on failure
    CT_CHECK(ops::Delete(p, {"c2"}, false, &err));
    CT_CHECK_EQ(p.FindClip(d)->start_us, TimeUs(60'000'000));
    Project q = MakeSampleProject();
    CT_REQUIRE(ops::Duplicate(q, "c2", &d, &err));
    CT_CHECK(ops::Delete(q, {"c1", "c2"}, true, &err));
    CT_CHECK_EQ(q.FindClip(d)->start_us, TimeUs(0));   // shifted by 30 + 30 s
    CT_CHECK_EQ(q.FindClip("c3")->start_us, TimeUs(0));   // other tracks untouched
    CT_CHECK_EQ(q.FindClip("c7")->start_us, TimeUs(60'000'000));
    Valid(p);
    Valid(q);
    CT_CHECK(!ops::Delete(q, {}, false, &err));
}

CT_TEST(ops_set_speed_duration_math) {
    Project p = MakeSampleProject();
    Err err;
    CT_CHECK(ops::SetSpeed(p, "c2", 2.0, &err));
    CT_CHECK_EQ(p.FindClip("c2")->Duration(), TimeUs(15'000'000));
    CT_CHECK_EQ(p.FindClip("c2")->start_us, TimeUs(30'000'000));
    CT_CHECK(ops::SetSpeed(p, "c2", 3.0, &err));
    CT_CHECK_EQ(p.FindClip("c2")->Duration(), TimeUs(10'000'000));
    CT_CHECK(!ops::SetSpeed(p, "c1", 0.5, &err));   // 60 s would overlap c2
    CT_CHECK(!ops::SetSpeed(p, "c1", 0.0, &err));
    CT_CHECK(!ops::SetSpeed(p, "c1", 100.0, &err));
    CT_CHECK(ops::SetSpeed(p, "c4", 1.0 / 3.0, &err));   // nothing after it on t3
    CT_CHECK_EQ(p.FindClip("c4")->Duration(), TimeUs(180'000'000));
    Valid(p);
}

CT_TEST(ops_property_setters_validate) {
    Project p = MakeSampleProject();
    Transform t;
    t.opacity = 1.5f;
    CT_CHECK(!ops::SetTransform(p, "c1", t));
    t.opacity = 0.5f;
    t.scale = 0;
    CT_CHECK(!ops::SetTransform(p, "c1", t));
    t.scale = 2;
    CT_CHECK(ops::SetTransform(p, "c1", t));
    CT_CHECK_EQ(p.FindClip("c1")->transform.scale, 2.0f);
    CT_CHECK(!ops::SetTransform(p, "nope", t));
    CT_CHECK(!ops::SetCrop(p, "c1", Crop{-1, 0, 0, 0, {}}));
    CT_CHECK(!ops::SetCrop(p, "c1", Crop{1000, 0, 920, 0, {}}));   // nothing left of 1920 px
    CT_CHECK(ops::SetCrop(p, "c1", Crop{10, 10, 10, 10, {}}));
    AudioProps a;
    a.fade_in_us = -1;
    CT_CHECK(!ops::SetAudio(p, "c4", a));
    a.fade_in_us = 500'000;
    a.noise_gate_db = -40.0f;
    CT_CHECK(ops::SetAudio(p, "c4", a));
    CursorStyle cs;
    cs.smoothing = 2;
    CT_CHECK(!ops::SetCursorStyle(p, "c1", cs));
    cs.smoothing = 0.5f;
    CT_CHECK(ops::SetCursorStyle(p, "c1", cs));
    KeystrokeStyle ks;
    ks.hold_ms = -5;
    CT_CHECK(!ops::SetKeystrokeStyle(p, "c1", ks));
    Pip pip;
    pip.size = 0;
    CT_CHECK(!ops::SetPip(p, "c3", pip));
    CT_CHECK(ops::SetPip(p, "c3", std::nullopt));
    CT_CHECK(!p.FindClip("c3")->pip.has_value());
    Annotation an;
    an.text = "Hi";
    CT_CHECK(!ops::SetAnnotation(p, "c1", an));   // not an annotation clip
    CT_CHECK(ops::SetAnnotation(p, "c6", an));
    CT_CHECK_EQ(p.FindClip("c6")->annotation->text, std::string("Hi"));
    CT_CHECK(!ops::SetKeyframe(p, "c1", "transform.skew", 0, 1, Ease::Linear));
    CT_CHECK(ops::SetKeyframe(p, "c1", "transform.x", 0, 1, Ease::Linear));
    CT_CHECK(ops::RemoveKeyframe(p, "c1", "transform.x", 0));
    CT_CHECK(p.FindClip("c1")->keyframes.empty());
    CT_CHECK(!ops::RemoveKeyframe(p, "c1", "transform.x", 0));
    Valid(p);
}

CT_TEST(ops_transitions_rules_and_pruning) {
    Project p = MakeSampleProject();
    Err err;
    Transition t;
    t.track = "t1";
    t.a = "c2";
    t.b = "c1";   // wrong order: b must start where a ends
    CT_CHECK(!ops::AddTransition(p, t, &err));
    t.a = "c1";
    t.b = "c2";
    t.duration_us = 16'000'000;   // > half of 30 s
    CT_CHECK(!ops::AddTransition(p, t, &err));
    t.duration_us = 1'000'000;
    CT_CHECK(ops::AddTransition(p, t, &err));
    CT_CHECK(!ops::AddTransition(p, t, &err));   // duplicate
    Transition other;
    other.a = "c1";
    other.b = "c3";   // different tracks
    CT_CHECK(!ops::AddTransition(p, other, &err));
    // Up to one frame of gap is still "adjacent".
    CT_REQUIRE(ops::RemoveTransition(p, "c1", "c2"));
    CT_CHECK(!ops::RemoveTransition(p, "c1", "c2"));
    CT_REQUIRE(ops::MoveClip(p, "c2", 30'000'000 + 16'666, "", &err));
    CT_CHECK(ops::AddTransition(p, t, &err));
    // Moving c2 away breaks adjacency: the transition is removed.
    CT_REQUIRE(ops::MoveClip(p, "c2", 31'000'000, "", &err));
    CT_CHECK(p.transitions.empty());
    Valid(p);
}

CT_TEST(ops_camera_markers_canvas) {
    Project p = MakeSampleProject();
    CT_CHECK(ops::SetCameraKey(p, {1000, 2.0f, 0.4f, 0.4f, Ease::InOut, false}));
    CT_CHECK(ops::SetCameraKey(p, {0, 1.0f, 0.5f, 0.5f, Ease::Linear, false}));
    CT_CHECK(ops::SetCameraKey(p, {1000, 3.0f, 0.4f, 0.4f, Ease::InOut, false}));   // replace
    CT_CHECK_EQ(p.camera.keys.size(), size_t(2));
    CT_CHECK_EQ(p.camera.keys[1].zoom, 3.0f);
    CT_CHECK(!ops::SetCameraKey(p, {5, 0.0f, 0.5f, 0.5f, Ease::Linear, false}));
    CT_CHECK(ops::RemoveCameraKey(p, 0));
    CT_CHECK(!ops::RemoveCameraKey(p, 0));
    CT_CHECK(ops::SetCameraMode(p, Camera::Mode::Manual));
    CT_CHECK(ops::AddMarker(p, Marker{5'000'000, "a"}));
    CT_CHECK(!ops::AddMarker(p, Marker{5'000'000, "dup"}));
    CT_CHECK(ops::AddMarker(p, Marker{1'000'000, "first"}));
    CT_CHECK_EQ(p.markers[0].label, std::string("first"));
    CT_CHECK(ops::MoveMarker(p, 1'000'000, 20'000'000));
    CT_CHECK_EQ(p.markers.back().label, std::string("first"));
    CT_CHECK(!ops::MoveMarker(p, 20'000'000, 5'000'000));   // occupied
    CT_CHECK(!ops::MoveMarker(p, 7, 8));
    CT_CHECK(ops::RemoveMarker(p, 5'000'000));
    CT_CHECK(!ops::RemoveMarker(p, 5'000'000));
    Err err;
    Canvas c = p.canvas;
    c.width = 1081;
    CT_CHECK(!ops::SetCanvas(p, c, &err));
    c.width = 1080;
    c.height = 1920;
    CT_CHECK(ops::SetCanvas(p, c, &err));
    CT_CHECK_EQ(p.canvas.height, 1920);
    Valid(p);
}

CT_TEST(ops_snap_picks_nearest_candidate) {
    const Project p = MakeSampleProject();
    CT_CHECK_EQ(ops::Snap(p, 29'900'000, 200'000, {}), TimeUs(30'000'000));   // c1 end / c2 start
    CT_CHECK_EQ(ops::Snap(p, 29'000'000, 200'000, {}), TimeUs(29'000'000));   // nothing in range
    CT_CHECK_EQ(ops::Snap(p, 11'950'000, 100'000, {}), TimeUs(12'000'000));   // marker
    CT_CHECK_EQ(ops::Snap(p, 4'990'000, 100'000, {}), TimeUs(5'000'000));     // c5/c6 start
    CT_CHECK_EQ(ops::Snap(p, 50'000, 100'000, {}), TimeUs(0));
    CT_CHECK_EQ(ops::Snap(p, 8'950'000, 100'000, {}), TimeUs(9'000'000));     // c6 end
    CT_CHECK_EQ(ops::Snap(p, 8'950'000, 100'000, {"c6"}), TimeUs(8'950'000)); // ignored clip
    CT_CHECK_EQ(ops::Snap(p, 8'950'000, 100'000, {"c6", "playhead:8930000"}), TimeUs(8'930'000));
    // Two candidates: the nearer wins.
    CT_CHECK_EQ(ops::Snap(p, 12'040'000, 100'000, {"playhead:12100000"}), TimeUs(12'000'000));
}
