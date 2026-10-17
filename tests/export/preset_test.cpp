// Preset: built-in list vs assets/presets/*.json, JSON round trip, validation, random-JSON robustness, user preset
// files, EstimateBytes.
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/export/preset.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::exportng;

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
    bool Chance(int percent) { return Int(0, 99) < percent; }
};

struct TempDir {
    std::string path = fs::TempPath("");
    TempDir() { fs::CreateDirectories(path); }
    ~TempDir() { fs::DeleteTree(path); }
};

const char* const kIds[] = {"mp4-720p", "mp4-1080p", "mp4-1440p", "mp4-4k", "mp4-hevc-1080p",
                            "mp4-hevc-4k", "gif-720p", "gif-480p", "png-frame", "m4a-audio"};

Json RandomValue(Rng& r, int depth);

Json RandomScalar(Rng& r) {
    static const char* const strings[] = {"mp4", "gif", "png", "m4a", "h264", "hevc", "vbr", "cbr", "quality", "source",
                                          "30000/1001", "60", "1/0", "x/y", "aac", "high", "main", "", "../evil", "a-b_c"};
    switch (r.Int(0, 5)) {
    case 0: return Json(nullptr);
    case 1: return Json(r.Chance(50));
    case 2: return Json(r.Int(-100000, 100000));
    case 3: return Json(static_cast<double>(r.Int(-1000, 1000)) / 7.0);
    case 4: return Json(strings[r.Int(0, 19)]);
    default: return Json(static_cast<int64_t>(r.Next()));
    }
}

Json RandomObject(Rng& r, int depth, const std::vector<const char*>& keys) {
    Json o = Json::Object();
    const int n = r.Int(0, static_cast<int>(keys.size()) + 2);
    for (int i = 0; i < n; ++i) {
        const char* key = r.Chance(85) ? keys[static_cast<size_t>(r.Int(0, static_cast<int>(keys.size()) - 1))] : "junk";
        o.Set(key, RandomValue(r, depth + 1));
    }
    return o;
}

Json RandomValue(Rng& r, int depth) {
    if (depth > 3 || r.Chance(70)) return RandomScalar(r);
    if (r.Chance(50)) {
        Json a = Json::Array();
        const int n = r.Int(0, 3);
        for (int i = 0; i < n; ++i) a.Push(RandomValue(r, depth + 1));
        return a;
    }
    return RandomObject(r, depth, {"codec", "width", "fps", "bitrate_kbps", "max_colors"});
}

// Starts from a valid preset and corrupts some members with random values.
Json RandomPreset(Rng& r) {
    const std::vector<Preset> builtin = Preset::Builtin();
    Json j = builtin[static_cast<size_t>(r.Int(0, static_cast<int>(builtin.size()) - 1))].ToJson();
    if (r.Chance(10)) return RandomValue(r, 0);
    const int edits = r.Int(0, 4);
    static const char* const top[] = {"id", "name", "container", "fast_start", "video", "audio", "gif"};
    static const char* const video[] = {"codec", "width", "height", "fps", "rate_control", "bitrate_kbps",
                                        "max_bitrate_kbps", "quality", "gop_s", "bframes", "profile"};
    static const char* const audio[] = {"codec", "bitrate_kbps", "sample_rate", "channels"};
    static const char* const gif[] = {"max_colors", "dither", "per_frame_palette", "fps"};
    for (int e = 0; e < edits; ++e) {
        switch (r.Int(0, 4)) {
        case 0: j.Set(top[r.Int(0, 6)], RandomValue(r, 1)); break;
        case 1: j["video"].Set(video[r.Int(0, 10)], RandomValue(r, 2)); break;
        case 2: j["audio"].Set(audio[r.Int(0, 3)], RandomValue(r, 2)); break;
        case 3: j["gif"].Set(gif[r.Int(0, 3)], RandomValue(r, 2)); break;
        default: j.Erase(top[r.Int(0, 6)]); break;
        }
    }
    return j;
}

} // namespace

CT_TEST(preset_builtin_ids) {
    const std::vector<Preset> list = Preset::Builtin();
    CT_REQUIRE(list.size() == std::size(kIds));
    for (size_t i = 0; i < list.size(); ++i) {
        CT_CHECK_EQ(list[i].id, std::string(kIds[i]));
        CT_CHECK(Preset::IsValidId(list[i].id));
        CT_CHECK(!list[i].name.empty());
        CT_CHECK(Preset::FindBuiltin(kIds[i]) != nullptr);
    }
    CT_CHECK(Preset::FindBuiltin("nope") == nullptr);
    CT_CHECK(Preset::FindBuiltin("mp4-hevc-4k")->video.codec == VideoCodec::Hevc);
    CT_CHECK(Preset::FindBuiltin("gif-720p")->container == Preset::Container::Gif);
    CT_CHECK(Preset::FindBuiltin("m4a-audio")->container == Preset::Container::M4a);
    CT_CHECK(Preset::FindBuiltin("png-frame")->container == Preset::Container::Png);
}

CT_TEST(preset_builtin_matches_asset_files) {
    const std::string dir = "../assets/presets";   // tests run from tests/
    if (ct::test::UpdateGolden())   // --update-golden regenerates the shipped files from Builtin()
        for (const Preset& p : Preset::Builtin()) CT_CHECK(fs::WriteAtomic(PathJoin(dir, p.id + ".json"), p.ToJson().Dump(2) + "\n"));
    std::vector<fs::Entry> entries;
    CT_REQUIRE(fs::List(dir, entries));
    int json_files = 0;
    for (const auto& e : entries)
        if (EndsWith(e.name, ".json")) ++json_files;
    CT_CHECK_EQ(json_files, static_cast<int>(std::size(kIds)));
    for (const Preset& p : Preset::Builtin()) {
        std::string text, error;
        CT_REQUIRE(fs::ReadAll(PathJoin(dir, p.id + ".json"), text));
        std::erase(text, '\r');   // tolerate a CRLF checkout
        Json j;
        CT_REQUIRE(Json::Parse(text, j, &error));
        Preset loaded;
        Err err;
        CT_CHECK(Preset::FromJson(j, loaded, &err));
        CT_CHECK_EQ(err.message, std::string());
        CT_CHECK(loaded == p);
        CT_CHECK(j == p.ToJson());
        CT_CHECK_EQ(text, p.ToJson().Dump(2) + "\n");   // byte-identical: regenerate the files from Builtin()
    }
}

CT_TEST(preset_json_round_trip) {
    for (const Preset& p : Preset::Builtin()) {
        Preset q;
        CT_CHECK(Preset::FromJson(p.ToJson(), q, nullptr));
        CT_CHECK(q == p);
    }
    Preset p;
    p.id = "my_custom-1";
    p.name = "Caf\xC3\xA9 \"quoted\"";
    p.container = Preset::Container::Mp4;
    p.video.codec = VideoCodec::Hevc;
    p.video.width = 1080;
    p.video.height = 1920;
    p.video.fps_num = 30000;
    p.video.fps_den = 1001;
    p.video.rc = RateControl::Quality;
    p.video.quality = 90;
    p.video.bitrate_kbps = 1234;
    p.video.max_bitrate_kbps = 5678;
    p.video.gop_s = 0;
    p.video.bframes = false;
    p.video.profile = "main";
    p.audio = {320, 44100, 1};
    p.gif = {64, false, true, 12};
    p.fast_start = false;
    std::string text = p.ToJson().Dump(0);
    Json j;
    CT_REQUIRE(Json::Parse(text, j));
    Preset q;
    Err err;
    CT_CHECK(Preset::FromJson(j, q, &err));
    CT_CHECK_EQ(err.message, std::string());
    CT_CHECK(q == p);
    CT_CHECK_EQ(j["video"]["fps"].AsString(), std::string("30000/1001"));

    p.video.fps_num = 24;
    p.video.fps_den = 1;
    CT_CHECK_EQ(p.ToJson()["video"]["fps"].AsInt(), 24);
    CT_CHECK(Preset::FromJson(p.ToJson(), q, nullptr) && q == p);

    // Missing sections keep defaults; name defaults to the id.
    Json minimal = Json::Object();
    minimal.Set("id", "bare");
    CT_CHECK(Preset::FromJson(minimal, q, nullptr));
    CT_CHECK_EQ(q.name, std::string("bare"));
    CT_CHECK(q.container == Preset::Container::Mp4);
    CT_CHECK_EQ(q.video.bitrate_kbps, 10000u);
    CT_CHECK_EQ(q.video.fps_num, 0);
}

CT_TEST(preset_from_json_rejects_invalid) {
    const Json base = Preset::Builtin()[1].ToJson();
    auto rejects = [&](const char* section, const char* key, Json value) {
        Json j = base;
        if (section) j[section].Set(key, std::move(value));
        else j.Set(key, std::move(value));
        Preset out;
        out.id = "untouched";
        Err err;
        const bool ok = Preset::FromJson(j, out, &err);
        CT_CHECK(!ok);
        CT_CHECK(!err.message.empty());
        CT_CHECK_EQ(out.id, std::string("untouched"));
        return !ok;
    };
    rejects(nullptr, "id", Json("../evil"));
    rejects(nullptr, "id", Json(""));
    rejects(nullptr, "id", Json("UPPER"));
    rejects(nullptr, "id", Json(5));
    rejects(nullptr, "container", Json("avi"));
    rejects(nullptr, "fast_start", Json(1));
    rejects(nullptr, "video", Json(3));
    rejects("video", "codec", Json("vp9"));
    rejects("video", "width", Json(1919));
    rejects("video", "width", Json(10.5));
    rejects("video", "width", Json(-2));
    rejects("video", "height", Json(0));   // width set, height 0
    rejects("video", "fps", Json("fast"));
    rejects("video", "fps", Json("1/0"));
    rejects("video", "fps", Json(500));
    rejects("video", "bitrate_kbps", Json(20000));   // > max_bitrate_kbps 16000
    rejects("video", "quality", Json(101));
    rejects("video", "profile", Json("extended"));
    rejects("audio", "codec", Json("mp3"));
    rejects("audio", "sample_rate", Json(44000));
    rejects("audio", "channels", Json(6));
    rejects("gif", "max_colors", Json(1));
    rejects("gif", "fps", Json(60));
    Preset out;
    Err err;
    CT_CHECK(!Preset::FromJson(Json(42), out, &err));
    CT_CHECK(!Preset::FromJson(Json::Array(), out, &err));
}

CT_TEST(preset_random_json_never_crashes) {
    Rng r{12345};
    int accepted = 0;
    for (int i = 0; i < 1000; ++i) {
        const Json j = RandomPreset(r);
        Preset p;
        Err err;
        if (Preset::FromJson(j, p, &err)) {
            ++accepted;
            CT_CHECK(err.message.empty());
            CT_CHECK(Preset::IsValidId(p.id));
            Preset q;
            CT_CHECK(Preset::FromJson(p.ToJson(), q, nullptr));
            CT_CHECK(q == p);
            CT_CHECK(p.EstimateBytes(10 * kUsPerSecond) > 0);
        } else {
            CT_CHECK(!err.message.empty());
        }
        // Text-level corruption too: truncated dumps must fail to parse or be handled.
        const std::string text = j.Dump(0);
        Json k;
        if (Json::Parse(std::string_view(text).substr(0, text.size() / 2), k)) Preset::FromJson(k, p, nullptr);
    }
    if (ct::test::Verbose()) std::printf("  accepted %d / 1000\n", accepted);
    CT_CHECK(accepted > 100);
    CT_CHECK(accepted < 1000);
}

CT_TEST(preset_user_save_load) {
    TempDir dir;
    const std::string sub = PathJoin(dir.path, "presets");
    Preset a = Preset::Builtin()[0];
    a.id = "zeta";
    a.name = "Zeta";
    Preset b = Preset::Builtin()[6];
    b.id = "alpha";
    b.name = "alpha gif";
    b.gif.max_colors = 32;
    Err err;
    CT_CHECK(a.SaveUserTo(sub, &err));
    CT_CHECK(b.SaveUserTo(sub, &err));
    CT_CHECK(fs::Exists(PathJoin(sub, "zeta.json")));
    CT_CHECK(fs::WriteAtomic(PathJoin(sub, "broken.json"), "{ not json"));
    CT_CHECK(fs::WriteAtomic(PathJoin(sub, "invalid.json"), "{\"id\": \"Bad Id\"}"));
    CT_CHECK(fs::WriteAtomic(PathJoin(sub, "readme.txt"), "ignored"));
    const std::vector<Preset> loaded = Preset::LoadUserFrom(sub);
    CT_REQUIRE(loaded.size() == 2);
    CT_CHECK(loaded[0] == b);   // sorted by name, case-insensitive
    CT_CHECK(loaded[1] == a);
    // Overwrite keeps one file.
    a.video.bitrate_kbps = 7000;
    CT_CHECK(a.SaveUserTo(sub, &err));
    const std::vector<Preset> again = Preset::LoadUserFrom(sub);
    CT_REQUIRE(again.size() == 2);
    CT_CHECK_EQ(again[1].video.bitrate_kbps, 7000u);
    // Invalid presets are never written.
    Preset bad = a;
    bad.id = "a/b";
    CT_CHECK(!bad.SaveUserTo(sub, &err));
    CT_CHECK(!err.message.empty());
    CT_CHECK(Preset::LoadUserFrom(PathJoin(dir.path, "missing")).empty());
}

CT_TEST(preset_estimate_bytes) {
    const Preset& p1080 = *Preset::FindBuiltin("mp4-1080p");
    const double expect = (10000 + 192) * 1000.0 * 60 / 8;
    const double got = static_cast<double>(p1080.EstimateBytes(60 * kUsPerSecond));
    CT_CHECK(got > expect * 0.99 && got < expect * 1.03);
    CT_CHECK(p1080.EstimateBytes(0) == 0);
    CT_CHECK(p1080.EstimateBytes(-5) == 0);
    CT_CHECK(p1080.EstimateBytes(120 * kUsPerSecond) > p1080.EstimateBytes(60 * kUsPerSecond));
    CT_CHECK(Preset::FindBuiltin("mp4-4k")->EstimateBytes(kUsPerSecond) > p1080.EstimateBytes(kUsPerSecond));

    Preset q = p1080;
    q.video.rc = RateControl::Quality;
    q.video.quality = 90;
    const uint64_t q90 = q.EstimateBytes(10 * kUsPerSecond);
    q.video.quality = 50;
    const uint64_t q50 = q.EstimateBytes(10 * kUsPerSecond);
    CT_CHECK(q90 > q50);
    q.video.width = q.video.height = 0;   // canvas size
    CT_CHECK(q.EstimateBytes(10 * kUsPerSecond, {3840, 2160}, 60) > q.EstimateBytes(10 * kUsPerSecond, {1280, 720}, 60));

    const Preset& m4a = *Preset::FindBuiltin("m4a-audio");
    const double audio = 192000.0 * 30 / 8;
    const double ga = static_cast<double>(m4a.EstimateBytes(30 * kUsPerSecond));
    CT_CHECK(ga > audio && ga < audio * 1.05);
    const Preset& gif = *Preset::FindBuiltin("gif-720p");
    CT_CHECK(gif.EstimateBytes(10 * kUsPerSecond) > gif.EstimateBytes(5 * kUsPerSecond));
    const Preset& png = *Preset::FindBuiltin("png-frame");
    CT_CHECK(png.EstimateBytes(0, {1920, 1080}) > 1000000);
    CT_CHECK(png.EstimateBytes(0, {1920, 1080}) == png.EstimateBytes(100 * kUsPerSecond, {1920, 1080}));
}
