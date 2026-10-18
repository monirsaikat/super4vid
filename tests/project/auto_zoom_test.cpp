// ct/project/auto_zoom.h: fixtures (tests/data/autozoom/*.json, written by the reference implementation with
// --update-golden and reviewed by eye), continuity of Camera::At over generated keys, interest extraction, import.
#include "bundle_util.h"
#include "test_util.h"

#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/events/event_log.h>
#include <ct/project/auto_zoom.h>
#include <ct/project/importer.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::project;
using namespace ct::project::testutil;

namespace {

AutoZoomParams ParamsFrom(const Json& j) {
    AutoZoomParams p;
    p.zoom = j.Get("zoom", p.zoom);
    p.min_hold_us = j.Get<int64_t>("min_hold_us", p.min_hold_us);
    p.lead_us = j.Get<int64_t>("lead_us", p.lead_us);
    p.ease_ms = j.Get("ease_ms", p.ease_ms);
    p.follow = j.Get("follow", p.follow);
    p.follow_deadzone = j.Get("follow_deadzone", p.follow_deadzone);
    return p;
}

events::EventHeader Region(int w, int h) {
    events::EventHeader hd;
    hd.region_w = w;
    hd.region_h = h;
    return hd;
}

// Continuity: sampled every millisecond, no step larger than max_zoom_step / max_centre_step (spec: 0.02 / 0.01 for
// the default ease; much shorter eases at high zoom move faster by design, so the random test scales the bound).
bool Continuous(const std::vector<CameraKey>& keys, TimeUs duration_us, std::string* where, float max_zoom_step = 0.02f,
                float max_centre_step = 0.01f) {
    Camera cam;
    cam.keys = keys;
    Camera::View prev = cam.At(0);
    for (TimeUs t = 1000; t <= duration_us; t += 1000) {
        const Camera::View v = cam.At(t);
        if (std::fabs(v.zoom - prev.zoom) > max_zoom_step || std::fabs(v.cx - prev.cx) > max_centre_step ||
            std::fabs(v.cy - prev.cy) > max_centre_step) {
            if (where)
                *where = Format("jump at t=%lld us: zoom %.4f->%.4f cx %.4f->%.4f cy %.4f->%.4f", static_cast<long long>(t),
                                prev.zoom, v.zoom, prev.cx, v.cx, prev.cy, v.cy);
            return false;
        }
        prev = v;
    }
    return true;
}

bool WellFormed(const std::vector<CameraKey>& keys, float zoom, std::string* why) {
    for (size_t i = 0; i < keys.size(); ++i) {
        const CameraKey& k = keys[i];
        if (i && keys[i - 1].t_us >= k.t_us) *why = Format("key %zu not after key %zu", i, i - 1);
        else if (!k.generated) *why = "key not marked generated";
        else if (k.zoom < 1.0f - 1e-6f || k.zoom > zoom + 1e-6f) *why = "zoom out of range";
        else if (std::fabs(k.cx - ClampCentre(k.cx, k.zoom)) > 1e-6f || std::fabs(k.cy - ClampCentre(k.cy, k.zoom)) > 1e-6f)
            *why = Format("key %zu centre not clamped", i);
        if (!why->empty()) return false;
    }
    return true;
}

} // namespace

CT_TEST(auto_zoom_fixtures) {
    std::vector<fs::Entry> files;
    CT_REQUIRE(fs::List("data/autozoom", files));
    int checked = 0;
    for (const fs::Entry& f : files) {
        if (!EndsWith(f.name, ".json")) continue;
        const std::string path = "data/autozoom/" + f.name;
        std::string text;
        CT_REQUIRE(fs::ReadAll(path, text));
        Json j;
        CT_REQUIRE(Json::Parse(text, j));
        const Json& in = j;   // read through a const reference: the non-const operator[] would add missing keys
        const TimeUs duration = in.Get<int64_t>("duration_us", 0);
        const AutoZoomParams params = ParamsFrom(in["params"]);
        std::vector<InterestEvent> evs;
        for (const Json& e : in["events"].Elements())
            evs.push_back({e.Get<int64_t>("t_us", 0), e.Get("x", 0.5f), e.Get("y", 0.5f),
                           e["kind"].AsString() == "typing" ? InterestEvent::Kind::Typing : InterestEvent::Kind::Click});
        events::EventLog log;
        const Json& cursor = in["cursor"];
        if (cursor.IsObject()) {
            events::EventWriter w;
            w.OpenMemory(Region(cursor["region"][size_t(0)].AsInt(), cursor["region"][size_t(1)].AsInt()));
            for (const Json& s : cursor["samples"].Elements())
                w.CursorMove(s[size_t(0)].AsInt64(), s[size_t(1)].AsInt(), s[size_t(2)].AsInt(), 1, true);
            CT_REQUIRE(log.LoadFromMemory(w.Bytes(), nullptr));
        }
        const std::vector<CameraKey> keys = AutoZoom::Generate(evs, duration, params, cursor.IsObject() ? &log : nullptr);
        std::string why;
        CT_CHECK(WellFormed(keys, params.zoom, &why));
        if (!why.empty()) CT_FAIL(f.name + ": " + why);
        if (!Continuous(keys, duration, &why)) CT_FAIL(f.name + ": " + why);
        if (ct::test::UpdateGolden()) {
            Json arr = Json::Array();
            for (const CameraKey& k : keys) {
                Json o = Json::Object();
                o.Set("t_us", Json(int64_t(k.t_us)));
                o.Set("zoom", Json(RoundFloat(k.zoom)));
                o.Set("cx", Json(RoundFloat(k.cx)));
                o.Set("cy", Json(RoundFloat(k.cy)));
                o.Set("ease", EaseName(k.ease));
                arr.Push(std::move(o));
            }
            j.Set("expected_keys", std::move(arr));
            CT_CHECK(fs::WriteAtomic(path, j.Dump(2) + "\n"));
            std::printf("  %s: %zu keys written\n", f.name.c_str(), keys.size());
            ++checked;
            continue;
        }
        const Json& expected = in["expected_keys"];
        CT_REQUIRE(expected.IsArray());
        if (expected.Size() != keys.size()) {
            CT_FAIL(Format("%s: %zu keys, expected %zu", f.name.c_str(), keys.size(), expected.Size()));
            continue;
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            const Json& e = expected[i];
            CT_CHECK_EQ(keys[i].t_us, TimeUs(e.Get<int64_t>("t_us", -1)));
            CT_CHECK_NEAR(keys[i].zoom, e.Get("zoom", 0.0), 1e-4);
            CT_CHECK_NEAR(keys[i].cx, e.Get("cx", 0.0), 1e-4);
            CT_CHECK_NEAR(keys[i].cy, e.Get("cy", 0.0), 1e-4);
            CT_CHECK_EQ(std::string(EaseName(keys[i].ease)), e["ease"].AsString());
        }
        ++checked;
    }
    CT_CHECK(checked >= 8);
}

CT_TEST(auto_zoom_one_click_is_four_keys) {
    AutoZoomParams p;
    p.follow = false;
    const std::vector<CameraKey> keys = AutoZoom::Generate({{5'000'000, 0.3f, 0.4f, InterestEvent::Kind::Click}}, 20'000'000, p, nullptr);
    CT_REQUIRE(keys.size() == 4);
    CT_CHECK_EQ(keys[0].t_us, TimeUs(5'000'000 - 150'000 - 650'000));
    CT_CHECK_EQ(keys[0].zoom, 1.0f);
    CT_CHECK(keys[0].ease == Ease::InOut);
    CT_CHECK_EQ(keys[1].t_us, TimeUs(4'850'000));
    CT_CHECK_EQ(keys[1].zoom, 2.0f);
    CT_CHECK_NEAR(keys[1].cx, 0.3, 1e-6);
    CT_CHECK(keys[1].ease == Ease::Hold);
    CT_CHECK_EQ(keys[2].t_us, TimeUs(6'200'000));
    CT_CHECK(keys[2].ease == Ease::InOut);
    CT_CHECK_EQ(keys[3].t_us, TimeUs(6'850'000));
    CT_CHECK_EQ(keys[3].zoom, 1.0f);
    CT_CHECK(keys[3].ease == Ease::Hold);
    for (const CameraKey& k : keys) CT_CHECK(k.generated);
    // Deterministic, and no events / zoom 1 -> no keys.
    CT_CHECK_EQ(AutoZoom::Generate({{5'000'000, 0.3f, 0.4f, InterestEvent::Kind::Click}}, 20'000'000, p, nullptr).size(), keys.size());
    CT_CHECK(AutoZoom::Generate({}, 20'000'000, p, nullptr).empty());
    p.zoom = 1.0f;
    CT_CHECK(AutoZoom::Generate({{5'000'000, 0.3f, 0.4f, InterestEvent::Kind::Click}}, 20'000'000, p, nullptr).empty());
}

// Review fix: segments exactly 2 * ease apart put the zoom-out Hold key and the next zoom-in InOut key at the same
// time; keeping the Hold made the camera jump to full zoom at the second t_in.
CT_TEST(auto_zoom_segments_two_eases_apart_stay_continuous) {
    AutoZoomParams p;
    p.follow = false;
    // Click 1 at 5.0 s: zoom-out ends at 6.85 s. Click 2 at 7.65 s: t_in 7.5 s, zoom-in ramp starts at 6.85 s.
    const std::vector<CameraKey> keys = AutoZoom::Generate({{5'000'000, 0.3f, 0.4f, InterestEvent::Kind::Click},
                                                            {7'650'000, 0.7f, 0.6f, InterestEvent::Kind::Click}},
                                                           20'000'000, p, nullptr);
    CT_REQUIRE(keys.size() == 7);
    CT_CHECK_EQ(keys[3].t_us, TimeUs(6'850'000));
    CT_CHECK_EQ(keys[3].zoom, 1.0f);
    CT_CHECK(keys[3].ease == Ease::InOut);   // the later (zoom-in) key's ease wins
    std::string why;
    if (!WellFormed(keys, p.zoom, &why) || !Continuous(keys, 20'000'000, &why)) CT_FAIL(why);
}

CT_TEST(auto_zoom_random_inputs_stay_continuous) {
    uint64_t s = 99;
    auto next = [&] {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(s >> 33);
    };
    auto uni = [&](double lo, double hi) { return lo + (hi - lo) * (next() % 100001) / 100000.0; };
#ifdef _DEBUG
    constexpr int kCases = 40;
#else
    constexpr int kCases = 200;
#endif
    for (int c = 0; c < kCases; ++c) {
        AutoZoomParams p;
        p.zoom = static_cast<float>(uni(1.1, 4.0));
        p.ease_ms = static_cast<int>(uni(100, 1000));
        p.min_hold_us = static_cast<TimeUs>(uni(300'000, 3'000'000));
        p.lead_us = static_cast<TimeUs>(uni(0, 500'000));
        p.follow = next() % 2 == 0;
        p.follow_deadzone = static_cast<float>(uni(0.0, 0.3));
        const TimeUs duration = static_cast<TimeUs>(uni(3'000'000, 60'000'000));
        std::vector<InterestEvent> evs;
        const int n = static_cast<int>(next() % 30);
        for (int i = 0; i < n; ++i)
            evs.push_back({static_cast<TimeUs>(uni(0, double(duration))), static_cast<float>(uni(-0.1, 1.1)),
                           static_cast<float>(uni(-0.1, 1.1)), InterestEvent::Kind::Click});
        events::EventWriter w;
        w.OpenMemory(Region(1920, 1080));
        for (TimeUs t = 0; t < duration; t += static_cast<TimeUs>(uni(8'333, 400'000)))
            w.CursorMove(t, static_cast<int>(uni(0, 1920)), static_cast<int>(uni(0, 1080)), 1, true);
        events::EventLog log;
        CT_REQUIRE(log.LoadFromMemory(w.Bytes(), nullptr));
        const std::vector<CameraKey> keys = AutoZoom::Generate(evs, duration, p, &log);
        std::string why;
        // Fastest legitimate motion: the InOut ramp peaks at 3x its average slope; follow pans take 300 ms (Ease::Out, 3x).
        const float ramp = 3.0f / static_cast<float>(p.ease_ms);
        const float zoom_step = std::max(0.02f, 1.2f * p.zoom * std::log(p.zoom) * ramp);
        const float centre_step = std::max(0.01f, 1.2f * std::max(ramp, 3.0f / 300.0f));
        if (!WellFormed(keys, p.zoom, &why) || !Continuous(keys, duration, &why, zoom_step, centre_step)) {
            CT_FAIL(Format("case %d (%zu events, %zu keys): ", c, evs.size(), keys.size()) + why);
            return;
        }
        if (!keys.empty()) {
            CT_CHECK(keys.front().t_us >= 0);
            CT_CHECK(keys.back().t_us <= duration);
        }
    }
}

CT_TEST(auto_zoom_extract_interest) {
    events::EventWriter w;
    w.OpenMemory(Region(1000, 500));
    w.CursorMove(0, 100, 100, 1, true);
    w.MouseButton(1'000'000, events::Button::Left, true, 250, 125);
    w.MouseButton(1'100'000, events::Button::Left, false, 250, 125);
    w.MouseButton(2'000'000, events::Button::Middle, true, 10, 10);   // ignored
    w.MouseButton(3'000'000, events::Button::Right, true, 2000, -5);  // clamped
    events::KeyPayload k;
    k.vk = 0x41;
    k.down = 1;
    events::CopyLabel(k.label, sizeof(k.label), "A");
    w.CursorMove(4'000'000, 500, 250, 1, true);
    w.Key(5'000'000, k);
    events::EventLog log;
    CT_REQUIRE(log.LoadFromMemory(w.Bytes(), nullptr));
    AutoZoomParams p;
    std::vector<InterestEvent> e = AutoZoom::ExtractInterest(log, SizeI{1000, 500}, p);
    CT_REQUIRE(e.size() == 3);
    CT_CHECK_NEAR(e[0].x, 0.25, 1e-6);
    CT_CHECK_NEAR(e[0].y, 0.25, 1e-6);
    CT_CHECK_EQ(e[1].x, 1.0f);
    CT_CHECK_EQ(e[1].y, 0.0f);
    CT_CHECK(e[2].kind == InterestEvent::Kind::Typing);
    CT_CHECK_NEAR(e[2].x, 0.5, 1e-6);   // cursor position at the burst start
    p.on_typing = false;
    CT_CHECK_EQ(AutoZoom::ExtractInterest(log, SizeI{1000, 500}, p).size(), size_t(2));
    p.on_click = false;
    CT_CHECK(AutoZoom::ExtractInterest(log, SizeI{1000, 500}, p).empty());
}

CT_TEST(auto_zoom_for_clip_maps_to_timeline) {
    events::EventWriter w;
    w.OpenMemory(Region(1920, 1080));
    w.MouseButton(2'000'000, events::Button::Left, true, 960, 540);    // before the clip's in point: skipped
    w.MouseButton(12'000'000, events::Button::Left, true, 960, 540);
    events::EventLog log;
    CT_REQUIRE(log.LoadFromMemory(w.Bytes(), nullptr));
    Clip c;
    c.start_us = 100'000'000;
    c.in_us = 10'000'000;
    c.out_us = 30'000'000;
    c.speed = 2.0;
    AutoZoomParams p;
    p.follow = false;
    const std::vector<CameraKey> keys = AutoZoom::ForClip(c, log, SizeI{1920, 1080}, p);
    CT_REQUIRE(keys.size() == 4);
    // Media 12 s -> 2 s into the clip -> timeline 100 s + (2 s - 0.8 s) / 2.
    CT_CHECK_EQ(keys[0].t_us, TimeUs(100'000'000 + (2'000'000 - 800'000) / 2));
    Camera cam;
    cam.keys.push_back({0, 1.5f, 0.5f, 0.5f, Ease::Linear, false});
    cam.keys.push_back({keys[1].t_us, 1.2f, 0.5f, 0.5f, Ease::Linear, false});   // a manual key at the same time wins
    AutoZoom::AppendKeys(cam, keys);
    CT_CHECK_EQ(cam.keys.size(), size_t(5));
    CT_CHECK(!cam.keys[2].generated);
    CT_CHECK(cam.IsSorted());
}

CT_TEST(auto_zoom_applied_on_import) {
    const std::string dir = fs::TempPath("");
    fs::CreateDirectories(dir);
    const Json meta = DocsMetaSample();
    CT_REQUIRE(meta.IsObject());
    const std::string bundle = MakeBundle(dir, "clicks", meta,
                                          MakeEventsFile(1920, 1080, {{10'000'000, 480, 270}, {60'000'000, 1440, 810}}, 70'000'000));
    Project p;
    Err err;
    CT_REQUIRE(ImportRecording(p, bundle, &err));
    CT_CHECK(p.camera.mode == Camera::Mode::Auto);
    CT_CHECK(p.camera.keys.size() >= 8);
    for (const CameraKey& k : p.camera.keys) CT_CHECK(k.generated);
    const size_t first = p.camera.keys.size();
    // A second import appends keys offset by the new clip's start (185 s).
    CT_REQUIRE(ImportRecording(p, bundle, &err));
    CT_REQUIRE(p.camera.keys.size() == 2 * first);
    CT_CHECK_EQ(p.camera.keys[first].t_us, p.camera.keys[0].t_us + 185'000'000);
    CT_CHECK(p.camera.IsSorted());
    std::string why;
    CT_CHECK(p.Validate(&why));
    // auto_zoom off: no keys.
    Project q;
    CT_REQUIRE(ImportRecording(q, bundle, &err, ImportOptions{true, false}));
    CT_CHECK(q.camera.keys.empty());
    fs::DeleteTree(dir);
}

namespace {

// One recording (1920x1080, events), a plain video, two screen clips of the recording (the second trimmed, at 2x),
// a mic clip of the recording and a clip of the plain video.
Project RegenProject() {
    Project p;
    p.id = "regen";
    p.media.push_back(MakeMedia("rec", Media::Kind::Recording, 60'000'000, "C:/r/take.ctrec"));
    p.media.back().has_events = true;
    p.media.back().audio_streams = {"mic"};
    p.media.push_back(MakeMedia("vid", Media::Kind::Video, 60'000'000, "C:/r/broll.mp4"));
    p.tracks.push_back(MakeTrack("t1", Track::Kind::Video, "Screen"));
    p.tracks.push_back(MakeTrack("t2", Track::Kind::Audio, "Mic"));
    p.clips.push_back(MakeClip("c2", "t1", "rec", Clip::Source::Screen, 40'000'000, 20'000'000, 40'000'000));
    p.clips.back().speed = 2.0;
    p.clips.push_back(MakeClip("c1", "t1", "rec", Clip::Source::Screen, 0, 0, 20'000'000));
    p.clips.push_back(MakeClip("c3", "t2", "rec", Clip::Source::Mic, 0, 0, 20'000'000));
    p.clips.push_back(MakeClip("c4", "t1", "vid", Clip::Source::Default, 100'000'000, 0, 20'000'000));
    p.RecomputeNextId();
    p.camera.auto_params.follow = false;
    return p;
}

events::EventLog RegenLog() {
    events::EventWriter w;
    w.OpenMemory(Region(1920, 1080));
    w.MouseButton(5'000'000, events::Button::Left, true, 480, 270);     // c1
    w.MouseButton(30'000'000, events::Button::Left, true, 1440, 810);   // c2 (media 30 s)
    events::EventLog log;
    log.LoadFromMemory(w.Bytes(), nullptr);
    return log;
}

} // namespace

CT_TEST(auto_zoom_regenerate_covers_screen_clips_of_recordings) {
    Project p = RegenProject();
    const events::EventLog log = RegenLog();
    int lookups = 0;
    const AutoZoom::LogLookup lookup = [&](const Media& m) -> const events::EventLog* {
        ++lookups;
        return m.id == "rec" ? &log : nullptr;
    };
    int clips = 0;
    const std::vector<CameraKey> keys = AutoZoom::ForProject(p, lookup, &clips);
    CT_CHECK_EQ(clips, 2);
    CT_CHECK_EQ(lookups, 2);   // only screen clips are looked up (the mic clip and the plain video clip are not)
    std::vector<CameraKey> want = AutoZoom::ForClip(*p.FindClip("c1"), log, SizeI{1920, 1080}, p.camera.auto_params);
    const std::vector<CameraKey> k2 = AutoZoom::ForClip(*p.FindClip("c2"), log, SizeI{1920, 1080}, p.camera.auto_params);
    CT_REQUIRE(want.size() == 4 && k2.size() == 4);
    want.insert(want.end(), k2.begin(), k2.end());
    CT_REQUIRE(keys.size() == want.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        CT_CHECK_EQ(keys[i].t_us, want[i].t_us);
        CT_CHECK(keys[i].generated);
    }
    // c2: media 30 s -> 10 s into the clip at 2x -> timeline 40 s + (10 s - 0.8 s) / 2.
    CT_CHECK_EQ(keys[4].t_us, TimeUs(40'000'000 + (10'000'000 - 800'000) / 2));

    // Regenerate replaces generated keys only; a manual key survives and wins at its time.
    CameraKey manual;
    manual.t_us = 70'000'000;
    manual.zoom = 1.5f;
    p.camera.keys.push_back(manual);
    CameraKey stale;
    stale.t_us = 80'000'000;
    stale.generated = true;
    p.camera.keys.push_back(stale);
    bool changed = false;
    CT_REQUIRE(AutoZoom::Regenerate(p, lookup, &changed, &clips));
    CT_CHECK(changed);
    CT_CHECK_EQ(p.camera.keys.size(), size_t(9));
    CT_CHECK(p.camera.IsSorted());
    bool has_manual = false, has_stale = false;
    for (const CameraKey& k : p.camera.keys) {
        has_manual |= k.t_us == 70'000'000 && !k.generated;
        has_stale |= k.t_us == 80'000'000;
    }
    CT_CHECK(has_manual);
    CT_CHECK(!has_stale);
    std::string why;
    if (!p.Validate(&why)) CT_FAIL(why);
    // Again with the same inputs: nothing changes (the editor pushes no undo entry then).
    CT_REQUIRE(AutoZoom::Regenerate(p, lookup, &changed));
    CT_CHECK(!changed);
    // New parameters -> new keys.
    p.camera.auto_params.zoom = 3.0f;
    CT_REQUIRE(AutoZoom::Regenerate(p, lookup, &changed));
    CT_CHECK(changed);
    float max_zoom = 0;
    for (const CameraKey& k : p.camera.keys)
        if (k.generated) max_zoom = std::max(max_zoom, k.zoom);
    CT_CHECK_NEAR(max_zoom, 3.0f, 1e-6f);
    // on_click off and no typing: no interest events -> no generated keys, the manual key stays.
    p.camera.auto_params.on_click = false;
    CT_REQUIRE(AutoZoom::Regenerate(p, lookup, &changed));
    CT_CHECK(changed);
    CT_CHECK_EQ(p.camera.keys.size(), size_t(1));
    // No log at all: false and the project is unchanged.
    const std::string before = ToJson(p);
    CT_CHECK(!AutoZoom::Regenerate(p, [](const Media&) -> const events::EventLog* { return nullptr; }, &changed, &clips));
    CT_CHECK_EQ(clips, 0);
    CT_CHECK(!changed);
    CT_CHECK_EQ(ToJson(p), before);
}

// Regenerate runs on the UI thread (button, live after a parameter change): a 30 min recording with a click every
// 4 s, typing and 120 Hz cursor samples, follow on, must stay far below a frame.
CT_TEST(auto_zoom_regenerate_long_recording_is_fast) {
    const TimeUs dur = 30LL * 60 * 1'000'000;
    std::vector<Click> clicks;
    for (TimeUs t = 2'000'000; t < dur - 2'000'000; t += 4'000'000)
        clicks.push_back({t, static_cast<int>((t / 1000) % 1800) + 60, static_cast<int>((t / 7000) % 1000) + 40});
    events::EventLog log;
    CT_REQUIRE(log.LoadFromMemory(MakeEventsFile(1920, 1080, clicks, dur), nullptr));
    Project p = RegenProject();
    p.media[0].duration_us = dur;
    p.clips.erase(p.clips.begin());   // c2 (in at 20 s) would overlap c1 below
    p.FindClip("c1")->out_us = dur;
    p.camera.auto_params.follow = true;
    const AutoZoom::LogLookup lookup = [&](const Media& m) -> const events::EventLog* { return m.id == "rec" ? &log : nullptr; };
    double best_ms = 1e9;
    size_t n = 0;
    for (int i = 0; i < 5; ++i) {
        Project q = p;
        Stopwatch sw;
        sw.Start();
        CT_REQUIRE(AutoZoom::Regenerate(q, lookup));
        best_ms = std::min(best_ms, sw.ElapsedMs());
        n = q.camera.keys.size();
    }
    std::printf("    regenerate 30 min, %zu clicks, follow: %zu keys, best %.2f ms\n", clicks.size(), n, best_ms);
    CT_CHECK(n > clicks.size());
#if defined(NDEBUG)
    CT_CHECK(best_ms < 50.0);
#endif
}
