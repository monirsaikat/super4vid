// RecordingSession + BundleWriter + meta.json (P05 tasks 5.1/5.2): bundle naming, meta layout, and real recordings
// (device tests) through the whole state machine: prepare, start, pause/resume, stop, cancel, prepare failure.
#include <ct/capture/display.h>
#include <ct/core/fs.h>
#include <ct/core/json.h>
#include <ct/core/time.h>
#include <ct/platform/d3d_device.h>
#include <ct/recorder/bundle_writer.h>
#include <ct/recorder/headless.h>
#include <ct/recorder/recording_session.h>
#include <ct/test/ct_test.h>

#include <atomic>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::recorder;

namespace {

std::string NewTempDir() {
    std::string d = fs::TempPath("");
    fs::CreateDirectories(d);
    return d;
}

// Without a UI poster the callbacks run on ct-session: atomics only.
struct TestListener final : SessionListener {
    std::atomic<int> states{0}, ticks{0}, finished{0}, errors{0}, prepared{0};
    std::atomic<int> last_state{0};
    std::atomic<HRESULT> prepare_hr{S_OK};
    void OnStateChanged(State s) override {
        ++states;
        last_state.store(static_cast<int>(s));
    }
    void OnCountdown(int) override {}
    void OnTick(const LiveStats&) override { ++ticks; }
    void OnFinished(const std::string&) override { ++finished; }
    void OnError(const std::string&, HRESULT) override { ++errors; }
    void OnPrepared(HRESULT hr) override {
        prepare_hr.store(hr);
        ++prepared;
    }
};

void WaitMediaUs(const RecordingSession& s, TimeUs t) {
    const int64_t end = Clock::QpcNow() + 20 * Clock::QpcFrequency();
    // StartNow is asynchronous: the clock is (re)started once the session reaches Recording.
    while (s.GetState() != State::Recording && s.GetState() != State::Paused && Clock::QpcNow() < end) Sleep(5);
    while (s.Clock().NowMediaUs() < t && Clock::QpcNow() < end) Sleep(10);
}

bool BaseConfig(platform::D3DDevice& dev, SessionConfig& cfg) {
    if (FAILED(dev.Init(false))) return false;
    const auto displays = capture::EnumerateDisplays(dev.Factory());
    if (displays.empty()) return false;
    cfg.display = displays[0];
    cfg.region = RectI{cfg.display.rect.x, cfg.display.rect.y, 640, 360};
    cfg.fps = 30;
    cfg.mic = false;
    cfg.system_audio = true;
    cfg.events = true;
    cfg.output_dir = NewTempDir();
    return true;
}

} // namespace

CT_TEST(bundle_name_collision_gets_suffix) {
    const std::string dir = NewTempDir();
    BundleWriter a, b, c;
    std::string pa, pb, pc;
    CT_REQUIRE(a.CreateNamed(dir, "2026-10-17 12-03-44", pa));
    CT_REQUIRE(b.CreateNamed(dir, "2026-10-17 12-03-44", pb));
    CT_REQUIRE(c.CreateNamed(dir, "2026-10-17 12-03-44", pc));
    CT_CHECK(pa.ends_with("\\2026-10-17 12-03-44.ctrec"));
    CT_CHECK(pb.ends_with("\\2026-10-17 12-03-44 (2).ctrec"));
    CT_CHECK(pc.ends_with("\\2026-10-17 12-03-44 (3).ctrec"));
    CT_CHECK(fs::IsDirectory(pb));
    CT_CHECK(b.ScreenPath() == pb + "\\screen.mp4");
    CT_CHECK(b.EventsPath() == pb + "\\events.ctev");
    Json j = Json::Object();
    j.Set("format", "ctrec");
    CT_CHECK(b.WriteMeta(j));
    CT_CHECK(fs::Exists(b.MetaPath()));
    CT_CHECK(b.Delete());
    CT_CHECK(!fs::Exists(pb));
    BundleWriter d;
    std::string pd;
    CT_REQUIRE(d.Create(dir, pd));   // local time stamp name
    CT_CHECK(pd.ends_with(".ctrec") && pd.size() > dir.size() + 20);
    fs::DeleteTree(dir);
}

CT_TEST(meta_json_layout_matches_docs04) {
    BundleMeta m;
    m.app_version = "0.1.0";
    m.created_utc = "2026-10-17T10:03:44Z";
    m.epoch_qpc = 123456789012;
    m.qpc_frequency = 10000000;
    m.duration_us = 185000000;
    m.width = 1920;
    m.height = 1080;
    m.region = RectI{0, 0, 1920, 1080};
    m.monitor_device = "\\\\.\\DISPLAY1";
    m.monitor_name = "DELL U2723QE";
    m.monitor_dpi = 144;
    m.has_camera = true;
    m.camera_width = 1280;
    m.camera_height = 720;
    m.camera_device = "Logitech BRIO";
    m.mic = {true, 1, "Microphone (Yeti)", 48000, 2};
    m.system_audio = {true, 2, "Speakers (Realtek)", 48000, 2};
    m.os = "10.0.26100";
    m.gpu = "AMD Radeon(TM) Graphics";
    m.encoder = "AMDh264Encoder (hardware)";
    m.pauses.push_back({60000000, 12000000});
    const Json j = BuildMetaJson(m);
    const char* keys[] = {"format", "version", "app_version", "created_utc", "epoch", "duration_us", "screen",
                          "camera", "audio", "events", "system", "pauses"};
    CT_REQUIRE(j.Items().size() == std::size(keys));
    for (size_t i = 0; i < std::size(keys); ++i) CT_CHECK_EQ(j.Items()[i].first, std::string(keys[i]));
    CT_CHECK_EQ(j["epoch"]["qpc"].AsInt64(), 123456789012LL);
    CT_CHECK_EQ(j["screen"]["monitor"]["dpi"].AsInt(), 144);
    CT_CHECK_EQ(j["screen"]["region"]["w"].AsInt(), 1920);
    CT_CHECK_EQ(j["camera"]["device"].AsString(), std::string("Logitech BRIO"));
    CT_CHECK_EQ(j["audio"]["system"]["stream_index"].AsInt(), 2);
    CT_CHECK_EQ(j["events"]["keystrokes"].AsString(), std::string("shortcuts"));
    CT_CHECK_EQ(j["pauses"][size_t{0}]["duration_us"].AsInt64(), 12000000LL);
    CT_CHECK(!j.Has("incomplete"));

    BundleMeta none;
    none.incomplete = true;
    const Json k = BuildMetaJson(none);
    CT_CHECK(k["camera"].IsNull() && k["audio"]["mic"].IsNull() && k["audio"]["system"].IsNull());
    CT_CHECK(k["incomplete"].AsBool());
    Json back;
    CT_CHECK(Json::Parse(j.Dump(2), back) && back == j);
}

CT_TEST_DEVICE(session_records_pauses_and_stops) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!BaseConfig(dev, cfg)) CT_SKIP("no D3D device / display");
    TestListener l;
    {
        RecordingSession s;
        CT_CHECK(s.GetState() == State::Idle);
        CT_REQUIRE(SUCCEEDED(s.Prepare(dev, cfg, &l)));
        CT_CHECK(s.IsPrepared());
        CT_CHECK(fs::IsDirectory(s.BundlePath()));
        s.StartCountdown();
        CT_CHECK(s.GetState() == State::Countdown);
        s.StartNow(Clock::QpcNow());
        WaitMediaUs(s, 800'000);
        CT_CHECK(s.GetState() == State::Recording);
        s.TogglePause();
        Sleep(700);
        CT_CHECK(s.GetState() == State::Paused);
        const TimeUs paused_at = s.Clock().NowMediaUs();
        Sleep(200);
        CT_CHECK_EQ(s.Clock().NowMediaUs(), paused_at);   // media time stands still
        s.TogglePause();
        s.AddMarker("test marker");
        WaitMediaUs(s, 1'600'000);
        s.Stop();
        CT_REQUIRE(s.WaitIdle(30'000));
        CT_CHECK_EQ(l.finished.load(), 1);
        CT_CHECK_EQ(l.errors.load(), 0);
        CT_CHECK(l.ticks.load() >= 3);
        CT_CHECK(s.LastStartLatencyMs() > 0 && s.LastStartLatencyMs() < 1000);
        const BundleReport rep = VerifyBundle(s.BundlePath(), BundleExpect{1.6, 0.25, false});
        std::printf("%s", rep.text.c_str());
        CT_CHECK(rep.ok);
        CT_CHECK_EQ(rep.fps, 30);
        Json meta;
        std::string text;
        CT_REQUIRE(fs::ReadAll(s.BundlePath() + "\\meta.json", text) && Json::Parse(text, meta));
        CT_REQUIRE(meta["pauses"].Size() == 1);
        CT_CHECK(meta["pauses"][size_t{0}]["duration_us"].AsInt64() >= 800'000);
        CT_CHECK_EQ(meta["screen"]["width"].AsInt(), 640);
    }
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(session_cancel_deletes_bundle) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!BaseConfig(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.events = false;
    TestListener l;
    RecordingSession s;
    // Cancel of a prepared, never started session.
    CT_REQUIRE(SUCCEEDED(s.Prepare(dev, cfg, &l)));
    const std::string first = s.BundlePath();
    s.Cancel();
    CT_REQUIRE(s.WaitIdle(10'000));
    CT_CHECK(!s.IsPrepared());
    CT_CHECK(!fs::Exists(first));
    // Cancel while recording (async prepare, start queued behind it).
    s.PrepareAsync(dev, cfg, &l);
    s.StartNow();
    WaitMediaUs(s, 500'000);
    CT_CHECK(s.GetState() == State::Recording);
    const std::string second = s.BundlePath();
    s.Cancel();
    CT_REQUIRE(s.WaitIdle(30'000));
    CT_CHECK(!fs::Exists(second));
    CT_CHECK_EQ(l.finished.load(), 0);
    CT_CHECK_EQ(l.prepared.load(), 2);
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(session_prepare_failure_reports_error) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!BaseConfig(dev, cfg)) CT_SKIP("no D3D device / display");
    fs::DeleteTree(cfg.output_dir);
    cfg.output_dir = "\\\\?\\Q:\\no such drive\\x";   // cannot be created
    TestListener l;
    RecordingSession s;
    CT_CHECK(FAILED(s.Prepare(dev, cfg, &l)));
    CT_CHECK_EQ(l.errors.load(), 1);
    CT_CHECK(!s.IsPrepared());
    s.StartNow();   // ignored
    CT_REQUIRE(s.WaitIdle(5'000));
    CT_CHECK(s.GetState() == State::Idle);
}
