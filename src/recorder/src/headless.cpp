#define CT_LOG_CATEGORY "headless"
#include <ct/recorder/headless.h>

#include <ct/capture/display.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/json.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/events/event_log.h>
#include <ct/media/probe.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>
#include <ct/recorder/recording_session.h>

#include <psapi.h>
#include <shellapi.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

namespace ct::recorder {

namespace {

struct Args {
    bool record = false, selftest = false, verify = false;
    int monitor = 0, fps = 60, quality = 70;
    std::string out;
    double seconds = 0;   // 0 = until Ctrl+C
    bool mic = true, system = true, camera = false, synthetic_camera = false;
    RectI region;
    media::VideoCodec codec = media::VideoCodec::H264;
    KeystrokeMode keys = KeystrokeMode::ShortcutsOnly;
    double pause_at = -1, pause_for = 0;
};

Args Parse(const wchar_t* cmdline) {
    Args a;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmdline, &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const wchar_t* s = argv[i];
        const bool has_next = i + 1 < argc;
        if (!wcscmp(s, L"--record")) a.record = true;
        else if (!wcscmp(s, L"--selftest")) a.selftest = true;
        else if (!wcscmp(s, L"--verify")) a.verify = true;
        else if (!wcscmp(s, L"--monitor") && has_next) a.monitor = _wtoi(argv[++i]);
        else if (!wcscmp(s, L"--fps") && has_next) a.fps = _wtoi(argv[++i]);
        else if (!wcscmp(s, L"--quality") && has_next) a.quality = _wtoi(argv[++i]);
        else if (!wcscmp(s, L"--out") && has_next) a.out = WideToUtf8(argv[++i]);
        else if (!wcscmp(s, L"--seconds") && has_next) a.seconds = _wtof(argv[++i]);
        else if (!wcscmp(s, L"--no-mic")) a.mic = false;
        else if (!wcscmp(s, L"--no-system")) a.system = false;
        else if (!wcscmp(s, L"--camera")) a.camera = true;
        else if (!wcscmp(s, L"--synthetic-camera")) a.camera = a.synthetic_camera = true;
        else if (!wcscmp(s, L"--codec") && has_next) a.codec = !_wcsicmp(argv[++i], L"hevc") ? media::VideoCodec::Hevc : media::VideoCodec::H264;
        else if (!wcscmp(s, L"--keys") && has_next) {
            const wchar_t* k = argv[++i];
            a.keys = !_wcsicmp(k, L"off") ? KeystrokeMode::Off : !_wcsicmp(k, L"all") ? KeystrokeMode::All : KeystrokeMode::ShortcutsOnly;
        } else if (!wcscmp(s, L"--region") && has_next) {
            int x = 0, y = 0, w = 0, h = 0;
            if (swscanf_s(argv[++i], L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) a.region = RectI{x, y, w, h};
        } else if (!wcscmp(s, L"--pause-at") && has_next) a.pause_at = _wtof(argv[++i]);
        else if (!wcscmp(s, L"--pause-for") && has_next) a.pause_for = _wtof(argv[++i]);
    }
    if (argv) LocalFree(argv);
    return a;
}

Event& StopEvent() {
    static Event e(true, false);
    return e;
}

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        StopEvent().Set();
        return TRUE;
    }
    return FALSE;
}

struct ConsoleListener final : SessionListener {
    std::atomic<bool> failed{false};
    std::atomic<bool> finished{false};
    std::string bundle;   // written once on ct-session before `finished`
    void OnStateChanged(State s) override { CT_LOG_INFO("headless: state %s", StateName(s)); }
    void OnCountdown(int) override {}
    void OnTick(const LiveStats&) override {}
    void OnFinished(const std::string& path) override {
        bundle = path;
        finished.store(true);
    }
    void OnError(const std::string& message, HRESULT hr) override {
        std::printf("error: %s (%s)\n", message.c_str(), HrToString(hr).c_str());
        failed.store(true);
    }
    void OnWarning(const std::string& message) override { std::printf("warning: %s\n", message.c_str()); }
};

double ProcessCpuMs() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    const auto v = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(v(k) + v(u)) / 10'000.0;
}

int Record(const Args& a, bool selftest) {
    platform::D3DDevice dev;
    HRESULT hr = dev.Init(false);
    if (FAILED(hr)) {
        std::printf("no D3D11 device: %s\n", HrToString(hr).c_str());
        return 1;
    }
    const std::vector<capture::DisplayInfo> displays = capture::EnumerateDisplays(dev.Factory());
    if (displays.empty() || a.monitor < 0 || a.monitor >= static_cast<int>(displays.size())) {
        std::printf("monitor %d not found (%zu displays)\n", a.monitor, displays.size());
        return 1;
    }
    SessionConfig cfg;
    cfg.display = displays[static_cast<size_t>(a.monitor)];
    cfg.region = a.region;
    cfg.fps = a.fps;
    cfg.codec = a.codec;
    cfg.quality = a.quality;
    cfg.mic = a.mic;
    cfg.system_audio = a.system;
    cfg.webcam = a.camera;
    cfg.webcam_synthetic = a.synthetic_camera;
    cfg.output_dir = a.out;
    cfg.keystrokes = a.keys;
    cfg.countdown_s = 0;

    ConsoleListener listener;
    RecordingSession session;
    const double cpu0 = ProcessCpuMs();
    Stopwatch prep;
    prep.Start();
    hr = session.Prepare(dev, cfg, &listener);
    if (FAILED(hr)) return 1;
    const double prepare_ms = prep.ElapsedMs();
    const LiveStats st0 = session.Stats();
    std::printf("bundle: %s\nencoder: %s (%s), %d fps, mic %s, system %s, camera %s; prepare %.0f ms\n",
                session.BundlePath().c_str(), st0.encoder_name.c_str(), st0.hardware_encoder ? "hardware" : "software",
                st0.fps, st0.has_mic ? "on" : "off", st0.has_system ? "on" : "off", st0.has_camera ? "on" : "off", prepare_ms);
    SetConsoleCtrlHandler(&CtrlHandler, TRUE);
    Stopwatch wall;
    wall.Start();
    const double cpu_start = ProcessCpuMs();
    session.StartNow(Clock::QpcNow());

    bool paused = false, pause_done = false;
    int64_t pause_began_qpc = 0;
    while (!StopEvent().Wait(20)) {
        const State s = session.GetState();
        if (listener.failed.load() || s == State::Idle) break;
        const double media_s = session.Clock().NowMediaUs() / 1e6;
        if (s == State::Recording && !pause_done && !paused && a.pause_at >= 0 && media_s >= a.pause_at) {
            session.TogglePause();
            paused = true;
            pause_began_qpc = Clock::QpcNow();
        }
        if (paused && (Clock::QpcNow() - pause_began_qpc) >= static_cast<int64_t>(a.pause_for * Clock::QpcFrequency())) {
            session.TogglePause();
            paused = false;
            pause_done = true;
        }
        if (a.seconds > 0 && !paused && media_s >= a.seconds) break;
    }
    const double wall_s = wall.ElapsedMs() / 1000.0;
    const double cpu_rec = ProcessCpuMs() - cpu_start;
    const LiveStats st = session.Stats();
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    session.Stop();
    if (!session.WaitIdle(60'000)) {
        std::printf("finalizing timed out\n");
        return 1;
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::printf("recorded %.3f s wall: %llu frames, %llu dropped, first frame %.1f ms after start, file %.1f MB\n",
                wall_s, static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.dropped_frames),
                session.LastStartLatencyMs(), st.file_bytes / 1e6);
    std::printf("process CPU while recording: %.2f %% of the machine (%u logical CPUs), %.1f %% of one core; RSS %.0f MB, peak %.0f MB\n",
                wall_s > 0 ? cpu_rec / (wall_s * 10.0 * si.dwNumberOfProcessors) : 0.0, si.dwNumberOfProcessors,
                wall_s > 0 ? cpu_rec / (wall_s * 10.0) : 0.0, pmc.WorkingSetSize / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0);
    (void)cpu0;
    const std::string bundle = session.BundlePath();
    if (listener.failed.load() && !listener.finished.load()) return 1;

    if (a.verify || selftest) {
        BundleExpect ex;
        ex.seconds = a.seconds;
        const BundleReport rep = VerifyBundle(bundle, ex);
        std::printf("%s", rep.text.c_str());
        std::printf("verify: %s\n", rep.ok ? "PASS" : "FAIL");
        if (!rep.ok) return 1;
        if (selftest) fs::DeleteTree(bundle);
    }
    return listener.failed.load() ? 1 : 0;
}

void Check(BundleReport& r, bool cond, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    r.text += cond ? "  ok    " : "  FAIL  ";
    r.text += buf;
    r.text += '\n';
    if (!cond) r.ok = false;
}

std::string Join(std::string_view dir, const std::string& name) {
    std::string s(dir);
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s += '\\';
    return s + name;
}

} // namespace

bool IsHeadlessCommandLine(const wchar_t* cmdline) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(cmdline, &argc);
    bool yes = false;
    for (int i = 1; argv && i < argc; ++i)
        if (!wcscmp(argv[i], L"--record") || !wcscmp(argv[i], L"--selftest")) yes = true;
    if (argv) LocalFree(argv);
    return yes;
}

int RunHeadless(const wchar_t* cmdline) {
    Args a = Parse(cmdline);
    int rc = 1;
    Thread t;
    Thread::Options opt;
    opt.name = "ct-headless";
    opt.com_mta = true;
    t.Start(opt, [&] {
        if (a.selftest) {
            Args s = a;
            s.monitor = 0;
            s.fps = 60;
            s.seconds = 3.0;
            s.camera = s.synthetic_camera = true;
            std::string tmp = fs::TempPath("");
            fs::CreateDirectories(tmp);
            s.out = tmp;
            std::printf("selftest: 3 s of monitor 0 into %s\n", tmp.c_str());
            rc = Record(s, true);
            if (rc == 0) fs::DeleteTree(tmp);
            std::printf("selftest: %s\n", rc == 0 ? "PASS" : "FAIL");
        } else {
            rc = Record(a, false);
        }
    });
    t.Join();
    return rc;
}

BundleReport VerifyBundle(std::string_view bundle_path, const BundleExpect& expect) {
    BundleReport r;
    r.ok = true;
    std::string text;
    Json meta;
    std::string perr;
    const bool read = fs::ReadAll(Join(bundle_path, "meta.json"), text);
    Check(r, read, "meta.json exists");
    if (!read) return r;
    const bool parsed = Json::Parse(text, meta, &perr);
    Check(r, parsed, "meta.json parses%s%s", parsed ? "" : ": ", perr.c_str());
    if (!parsed) return r;
    Check(r, meta["format"].AsString() == "ctrec" && meta["version"].AsInt() == 1, "format ctrec, version 1");
    Check(r, meta["epoch"]["qpc"].AsInt64() != 0 && meta["epoch"]["qpc_frequency"].AsInt64() > 0, "epoch qpc + frequency");
    Check(r, !meta.Has("incomplete"), "complete (no \"incomplete\" flag)");
    const Json& screen = meta["screen"];
    r.duration_s = meta["duration_us"].AsInt64() / 1e6;
    r.fps = screen["fps_num"].AsInt();
    if (expect.seconds > 0)
        Check(r, std::fabs(r.duration_s - expect.seconds) <= expect.tolerance_s, "duration %.3f s (expected %.1f +- %.2f)",
              r.duration_s, expect.seconds, expect.tolerance_s);
    else
        Check(r, r.duration_s > 0, "duration %.3f s", r.duration_s);
    const Json& region = screen["region"];
    Check(r, region["w"].AsInt() == screen["width"].AsInt() && region["h"].AsInt() == screen["height"].AsInt() &&
                 screen["width"].AsInt() > 0 && screen["scale"].AsNumber() == 1.0,
          "screen %dx%d = region, scale 1", screen["width"].AsInt(), screen["height"].AsInt());
    Check(r, meta["camera"].IsNull() || meta["camera"].IsObject(), "camera object or null");
    Check(r, meta["audio"].IsObject() && meta["events"].IsObject() && meta["system"].IsObject() && meta["pauses"].IsArray(),
          "audio/events/system/pauses present");
    int audio_expected = 0;
    for (const char* k : {"mic", "system"}) {
        const Json& a = meta["audio"][k];
        if (a.IsObject()) {
            ++audio_expected;
            Check(r, a["stream_index"].AsInt() >= 1 && a["sample_rate"].AsInt() == 48000 && a["channels"].AsInt() == 2,
                  "audio.%s stream %d, 48000 Hz stereo, \"%s\"", k, a["stream_index"].AsInt(), a["device"].AsString().c_str());
        }
    }

    const std::string screen_path = Join(bundle_path, screen["file"].AsString("screen.mp4"));
    media::ProbeResult pr;
    HRESULT hr = media::Probe(screen_path, pr);
    Check(r, SUCCEEDED(hr), "screen.mp4 probes (%s)", HrToString(hr).c_str());
    if (SUCCEEDED(hr)) {
        r.audio_streams = pr.audio_streams;
        Check(r, pr.video_streams == 1 && pr.video_codec == screen["codec"].AsString(), "1 video stream, codec %s",
              pr.video_codec.c_str());
        Check(r, pr.video_width == screen["width"].AsInt() && pr.video_height == screen["height"].AsInt(),
              "video %dx%d", pr.video_width, pr.video_height);
        Check(r, pr.audio_streams == audio_expected, "%d audio stream(s) (meta lists %d)", pr.audio_streams, audio_expected);
        media::StreamScan scan;
        hr = media::ScanStream(screen_path, 0, scan);
        r.video_frames = scan.samples;
        const double expected_frames = r.duration_s * r.fps;
        Check(r, SUCCEEDED(hr) && std::fabs(static_cast<double>(scan.samples) - expected_frames) <= std::max(2.0, expected_frames * expect.max_drop_fraction),
              "%llu video frames (fps x duration = %.0f), max gap %.1f ms", static_cast<unsigned long long>(scan.samples),
              expected_frames, scan.max_gap_us / 1000.0);
        // The MP4 source's MF_MT_FRAME_RATE counts the first frame's offset from 0, so check the sample spacing instead.
        const double spacing_us = scan.samples > 1 ? static_cast<double>(scan.last_us - scan.first_us) / (scan.samples - 1) : 0;
        // Allow the same few dropped frames the count check above accepts (a loaded machine can skip a tick or two);
        // each one lengthens the mean spacing by 1 / (samples - 1).
        const double allowed_drops = std::max(2.0, expected_frames * expect.max_drop_fraction) + 1.0;
        const double spacing_tol = scan.samples > 1 ? std::max(0.03, allowed_drops / static_cast<double>(scan.samples - 1)) : 0.03;
        Check(r, r.fps > 0 && std::fabs(spacing_us * r.fps / 1e6 - 1.0) < spacing_tol, "constant %d fps (mean spacing %.2f ms; MF reports %d/%d)",
              r.fps, spacing_us / 1000.0, pr.fps_num, pr.fps_den);
    }
    if (meta["camera"].IsObject()) {
        const std::string cam_path = Join(bundle_path, meta["camera"]["file"].AsString("cam.mp4"));
        media::ProbeResult cp;
        hr = media::Probe(cam_path, cp);
        media::StreamScan cs;
        if (SUCCEEDED(hr)) hr = media::ScanStream(cam_path, 0, cs);
        r.cam_frames = cs.samples;
        Check(r, SUCCEEDED(hr) && cp.video_streams == 1 && cp.video_width == meta["camera"]["width"].AsInt() && cs.samples > 0,
              "cam.mp4 %dx%d, %llu frames", cp.video_width, cp.video_height, static_cast<unsigned long long>(cs.samples));
    }

    events::EventLog log;
    Err err;
    const bool loaded = log.Load(Join(bundle_path, meta["events"]["file"].AsString("events.ctev")), &err);
    Check(r, loaded, "events.ctev loads%s%s", loaded ? "" : ": ", err.message.c_str());
    if (loaded) {
        r.cursor_shapes = log.Shapes().size();
        r.cursor_samples = log.Cursor().size();
        Check(r, !log.Truncated(), "events.ctev not truncated");
        Check(r, log.RegionSize().w == screen["width"].AsInt() && log.RegionSize().h == screen["height"].AsInt(),
              "events region %dx%d", log.RegionSize().w, log.RegionSize().h);
        Check(r, !expect.require_cursor || r.cursor_shapes >= 1, "%zu cursor shape(s), %zu cursor samples, %zu focus, %zu pauses",
              r.cursor_shapes, r.cursor_samples, log.Focus().size(), log.Pauses().size());
    }
    return r;
}

} // namespace ct::recorder
