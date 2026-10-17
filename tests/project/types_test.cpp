// ct/project/{easing,types,camera,project}.h: easing curves, keyframe tracks, camera evaluation, Validate, lookups.
#include "test_util.h"

#include <ct/project/project.h>
#include <ct/test/ct_test.h>

#include <cmath>
#include <string>

using namespace ct;
using namespace ct::project;
using namespace ct::project::testutil;

CT_TEST(easing_endpoints_and_monotonic) {
    const Ease all[] = {Ease::Linear, Ease::In, Ease::Out, Ease::InOut};
    for (Ease e : all) {
        CT_CHECK_NEAR(ApplyEase(e, 0), 0.0, 1e-6);
        CT_CHECK_NEAR(ApplyEase(e, 1), 1.0, 1e-6);
        float prev = 0;
        for (int i = 1; i <= 1000; ++i) {
            const float v = ApplyEase(e, i / 1000.0f);
            CT_CHECK(v >= prev - 1e-6f);
            prev = v;
        }
    }
    CT_CHECK_NEAR(ApplyEase(Ease::InOut, 0.5f), 0.5, 1e-6);
    CT_CHECK_NEAR(ApplyEase(Ease::In, 0.5f), 0.125, 1e-6);
    CT_CHECK_NEAR(ApplyEase(Ease::Out, 0.5f), 0.875, 1e-6);
    CT_CHECK_EQ(ApplyEase(Ease::Hold, 0.7f), 0.0f);
    CT_CHECK_EQ(ApplyEase(Ease::Linear, 2.0f), 1.0f);   // clamped
    Ease parsed = Ease::Linear;
    CT_CHECK(ParseEase("inout", &parsed));
    CT_CHECK(parsed == Ease::InOut);
    CT_CHECK(!ParseEase("bounce", &parsed));
    CT_CHECK_EQ(std::string(EaseName(Ease::Hold)), std::string("hold"));
}

CT_TEST(keytrack_at_set_remove_nearest) {
    KeyTrack<float> k;
    CT_CHECK(k.Empty());
    CT_CHECK_EQ(k.At(5, 7.0f), 7.0f);
    k.Set(1000, 10.0f, Ease::Linear);
    k.Set(0, 0.0f, Ease::Linear);
    k.Set(2000, 20.0f, Ease::Hold);
    CT_CHECK(k.IsSorted());
    CT_CHECK_EQ(k.At(-5, 0), 0.0f);
    CT_CHECK_NEAR(k.At(500, 0), 5.0, 1e-5);
    CT_CHECK_EQ(k.At(5000, 0), 20.0f);
    k.Set(1000, 12.0f, Ease::Hold);   // replace
    CT_CHECK_EQ(k.keys.size(), size_t(3));
    CT_CHECK_NEAR(k.At(1500, 0), 12.0, 1e-6);   // hold
    CT_CHECK(k.Nearest(1090, 100) != nullptr);
    CT_CHECK_EQ(k.Nearest(1090, 100)->t_us, TimeUs(1000));
    CT_CHECK(k.Nearest(1500, 100) == nullptr);
    CT_CHECK_EQ(k.Nearest(1500, 500)->t_us, TimeUs(1000));   // tie -> earlier
    CT_CHECK(k.Remove(1000));
    CT_CHECK(!k.Remove(1000));
    CT_CHECK_NEAR(k.At(1000, 0), 10.0, 1e-5);
}

CT_TEST(clip_duration_and_media_time) {
    Clip c;
    c.start_us = 1'000'000;
    c.in_us = 2'000'000;
    c.out_us = 12'000'000;
    c.speed = 2.0;
    CT_CHECK_EQ(c.Duration(), TimeUs(5'000'000));
    CT_CHECK_EQ(c.End(), TimeUs(6'000'000));
    CT_CHECK_EQ(c.MediaTimeAt(2'000'000), TimeUs(4'000'000));
    c.speed = 3.0;
    CT_CHECK_EQ(c.Duration(), TimeUs(3'333'333));   // truncates
    c.keyframes["transform.opacity"].Set(0, 0.0f, Ease::Linear);
    c.keyframes["transform.opacity"].Set(1'000'000, 1.0f, Ease::Linear);
    CT_CHECK_NEAR(c.Prop("transform.opacity", 1'500'000, 0.3f), 0.5, 1e-5);   // relative to start_us
    CT_CHECK_EQ(c.Prop("transform.scale", 1'500'000, 0.3f), 0.3f);
    CT_CHECK(IsKeyframeProp("audio.gain_db"));
    CT_CHECK(!IsKeyframeProp("transform.skew"));
}

CT_TEST(camera_at_interpolates_and_clamps) {
    Camera cam;
    Camera::View v = cam.At(123);
    CT_CHECK_EQ(v.zoom, 1.0f);
    cam.SetKey({0, 1.0f, 0.5f, 0.5f, Ease::Linear, false});
    cam.SetKey({1000, 4.0f, 0.9f, 0.1f, Ease::Hold, false});
    v = cam.At(500);
    CT_CHECK_NEAR(v.zoom, 2.0, 1e-5);   // log2 space: halfway between 1 and 4 is 2
    // linear centre would be 0.7/0.3; clamp at zoom 2 is [0.25, 0.75]
    CT_CHECK_NEAR(v.cx, 0.7, 1e-5);
    CT_CHECK_NEAR(v.cy, 0.3, 1e-5);
    v = cam.At(5000);
    CT_CHECK_NEAR(v.zoom, 4.0, 1e-6);
    CT_CHECK_NEAR(v.cx, 0.875, 1e-6);   // clamped to 1 - 1/8
    CT_CHECK_NEAR(v.cy, 0.125, 1e-6);
    CT_CHECK_NEAR(ClampCentre(0.1f, 1.0f), 0.5, 1e-6);
    CT_CHECK(cam.RemoveKey(1000));
    CT_CHECK(!cam.RemoveKey(1000));
    cam.SetKey({2000, 2.0f, 0.5f, 0.5f, Ease::InOut, true});
    cam.SetKey({3000, 2.0f, 0.5f, 0.5f, Ease::InOut, false});
    cam.ReplaceGenerated({{2500, 3.0f, 0.5f, 0.5f, Ease::InOut, false}, {3000, 9.0f, 0.5f, 0.5f, Ease::InOut, false}});
    CT_REQUIRE(cam.keys.size() == 3);   // 0 (manual), 2500 (generated), 3000 (manual wins)
    CT_CHECK(cam.keys[1].generated);
    CT_CHECK_EQ(cam.keys[2].zoom, 2.0f);
    CT_CHECK(cam.IsSorted());
}

CT_TEST(project_sample_validates_and_lookups) {
    Project p = MakeSampleProject();
    std::string why;
    CT_CHECK(p.Validate(&why));
    CT_CHECK_EQ(why, std::string());
    CT_CHECK_EQ(p.Duration(), TimeUs(65'000'000));
    CT_CHECK_EQ(p.next_id, uint64_t(8));   // c7 is the largest counter
    CT_CHECK_EQ(p.NewId("c"), Id("c8"));
    p.next_id = 35;
    CT_CHECK_EQ(p.NewId("c"), Id("cz"));
    CT_CHECK_EQ(p.NewId("c"), Id("c10"));
    p.next_id = 1;
    CT_CHECK_EQ(p.NewId("t"), Id("t7"));   // skips the existing t1..t6
    CT_REQUIRE(p.FindClip("c2") != nullptr);
    CT_CHECK(p.FindClip("nope") == nullptr);
    CT_CHECK_EQ(p.TrackIndex("t3"), 2);
    const std::vector<Clip*> on = p.ClipsOnTrack("t1");
    CT_REQUIRE(on.size() == 2);
    CT_CHECK_EQ(on[0]->id, Id("c1"));
    const std::vector<const Clip*> at = static_cast<const Project&>(p).ClipsAt(6'000'000);
    CT_REQUIRE(at.size() == 5);   // c1 (t1), c3 (t2), c4 (t3), c5 (t5), c6 (t6): bottom first
    CT_CHECK_EQ(at[0]->id, Id("c1"));
    CT_CHECK_EQ(at[4]->id, Id("c6"));
    p.FindTrack("t2")->hidden = true;
    CT_CHECK_EQ(static_cast<const Project&>(p).ClipsAt(6'000'000).size(), size_t(4));
    CT_REQUIRE(p.PrimaryScreenClip() != nullptr);
    CT_CHECK_EQ(p.PrimaryScreenClip()->id, Id("c1"));
}

CT_TEST(project_validate_rejects_each_invariant) {
    std::string why;
    {
        Project p = MakeSampleProject();
        p.canvas.width = 1921;
        CT_CHECK(!p.Validate(&why));
        p.canvas.width = 8;
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c2")->start_us = 29'000'000;   // overlaps c1
        CT_CHECK(!p.Validate(&why));
        CT_CHECK(why.find("overlap") != std::string::npos);
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c2")->out_us = 61'000'000;   // past media end
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c2")->in_us = p.FindClip("c2")->out_us;
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.clips[1].id = "c1";
        CT_CHECK(!p.Validate(&why));
        CT_CHECK(why.find("duplicate") != std::string::npos);
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c1")->track = "t9";
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c5")->track = "t1";   // audio media on a video track
        p.FindClip("c5")->start_us = 70'000'000;
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c6")->track = "t1";   // annotation on a video track
        p.FindClip("c6")->start_us = 70'000'000;
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c1")->keyframes["transform.x"].keys = {{10, 0, Ease::Linear}, {5, 1, Ease::Linear}};
        CT_CHECK(!p.Validate(&why));
        p.FindClip("c1")->keyframes.clear();
        p.FindClip("c1")->keyframes["nope"].Set(0, 1, Ease::Linear);
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c1")->speed = 0;
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        Transition t;
        t.track = "t1";
        t.a = "c1";
        t.b = "c2";
        p.transitions.push_back(t);
        CT_CHECK(p.Validate(&why));
        p.transitions[0].duration_us = 20'000'000;   // > half of the shorter clip
        CT_CHECK(!p.Validate(&why));
        p.transitions[0].duration_us = 500'000;
        p.FindClip("c2")->start_us = 31'000'000;   // no longer adjacent
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.markers.push_back(Marker{1'000'000, "early"});   // unsorted
        CT_CHECK(!p.Validate(&why));
    }
    {
        Project p = MakeSampleProject();
        p.FindClip("c4")->source = Clip::Source::System;
        p.FindMedia("m1")->audio_streams = {"mic"};   // no system stream
        CT_CHECK(!p.Validate(&why));
    }
}
