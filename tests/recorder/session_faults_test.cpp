// RecordingSession error paths with recovery (P11 task 11.5), forced through SessionConfig::faults: a full disk
// (writer ERROR_DISK_FULL and the 200 MB auto-stop), a lost device (capture and writer DXGI_ERROR_DEVICE_REMOVED),
// a missing hardware encoder (software fallback) and no encoder at all, a busy microphone and an audio device that
// goes away mid-recording, and an output folder that cannot be created. Each kept bundle is verified as playable.
#include <ct/capture/display.h>
#include <ct/core/fs.h>
#include <ct/core/json.h>
#include <ct/core/sync.h>
#include <ct/core/time.h>
#include <ct/media/probe.h>
#include <ct/platform/d3d_device.h>
#include <ct/recorder/recording_session.h>
#include <ct/test/ct_test.h>

#include <audioclient.h>
#include <mferror.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::recorder;

namespace {

// Without a UI poster the callbacks run on ct-session; the test thread reads under the lock.
struct FaultListener final : SessionListener {
    mutable SrwLock lock;
    std::vector<SessionFailure> failures;
    std::vector<SessionWarning> warnings;
    std::atomic<int> finished{0}, errors{0};
    void OnStateChanged(State) override {}
    void OnCountdown(int) override {}
    void OnTick(const LiveStats&) override {}
    void OnFinished(const std::string&) override { ++finished; }
    void OnError(const std::string&, HRESULT) override { ++errors; }   // must not be reached: OnFailure is overridden
    void OnFailure(const SessionFailure& f) override {
        ExclusiveGuard g(lock);
        failures.push_back(f);
    }
    void OnWarningInfo(const SessionWarning& w) override {
        ExclusiveGuard g(lock);
        warnings.push_back(w);
    }
    size_t FailureCount() const {
        SharedGuard g(lock);
        return failures.size();
    }
    SessionFailure Failure() const {
        SharedGuard g(lock);
        return failures.empty() ? SessionFailure{} : failures.front();
    }
    bool Warned(WarningCause c) const {
        SharedGuard g(lock);
        for (const SessionWarning& w : warnings)
            if (w.cause == c) return true;
        return false;
    }
};

bool Setup(platform::D3DDevice& dev, SessionConfig& cfg) {
    if (FAILED(dev.Init(false))) return false;
    const auto displays = capture::EnumerateDisplays(dev.Factory());
    if (displays.empty()) return false;
    cfg.display = displays[0];
    cfg.region = RectI{cfg.display.rect.x, cfg.display.rect.y, 640, 360};
    cfg.fps = 30;
    cfg.mic = false;
    cfg.system_audio = true;
    cfg.events = false;   // no global hooks: these tests run next to other agents' input
    cfg.output_dir = fs::TempPath("");
    fs::CreateDirectories(cfg.output_dir);
    return true;
}

bool WaitForFailures(uint32_t ms, const FaultListener& l, size_t failures) {
    const int64_t end = Clock::QpcNow() + static_cast<int64_t>(ms) * Clock::QpcFrequency() / 1000;
    while (Clock::QpcNow() < end) {
        if (l.FailureCount() >= failures) return true;
        Sleep(20);
    }
    return false;
}

// Records until the injected failure stops the session (or `max_ms`), then returns the bundle path.
std::string RecordUntilFailure(platform::D3DDevice& dev, const SessionConfig& cfg, FaultListener& l, RecordingSession& s,
                               uint32_t max_ms = 20'000) {
    if (FAILED(s.Prepare(dev, cfg, &l))) return {};
    const std::string path = s.BundlePath();
    s.StartNow(Clock::QpcNow());
    WaitForFailures(max_ms, l, 1);
    s.WaitIdle(30'000);
    return path;
}

// "Playable": screen.mp4 opens with one video stream, has frames and a duration, and meta.json is complete.
// (Not VerifyBundle: these bundles have no events.ctev, and its 3 % frame-count tolerance is too strict for a Debug
// software encoder on a loaded machine.)
void CheckPlayable(const std::string& bundle) {
    media::ProbeResult pr;
    CT_REQUIRE(SUCCEEDED(media::Probe(bundle + "\\screen.mp4", pr)));
    CT_CHECK_EQ(pr.video_streams, 1);
    CT_CHECK(pr.duration_us > 0);
    media::StreamScan scan;
    CT_REQUIRE(SUCCEEDED(media::ScanStream(bundle + "\\screen.mp4", 0, scan)));
    CT_CHECK(scan.samples > 0);
    Json meta;
    std::string text;
    CT_REQUIRE(fs::ReadAll(bundle + "\\meta.json", text) && Json::Parse(text, meta));
    CT_CHECK(!meta.Has("incomplete"));
    CT_CHECK(meta["duration_us"].AsInt64() > 0);
}

} // namespace

CT_TEST_DEVICE(fault_disk_full_write_error_stops_and_keeps_file) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.faults.writer_error_hr = HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    cfg.faults.writer_error_after_samples = 40;   // ~1 s of video + system audio
    FaultListener l;
    RecordingSession s;
    const std::string bundle = RecordUntilFailure(dev, cfg, l, s);
    CT_REQUIRE(!bundle.empty());
    CT_REQUIRE(l.FailureCount() == 1);
    const SessionFailure f = l.Failure();
    CT_CHECK(f.cause == FailureCause::DiskFull);
    CT_CHECK(f.hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL));
    CT_CHECK(!f.during_prepare);
    CT_CHECK(f.saved);
    CT_CHECK(f.finalized);
    CT_CHECK_EQ(f.bundle_path, bundle);
    CT_CHECK_EQ(f.output_dir, cfg.output_dir);
    CT_CHECK_EQ(l.finished.load(), 1);   // the kept bundle is reported as finished too
    CT_CHECK_EQ(l.errors.load(), 0);
    CT_CHECK(s.GetState() == State::Idle);
    CheckPlayable(bundle);
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_low_disk_auto_stops_at_200mb) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.faults.free_disk_bytes = 150ull << 20;   // below kStopFreeBytes: the next 4 Hz tick stops the recording
    cfg.faults.free_disk_after_ms = 1000;
    FaultListener l;
    RecordingSession s;
    const std::string bundle = RecordUntilFailure(dev, cfg, l, s);
    CT_REQUIRE(l.FailureCount() == 1);
    const SessionFailure f = l.Failure();
    CT_CHECK(f.cause == FailureCause::DiskFull);
    CT_CHECK(f.saved);
    CheckPlayable(bundle);
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_low_disk_warns_and_continues) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.faults.free_disk_bytes = 600ull << 20;   // < 1 GB: warn once, keep recording
    FaultListener l;
    RecordingSession s;
    CT_REQUIRE(SUCCEEDED(s.Prepare(dev, cfg, &l)));
    s.StartNow(Clock::QpcNow());
    Sleep(1200);
    CT_CHECK(s.GetState() == State::Recording);
    s.Stop();
    CT_REQUIRE(s.WaitIdle(30'000));
    CT_CHECK(l.Warned(WarningCause::LowDisk));
    CT_CHECK_EQ(l.FailureCount(), size_t(0));
    CT_CHECK_EQ(l.finished.load(), 1);
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_device_lost_in_capture_finalizes_bundle) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.faults.capture_error_hr = DXGI_ERROR_DEVICE_REMOVED;   // what ScreenCapturer::OnError reports after a TDR
    cfg.faults.capture_error_after_ms = 900;
    FaultListener l;
    RecordingSession s;
    const std::string bundle = RecordUntilFailure(dev, cfg, l, s);
    CT_REQUIRE(l.FailureCount() == 1);
    const SessionFailure f = l.Failure();
    CT_CHECK(f.cause == FailureCause::DeviceLost);
    CT_CHECK(f.hr == DXGI_ERROR_DEVICE_REMOVED);
    CT_CHECK(f.saved && f.finalized);
    CheckPlayable(bundle);
    // The session is reusable afterwards (the app recreates its device first; this one was never really lost).
    SessionConfig again = cfg;
    again.faults = SessionFaults{};
    CT_CHECK(SUCCEEDED(s.Prepare(dev, again, &l)));
    s.Cancel();
    CT_CHECK(s.WaitIdle(10'000));
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_device_lost_in_writer_is_device_lost) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.faults.writer_error_hr = DXGI_ERROR_DEVICE_REMOVED;
    cfg.faults.writer_error_after_samples = 40;
    FaultListener l;
    RecordingSession s;
    const std::string bundle = RecordUntilFailure(dev, cfg, l, s);
    CT_REQUIRE(l.FailureCount() == 1);
    CT_CHECK(l.Failure().cause == FailureCause::DeviceLost);
    CT_CHECK(l.Failure().saved);
    // Whatever the writer managed to finalize, meta.json tells the truth about it.
    Json meta;
    std::string text;
    CT_REQUIRE(fs::ReadAll(bundle + "\\meta.json", text) && Json::Parse(text, meta));
    CT_CHECK_EQ(meta.Has("incomplete"), !l.Failure().finalized);
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_no_hardware_encoder_falls_back_to_software) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.faults.hw_encoder_init_hr = MF_E_TOPO_CODEC_NOT_FOUND;
    FaultListener l;
    RecordingSession s;
    CT_REQUIRE(SUCCEEDED(s.Prepare(dev, cfg, &l)));
    CT_CHECK(!s.Stats().hardware_encoder);
    CT_CHECK(l.Warned(WarningCause::SoftwareEncoder));
    s.StartNow(Clock::QpcNow());
    Sleep(1200);
    s.Stop();
    CT_REQUIRE(s.WaitIdle(30'000));
    CT_CHECK_EQ(l.FailureCount(), size_t(0));
    CT_CHECK_EQ(l.finished.load(), 1);
    CheckPlayable(s.BundlePath());
    Json meta;
    std::string text;
    CT_REQUIRE(fs::ReadAll(s.BundlePath() + "\\meta.json", text) && Json::Parse(text, meta));
    CT_CHECK(meta["system"]["encoder"].AsString().ends_with("(software)"));
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_no_encoder_at_all_fails_prepare_clearly) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.faults.encoder_init_hr = MF_E_TOPO_CODEC_NOT_FOUND;
    FaultListener l;
    RecordingSession s;
    CT_CHECK(FAILED(s.Prepare(dev, cfg, &l)));
    CT_REQUIRE(l.FailureCount() == 1);
    const SessionFailure f = l.Failure();
    CT_CHECK(f.cause == FailureCause::NoEncoder);
    CT_CHECK(f.during_prepare);
    CT_CHECK(!f.saved);
    CT_CHECK(f.hr == MF_E_TOPO_CODEC_NOT_FOUND);
    CT_CHECK(!f.bundle_path.empty() && !fs::Exists(f.bundle_path));   // nothing left behind
    CT_CHECK(!s.IsPrepared());
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_mic_busy_records_without_it) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.mic = true;
    cfg.faults.mic_open_hr = AUDCLNT_E_DEVICE_IN_USE;   // another app holds the microphone in exclusive mode
    FaultListener l;
    RecordingSession s;
    CT_REQUIRE(SUCCEEDED(s.Prepare(dev, cfg, &l)));
    CT_CHECK(l.Warned(WarningCause::MicBusy));
    CT_CHECK(!s.Stats().has_mic);
    s.StartNow(Clock::QpcNow());
    Sleep(1000);
    s.Stop();
    CT_REQUIRE(s.WaitIdle(30'000));
    CT_CHECK_EQ(l.FailureCount(), size_t(0));
    CheckPlayable(s.BundlePath());
    Json meta;
    std::string text;
    CT_REQUIRE(fs::ReadAll(s.BundlePath() + "\\meta.json", text) && Json::Parse(text, meta));
    CT_CHECK(meta["audio"]["mic"].IsNull());
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_audio_device_lost_keeps_recording) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    cfg.mic = true;   // the mic stream when this machine has one, else system audio
    cfg.faults.audio_error_hr = AUDCLNT_E_DEVICE_INVALIDATED;   // unplugged
    cfg.faults.audio_error_after_ms = 400;
    FaultListener l;
    RecordingSession s;
    CT_REQUIRE(SUCCEEDED(s.Prepare(dev, cfg, &l)));
    const bool has_mic = s.Stats().has_mic;
    s.StartNow(Clock::QpcNow());
    Sleep(1200);
    CT_CHECK(s.GetState() == State::Recording);
    s.Stop();
    CT_REQUIRE(s.WaitIdle(30'000));
    CT_CHECK(l.Warned(has_mic ? WarningCause::AudioDeviceLost : WarningCause::SystemAudio));
    CT_CHECK_EQ(l.FailureCount(), size_t(0));
    CT_CHECK_EQ(l.finished.load(), 1);
    fs::DeleteTree(cfg.output_dir);
}

CT_TEST_DEVICE(fault_output_folder_unusable_is_output_folder) {
    platform::D3DDevice dev;
    SessionConfig cfg;
    if (!Setup(dev, cfg)) CT_SKIP("no D3D device / display");
    fs::DeleteTree(cfg.output_dir);
    cfg.output_dir = "\\\\?\\Q:\\no such drive\\x";
    FaultListener l;
    RecordingSession s;
    CT_CHECK(FAILED(s.Prepare(dev, cfg, &l)));
    CT_REQUIRE(l.FailureCount() == 1);
    CT_CHECK(l.Failure().cause == FailureCause::OutputFolder);
    CT_CHECK(l.Failure().during_prepare);
    CT_CHECK_EQ(l.Failure().output_dir, cfg.output_dir);
}
