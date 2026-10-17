// ct/project/serialize.h: byte-identical round trips, fixtures, unknown keys, migration, paths, missing media.
#include "test_util.h"

#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>
#include <ct/version.h>

#include <string>
#include <vector>

using namespace ct;
using namespace ct::project;
using namespace ct::project::testutil;

namespace {

std::string ReadText(const std::string& path) {
    std::string s;
    fs::ReadAll(path, s);
    return s;
}

// Fixtures carry the app_version of the build that wrote them; compare against the current one.
std::string WithCurrentAppVersion(std::string text) {
    const std::string key = "\"app_version\": \"";
    const size_t pos = text.find(key);
    if (pos == std::string::npos) return text;
    const size_t end = text.find('"', pos + key.size());
    return text.replace(pos + key.size(), end - pos - key.size(), CT_VERSION_STRING);
}

Json Obj(std::initializer_list<std::pair<const char*, Json>> items) {
    Json o = Json::Object();
    for (const auto& [k, v] : items) o.Set(k, v);
    return o;
}

// Every feature of the model at least once, plus unknown keys at several levels.
Project BuildFullProject() {
    Project p = MakeSampleProject();
    p.canvas.background.kind = Background::Kind::Image;
    p.canvas.background.image = "C:/wallpapers/hills.jpg";
    p.canvas.background.blur = 12.5f;
    p.canvas.background.extra = Obj({{"future_noise", Json(0.25)}});
    p.canvas.shadow.opacity = 0.333333f;
    Clip* c1 = p.FindClip("c1");
    c1->name = "Intro";
    c1->color = ColorF{0.2f, 0.4f, 0.6f, 1};
    c1->transform = Transform{0.1f, -0.05f, 1.25f, 2.5f, 0.9f, {}};
    c1->crop = Crop{10, 20, 30, 40, {}};
    c1->corner_radius_px = 24;
    c1->shadow = Shadow{true, 30, 12, 0.5f, {}};
    c1->audio.gain_db = -3.5f;
    c1->audio.noise_gate_db = -45.0f;
    c1->audio.fade_in_us = 250'000;
    c1->keyframes["transform.opacity"].Set(0, 0.0f, Ease::Out);
    c1->keyframes["transform.opacity"].Set(500'000, 1.0f, Ease::Linear);
    c1->keyframes["transform.scale"].Set(1'000'000, 1.0f, Ease::InOut);
    c1->keyframes["transform.scale"].Set(2'000'000, 1.0000001f, Ease::Hold);   // %.6g collapses it
    c1->cursor.click = CursorStyle::Click::Both;
    c1->cursor.spotlight = true;
    c1->keystrokes.pos = KeystrokeStyle::Pos::TopCenter;
    c1->keystrokes.style = KeystrokeStyle::Style::Keycap;
    c1->extra = Obj({{"x_plugin", Obj({{"strength", Json(3)}})}});
    Clip* c3 = p.FindClip("c3");
    c3->pip->mask = Pip::Mask::Circle;
    c3->pip->corner = Pip::Corner::TL;
    c3->pip->mirror = true;
    c3->speed = 1.0 / 3.0;   // doubles are written exactly
    c3->out_us = 20'000'000;
    Clip* c6 = p.FindClip("c6");
    c6->annotation->type = Annotation::Type::Arrow;
    c6->annotation->text = "Unicode \xE2\x9C\x93 \"quoted\"\n";
    c6->annotation->pixelate = true;
    c6->annotation->extra = Obj({{"shape_v2", Json(true)}});
    Transition t;
    t.track = "t1";
    t.a = "c1";
    t.b = "c2";
    t.kind = Transition::Kind::Slide;
    t.dir = Transition::Dir::Up;
    t.extra = Obj({{"curve", Json("smooth")}});
    p.transitions.push_back(t);
    p.camera.mode = Camera::Mode::Manual;
    p.camera.keys.push_back({0, 1, 0.5f, 0.5f, Ease::InOut, false});
    p.camera.keys.push_back({2'000'000, 2.5f, 0.3f, 0.7f, Ease::Hold, true});
    p.camera.auto_params.on_typing = false;
    p.camera.auto_params.extra = Obj({{"smart", Json(false)}});
    p.markers.push_back(Marker{30'000'000, "middle", ColorF{1, 0, 0, 0.5f}, {}});
    p.ui.playhead_us = 1'234'567;
    p.ui.zoom = 0.1 + 0.2;
    p.ui.selected = {"c1", "c3"};
    p.ui.loop = std::make_pair(TimeUs(1'000'000), TimeUs(5'000'000));
    p.FindTrack("t1")->height_px = 72;
    p.FindTrack("t4")->muted = true;
    p.FindMedia("m2")->extra = Obj({{"codec", Json("h264")}});
    p.extra = Obj({{"future_feature", Obj({{"enabled", Json(true)}, {"list", Json::Array()}})}});
    return p;
}

const char* kFixtures[] = {"full", "minimal", "sample"};

Project BuildFixture(const std::string& name) {
    if (name == "full") return BuildFullProject();
    if (name == "sample") return MakeSampleProject();
    Project p;
    p.id = "00000000-0000-4000-8000-000000000000";
    p.name = "Empty";
    return p;
}

} // namespace

CT_TEST(serialize_round_trip_in_memory) {
    const Project p = BuildFullProject();
    const std::string a = ToJson(p);
    Project q;
    Err err;
    CT_REQUIRE(FromJson(a, q, &err));
    const std::string b = ToJson(q);
    CT_CHECK_EQ(a, b);
    std::string why;
    CT_CHECK(q.Validate(&why));
    CT_CHECK_EQ(q.FindClip("c3")->speed, 1.0 / 3.0);
    CT_CHECK_EQ(q.ui.zoom, 0.1 + 0.2);
    CT_CHECK_EQ(q.FindClip("c1")->keyframes["transform.opacity"].keys[0].ease, Ease::Out);
    CT_CHECK_EQ(q.FindClip("c6")->annotation->text, std::string("Unicode \xE2\x9C\x93 \"quoted\"\n"));
    CT_CHECK(q.FindClip("c1")->audio.noise_gate_db.has_value());
    CT_CHECK(!q.FindClip("c2")->audio.noise_gate_db.has_value());
    CT_CHECK(q.ui.loop.has_value());
    CT_CHECK_EQ(q.next_id, uint64_t(8));
}

CT_TEST(serialize_key_order_and_formats) {
    const std::string s = ToJson(BuildFullProject());
    // Top-level key order per docs/04 section 3, unknown keys last.
    const char* keys[] = {"\"format\"", "\"version\"", "\"app_version\"", "\"id\"", "\"name\"", "\"canvas\"", "\"media\"",
                          "\"tracks\"", "\"clips\"", "\"transitions\"", "\"camera\"", "\"markers\"", "\"ui\"",
                          "\"future_feature\""};
    size_t last = 0;
    for (const char* k : keys) {
        const size_t pos = s.find(std::string("\n  ") + k);
        CT_CHECK(pos != std::string::npos);
        CT_CHECK(pos >= last);
        last = pos;
    }
    CT_CHECK(s.find("\"format\": \"ctproj\"") != std::string::npos);
    CT_CHECK(s.find("\"color_a\": \"#1B2735\"") != std::string::npos);   // #RRGGBB
    CT_CHECK(s.find("\"color\": \"#FF000080\"") != std::string::npos);   // #RRGGBBAA
    CT_CHECK(s.find("\"opacity\": 0.333333") != std::string::npos);      // %.6g
    CT_CHECK(s.find("\"blur\": 12.5") != std::string::npos);
    CT_CHECK(s.find("\"ease\": \"inout\"") != std::string::npos);
    CT_CHECK(s.find("\"kind\": \"annotations\"") != std::string::npos);
    CT_CHECK(s.find("\"source\": \"screen\"") != std::string::npos);
    CT_CHECK(s.find("\"click_effect\": \"both\"") != std::string::npos);
    CT_CHECK(s.find("\"position\": \"top-center\"") != std::string::npos);
    CT_CHECK(s.find("\"corner\": \"top-left\"") != std::string::npos);
    CT_CHECK(s.find("\"trigger_on\": [\n        \"click\"\n      ]") != std::string::npos);
    CT_CHECK(s.find("\n  \"name\": \"Sample\",\n") != std::string::npos);   // 2-space indent
    CT_CHECK(s.back() == '\n');
}

CT_TEST(serialize_floats_are_stable) {
    CT_CHECK_EQ(RoundFloat(0.1f), 0.1);
    CT_CHECK_EQ(RoundFloat(1e-7f), 1e-07);
    CT_CHECK_EQ(RoundFloat(-0.0f), 0.0);
    CT_CHECK_EQ(RoundFloat(123456789.0f), 123457000.0);
    CT_CHECK_EQ(Json(RoundFloat(1e-7f)).Dump(0), std::string("1e-07"));
    CT_CHECK_EQ(Json(RoundFloat(0.45f)).Dump(0), std::string("0.45"));
    // write -> parse -> write is identical for awkward values (the parser accepts the exponent form).
    Project p;
    p.id = "x";
    p.canvas.background.angle_deg = 1e-7f;
    p.canvas.shadow.blur_px = 3.4028235e38f;
    p.canvas.shadow.offset_y_px = -1.17549435e-38f;
    const std::string a = ToJson(p);
    Project q;
    CT_REQUIRE(FromJson(a, q, nullptr));
    CT_CHECK_EQ(ToJson(q), a);
}

CT_TEST(serialize_fixtures_round_trip_byte_identical) {
    for (const char* name : kFixtures) {
        const std::string path = std::string("data/projects/") + name + ".ctproj";
        if (ct::test::UpdateGolden()) {
            CT_CHECK(fs::WriteAtomic(path, ToJson(BuildFixture(name))));
            continue;
        }
        const std::string text = ReadText(path);
        CT_REQUIRE(!text.empty());
        Project p;
        Err err;
        CT_REQUIRE(FromJson(text, p, &err));
        CT_CHECK_EQ(ToJson(p), WithCurrentAppVersion(text));
        // The fixture is what the builder produces today (catches accidental format changes).
        CT_CHECK_EQ(ToJson(BuildFixture(name)), WithCurrentAppVersion(text));
    }
}

CT_TEST(serialize_unknown_keys_preserved_everywhere) {
    const std::string text = ReadText("data/projects/full.ctproj");
    CT_REQUIRE(!text.empty());
    Project p;
    CT_REQUIRE(FromJson(text, p, nullptr));
    CT_CHECK(p.extra["future_feature"]["enabled"].AsBool());
    CT_CHECK_EQ(p.canvas.background.extra["future_noise"].AsNumber(), 0.25);
    CT_CHECK_EQ(p.FindClip("c1")->extra["x_plugin"]["strength"].AsInt(), 3);
    CT_CHECK(p.FindClip("c6")->annotation->extra["shape_v2"].AsBool());
    CT_CHECK_EQ(p.transitions[0].extra["curve"].AsString(), std::string("smooth"));
    CT_CHECK_EQ(p.FindMedia("m2")->extra["codec"].AsString(), std::string("h264"));
    CT_CHECK(p.camera.auto_params.extra.Has("smart"));
    // Hand-written document with unknown keys in places the builder does not use.
    const char* doc = R"({"format":"ctproj","version":1,"id":"u","name":"n","zz_top":1,
        "canvas":{"width":640,"height":360,"aa":[1,2],"shadow":{"enabled":false,"glow":2}},
        "tracks":[{"id":"t1","kind":"video","name":"V","lane":"x"}],
        "markers":[{"t_us":5,"label":"m","pinned":true}],
        "ui":{"playhead_us":0,"zoom":1,"scroll_us":0,"selected":[],"loop":null,"panel":"left"},
        "camera":{"mode":"auto","keyframes":[],"tilt":0}})";
    Project q;
    Err err;
    CT_REQUIRE(FromJson(doc, q, &err));
    const std::string out = ToJson(q);
    for (const char* k : {"\"zz_top\"", "\"aa\"", "\"glow\"", "\"lane\"", "\"pinned\"", "\"panel\"", "\"tilt\""})
        CT_CHECK(out.find(k) != std::string::npos);
    CT_CHECK(!q.canvas.shadow.enabled);
    Project r;
    CT_REQUIRE(FromJson(out, r, nullptr));
    CT_CHECK_EQ(ToJson(r), out);
}

CT_TEST(serialize_docs_sample_loads) {
    std::string md = ReadText("../docs/04-file-formats.md");
    CT_REQUIRE(!md.empty());
    for (char& c : md)
        if (c == '\r') c = ' ';
    const size_t start = md.find("```json\n{\n  \"format\": \"ctproj\"");
    CT_REQUIRE(start != std::string::npos);
    const size_t body = start + 8;
    const size_t end = md.find("\n```", body);
    const std::string sample = md.substr(body, end - body);
    Project p;
    Err err;
    CT_REQUIRE(FromJson(sample, p, &err));
    CT_CHECK_EQ(p.name, std::string("My recording"));
    CT_CHECK_EQ(p.media.size(), size_t(2));
    CT_CHECK_EQ(p.media[0].audio_streams.size(), size_t(2));
    CT_CHECK(p.media[0].has_camera);
    CT_CHECK_EQ(p.tracks.size(), size_t(4));
    CT_CHECK(p.tracks[3].kind == Track::Kind::Annotations);
    CT_REQUIRE(p.clips.size() == 3);
    CT_CHECK_EQ(p.clips[0].out_us, TimeUs(185'000'000));
    CT_CHECK(p.clips[0].cursor.click == CursorStyle::Click::Ripple);
    CT_CHECK_NEAR(p.clips[0].cursor.click_color.r, 0x4F / 255.0, 1e-6);
    CT_CHECK(p.clips[1].pip.has_value());
    CT_CHECK(p.clips[2].annotation.has_value());
    CT_CHECK(p.clips[2].annotation->type == Annotation::Type::Callout);
    CT_CHECK_NEAR(p.clips[2].annotation->rect.w, 0.25, 1e-6);
    CT_CHECK_EQ(p.camera.keys.size(), size_t(1));
    CT_CHECK_EQ(p.markers.size(), size_t(1));
    CT_CHECK_EQ(p.canvas.background.color_a.ToHex(), std::string("#1B2735"));
    // Placeholder values like "video|audio|image" fall back to defaults; the result is stable from then on.
    const std::string a = ToJson(p);
    Project q;
    CT_REQUIRE(FromJson(a, q, nullptr));
    CT_CHECK_EQ(ToJson(q), a);
}

CT_TEST(serialize_migrates_version_0) {
    const std::string text = ReadText("data/projects/v0.ctproj");
    CT_REQUIRE(!text.empty());
    Project p;
    Err err;
    CT_REQUIRE(FromJson(text, p, &err));
    CT_CHECK_EQ(p.version, 1);
    std::string why;
    CT_CHECK(p.Validate(&why));
    CT_REQUIRE(p.FindClip("c1") != nullptr);
    CT_CHECK_EQ(p.FindClip("c1")->out_us, TimeUs(10'000'000));
    CT_CHECK_EQ(p.FindClip("c2")->out_us, TimeUs(2'000'000 + 8'000'000));   // in + duration * speed
    CT_CHECK(p.canvas.background.kind == Background::Kind::Solid);
    CT_CHECK_EQ(p.canvas.background.color_a.ToHex(), std::string("#202020"));
    CT_CHECK_EQ(p.camera.keys.size(), size_t(2));
    const std::string out = ToJson(p);
    CT_CHECK(out.find("\"duration_us\": 4000000") == std::string::npos);
    CT_CHECK(out.find("\"keys\"") == std::string::npos);
}

CT_TEST(serialize_rejects_garbage_and_foreign_documents) {
    Project p;
    Err err;
    CT_CHECK(!FromJson("{\"format\": \"ctproj\", ", p, &err));
    CT_CHECK(err.message.find("damaged") != std::string::npos);
    err = Err{};
    CT_CHECK(!FromJson("{\"format\": \"ctrec\"}", p, &err));
    CT_CHECK(!err.Ok());
    CT_CHECK(!FromJson("[1,2]", p, nullptr));
    // Wrong types fall back to defaults instead of failing.
    CT_CHECK(FromJson(R"({"format":"ctproj","version":1,"canvas":{"width":"wide","padding_px":true}})", p, nullptr));
    CT_CHECK_EQ(p.canvas.width, 1920);
    CT_CHECK_EQ(p.canvas.padding_px, 96);
    CT_CHECK(!p.id.empty());   // generated when absent
    // A newer version loads best-effort and keeps its number.
    CT_CHECK(FromJson(R"({"format":"ctproj","version":7,"id":"n","hologram":{}})", p, nullptr));
    CT_CHECK_EQ(p.version, 7);
    CT_CHECK(ToJson(p).find("\"version\": 7") != std::string::npos);
}

CT_TEST(serialize_save_load_relative_paths_and_missing_media) {
    const std::string dir = fs::TempPath("");
    CT_REQUIRE(fs::CreateDirectories(dir + "\\media"));
    const std::string clip = dir + "\\media\\clip.mp4";
    CT_REQUIRE(fs::WriteAtomic(clip, "x"));
    Project p = MakeSampleProject();
    p.FindMedia("m2")->path = clip;                          // same drive: stored relative
    p.FindMedia("m3")->path = dir + "\\gone\\music.m4a";     // relative too, but missing
    p.FindMedia("m4")->path = "Z:\\nowhere\\logo.png";        // other drive: absolute
    const std::string proj = dir + "\\project.ctproj";
    Err err;
    CT_REQUIRE(Save(p, proj, &err));
    const std::string text = ReadText(proj);
    CT_CHECK(text.find("\"path\": \"media/clip.mp4\"") != std::string::npos);
    CT_CHECK(text.find("\"path\": \"gone/music.m4a\"") != std::string::npos);
    CT_CHECK(text.find("\"path\": \"Z:\\\\nowhere\\\\logo.png\"") != std::string::npos);
    Project q;
    CT_REQUIRE(Load(proj, q, &err));
    CT_CHECK_EQ(q.path, fs::Absolute(proj));
    CT_CHECK(IEquals(q.FindMedia("m2")->path, fs::Absolute(clip)));
    CT_CHECK(!q.FindMedia("m2")->missing);
    CT_CHECK(q.FindMedia("m3")->missing);   // project still loads
    CT_CHECK(q.FindMedia("m4")->missing);
    CT_CHECK(q.FindMedia("m1")->missing);
    // Save(Load(x)) == x, byte for byte.
    const std::string proj2 = dir + "\\copy.ctproj";
    CT_REQUIRE(Save(q, proj2, &err));
    CT_CHECK_EQ(ReadText(proj2), text);
    // Moved media: an absolute path that no longer exists is found next to the project.
    CT_REQUIRE(fs::WriteAtomic(dir + "\\beside.png", "x"));
    p.FindMedia("m4")->path = "Z:\\old\\beside.png";
    CT_REQUIRE(Save(p, proj, &err));
    CT_REQUIRE(Load(proj, q, &err));
    CT_CHECK(!q.FindMedia("m4")->missing);
    // A damaged timeline is reported, not silently loaded.
    p.FindClip("c2")->start_us = 1;
    CT_REQUIRE(Save(p, proj, &err));
    err = Err{};
    CT_CHECK(!Load(proj, q, &err));
    CT_CHECK(err.message.find("overlap") != std::string::npos);
    CT_CHECK(!Load(dir + "\\nope.ctproj", q, &err));
    fs::DeleteTree(dir);
}

CT_BENCH(serialize_to_json_full, 2000) {
    const Project p = BuildFullProject();
    size_t n = 0;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) n += ToJson(p).size();
    if (n == 0) CT_FAIL("empty");
}

CT_BENCH(serialize_from_json_full, 2000) {
    const std::string text = ToJson(BuildFullProject());
    size_t n = 0;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        Project p;
        FromJson(text, p, nullptr);
        n += p.clips.size();
    }
    if (n == 0) CT_FAIL("empty");
}

CT_BENCH(serialize_json_parse_only, 2000) {
    const std::string text = ToJson(BuildFullProject());
    size_t n = 0;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        Json j;
        Json::Parse(text, j);
        n += j.Size();
    }
    if (n == 0) CT_FAIL("empty");
}
