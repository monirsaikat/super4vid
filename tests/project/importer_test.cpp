// ct/project/importer.h: .ctrec bundle -> media, tracks, clips, canvas (synthetic bundles, no screen.mp4).
#include "bundle_util.h"
#include "test_util.h"

#include <ct/core/fs.h>
#include <ct/project/edit_ops.h>
#include <ct/project/importer.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>

#include <string>

using namespace ct;
using namespace ct::project;
using namespace ct::project::testutil;

namespace {

struct TempDir {
    std::string path = fs::TempPath("");
    TempDir() { fs::CreateDirectories(path); }
    ~TempDir() { fs::DeleteTree(path); }
};

const Track* TrackNamed(const Project& p, const char* name) {
    for (const Track& t : p.tracks)
        if (t.name == name) return &t;
    return nullptr;
}

} // namespace

CT_TEST(import_docs_sample_into_new_project) {
    TempDir dir;
    const Json meta = DocsMetaSample();
    CT_REQUIRE(meta.IsObject());
    const std::string bundle = MakeBundle(dir.path, "2026-10-17 12-03-44", meta,
                                          MakeEventsFile(1920, 1080, {{10'000'000, 300, 200}}, 20'000'000));
    Project p;
    p.canvas.padding_px = 0;   // not the polished default: import of a new project replaces it
    Err err;
    CT_REQUIRE(ImportRecording(p, bundle, &err, ImportOptions{true, false}));
    std::string why;
    CT_CHECK(p.Validate(&why));
    CT_CHECK(!p.id.empty());
    CT_CHECK_EQ(p.name, std::string("2026-10-17 12-03-44"));
    CT_REQUIRE(p.media.size() == 1);
    const Media& m = p.media[0];
    CT_CHECK(m.kind == Media::Kind::Recording);
    CT_CHECK_EQ(m.duration_us, TimeUs(185'000'000));
    CT_CHECK_EQ(m.width, 1920);
    CT_CHECK_EQ(m.fps_num, 60);
    CT_REQUIRE(m.audio_streams.size() == 2);
    CT_CHECK_EQ(m.audio_streams[0], std::string("mic"));
    CT_CHECK_EQ(m.audio_streams[1], std::string("system"));
    CT_CHECK(m.has_events);
    CT_CHECK(m.has_camera);
    CT_CHECK(IEquals(m.path, fs::Absolute(bundle)));
    // Tracks in the documented order.
    CT_REQUIRE(p.tracks.size() == 5);
    const char* names[] = {"Screen", "Camera", "Mic", "System", "Callouts"};
    const Track::Kind kinds[] = {Track::Kind::Video, Track::Kind::Video, Track::Kind::Audio, Track::Kind::Audio,
                                 Track::Kind::Annotations};
    for (size_t i = 0; i < 5; ++i) {
        CT_CHECK_EQ(p.tracks[i].name, std::string(names[i]));
        CT_CHECK(p.tracks[i].kind == kinds[i]);
    }
    // Clips: screen, camera (with PIP), mic, system, all 0..185 s.
    CT_REQUIRE(p.clips.size() == 4);
    CT_CHECK(p.clips[0].source == Clip::Source::Screen);
    CT_CHECK_EQ(p.clips[0].track, p.tracks[0].id);
    CT_CHECK(p.clips[1].source == Clip::Source::Camera);
    CT_CHECK(p.clips[1].pip.has_value());
    CT_CHECK(p.clips[2].source == Clip::Source::Mic);
    CT_CHECK(p.clips[3].source == Clip::Source::System);
    for (const Clip& c : p.clips) {
        CT_CHECK_EQ(c.start_us, TimeUs(0));
        CT_CHECK_EQ(c.Duration(), TimeUs(185'000'000));
    }
    CT_CHECK(p.clips[0].cursor.visible);
    CT_CHECK(p.clips[0].keystrokes.enabled);
    // Canvas from the recording, polished defaults applied.
    CT_CHECK_EQ(p.canvas.width, 1920);
    CT_CHECK_EQ(p.canvas.height, 1080);
    CT_CHECK_EQ(p.canvas.fps_num, 60);
    CT_CHECK_EQ(p.canvas.padding_px, 96);
    CT_CHECK_EQ(p.canvas.corner_radius_px, 18);
    CT_CHECK(p.canvas.background.kind == Background::Kind::Gradient);
    CT_CHECK(p.canvas.shadow.enabled);
    // The result saves and loads (media is offline: no screen.mp4 needed, the bundle folder exists).
    const std::string proj = dir.path + "\\p.ctproj";
    CT_REQUIRE(Save(p, proj, &err));
    Project q;
    CT_REQUIRE(Load(proj, q, &err));
    CT_CHECK(!q.media[0].missing);
}

CT_TEST(import_appends_and_reuses_tracks) {
    TempDir dir;
    const Json meta = DocsMetaSample();
    CT_REQUIRE(meta.IsObject());
    const std::string a = MakeBundle(dir.path, "a", meta, MakeEventsFile(1920, 1080, {}, 1'000'000));
    Json small = meta;
    small.Set("duration_us", Json(int64_t(10'000'000)));
    small.Set("camera", Json(nullptr));
    Json audio = Json::Object();
    audio.Set("mic", meta["audio"]["mic"]);
    audio.Set("system", Json(nullptr));
    small.Set("audio", audio);
    small.Set("events", Json(nullptr));
    const std::string b = MakeBundle(dir.path, "b", small, std::string());
    Project p;
    Err err;
    CT_REQUIRE(ImportRecording(p, a, &err, ImportOptions{true, false}));
    Canvas styled = p.canvas;
    styled.padding_px = 10;
    CT_REQUIRE(ops::SetCanvas(p, styled, &err));
    CT_REQUIRE(ImportRecording(p, b, &err, ImportOptions{true, false}));
    CT_CHECK_EQ(p.tracks.size(), size_t(5));   // no duplicates
    CT_CHECK_EQ(p.media.size(), size_t(2));
    const Media& mb = p.media[1];
    CT_CHECK(!mb.has_camera);
    CT_CHECK(!mb.has_events);
    CT_REQUIRE(mb.audio_streams.size() == 1);
    CT_CHECK_EQ(mb.audio_streams[0], std::string("mic"));
    CT_REQUIRE(p.clips.size() == 6);   // 4 + screen + mic
    CT_CHECK_EQ(p.clips[4].start_us, TimeUs(185'000'000));   // at the end of the timeline
    CT_CHECK_EQ(p.clips[4].track, TrackNamed(p, "Screen")->id);
    CT_CHECK_EQ(p.clips[5].track, TrackNamed(p, "Mic")->id);
    CT_CHECK_EQ(p.canvas.padding_px, 10);   // not a new project: canvas untouched
    std::string why;
    CT_CHECK(p.Validate(&why));
}

CT_TEST(import_screen_only_and_odd_size_without_style) {
    TempDir dir;
    Json meta = DocsMetaSample();
    CT_REQUIRE(meta.IsObject());
    meta.Set("camera", Json(nullptr));
    Json audio = Json::Object();
    audio.Set("mic", Json(nullptr));
    audio.Set("system", Json(nullptr));
    meta.Set("audio", audio);
    Json& screen = meta["screen"];
    screen.Set("width", 1365);
    screen.Set("height", 767);
    screen.Set("fps_num", 30);
    const std::string bundle = MakeBundle(dir.path, "odd", meta, MakeEventsFile(1365, 767, {}, 1'000'000));
    Project p;
    p.canvas.padding_px = 7;
    Err err;
    CT_REQUIRE(ImportRecording(p, bundle, &err, ImportOptions{false, false}));
    CT_CHECK_EQ(p.tracks.size(), size_t(2));   // Screen + Callouts
    CT_CHECK_EQ(p.clips.size(), size_t(1));
    CT_CHECK(p.media[0].audio_streams.empty());
    CT_CHECK_EQ(p.canvas.width, 1364);   // even
    CT_CHECK_EQ(p.canvas.height, 766);
    CT_CHECK_EQ(p.canvas.fps_num, 30);
    CT_CHECK_EQ(p.canvas.padding_px, 7);   // style left alone
}

CT_TEST(import_failures_leave_project_unchanged) {
    TempDir dir;
    Project p = MakeSampleProject();
    const std::string before = ToJson(p);
    Err err;
    CT_CHECK(!ImportRecording(p, dir.path + "\\nothing.ctrec", &err));
    CT_CHECK(err.message.find("meta.json") != std::string::npos);
    Json meta = DocsMetaSample();
    CT_REQUIRE(meta.IsObject());
    meta.Set("format", "ctproj");
    CT_CHECK(!ImportRecording(p, MakeBundle(dir.path, "wrong", meta, ""), &err));
    meta.Set("format", "ctrec");
    meta.Set("duration_us", 0);
    CT_CHECK(!ImportRecording(p, MakeBundle(dir.path, "empty", meta, ""), &err));
    meta.Set("duration_us", 1'000'000);
    meta["screen"].Set("fps_den", 0);
    CT_CHECK(!ImportRecording(p, MakeBundle(dir.path, "badfps", meta, ""), &err));
    CT_REQUIRE(fs::CreateDirectories(dir.path + "\\garbage.ctrec"));
    CT_REQUIRE(fs::WriteAtomic(dir.path + "\\garbage.ctrec\\meta.json", "{ not json"));
    CT_CHECK(!ImportRecording(p, dir.path + "\\garbage.ctrec", &err));
    // A locked Screen track makes the import fail as a whole.
    meta["screen"].Set("fps_den", 1);
    p.FindTrack("t1")->locked = true;
    const std::string locked_before = ToJson(p);
    CT_CHECK(!ImportRecording(p, MakeBundle(dir.path, "ok", meta, ""), &err));
    CT_CHECK_EQ(ToJson(p), locked_before);
    p.FindTrack("t1")->locked = false;
    CT_CHECK_EQ(ToJson(p), before);
}
