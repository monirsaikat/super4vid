#define CT_LOG_CATEGORY "session"
#include <ct/recorder/recording_session.h>

#include <ct/audio/devices.h>
#include <ct/audio/wasapi_capture.h>
#include <ct/audio/wasapi_loopback.h>
#include <ct/capture/screen_capturer.h>
#include <ct/capture/webcam_source.h>
#include <ct/core/assert.h>
#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/stats.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/media/encoder_caps.h>
#include <ct/media/mp4_writer.h>
#include <ct/platform/crash_handler.h>
#include <ct/platform/paths.h>
#include <ct/recorder/bundle_writer.h>
#include <ct/version.h>

#include <powrprof.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace ct::recorder {

const char* StateName(State s) {
    switch (s) {
    case State::Idle: return "Idle";
    case State::Countdown: return "Countdown";
    case State::Starting: return "Starting";
    case State::Recording: return "Recording";
    case State::Paused: return "Paused";
    case State::Finalizing: return "Finalizing";
    case State::Error: return "Error";
    }
    return "?";
}

namespace {

constexpr int64_t kTickMs = 250;   // LiveStats at 4 Hz
constexpr int64_t kMeterMs = 33;   // live meter levels at 30 Hz

// SessionClock as the webcam's time base (the capture module cannot depend on the recorder).
struct ClockTimeSource final : capture::MediaTimeSource {
    const SessionClock* clock = nullptr;
    TimeUs QpcHnsToMediaUs(uint64_t qpc_hns) const override { return clock->HnsToMediaUs(qpc_hns); }
    TimeUs NowMediaUs() const override { return clock->NowMediaUs(); }
};

bool IsFileError(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
           hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) ||
           hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

float PeakDb(const audio::Meter& m) {
    float peak = audio::Meter::kFloorDb;
    const int n = std::max(1, m.Channels());
    for (int c = 0; c < n && c < audio::Meter::kMaxChannels; ++c) peak = std::max(peak, m.PeakDb(c));
    return peak;
}

const char* KeystrokeName(KeystrokeMode k) {
    switch (k) {
    case KeystrokeMode::Off: return "off";
    case KeystrokeMode::All: return "all";
    default: return "shortcuts";
    }
}

// RegisterSuspendResumeNotification callback (a system thread): forwards to the session.
ULONG CALLBACK PowerCallback(PVOID context, ULONG type, PVOID) {
    auto* s = static_cast<RecordingSession*>(context);
    if (type == PBT_APMSUSPEND) s->OnSystemSuspend();
    else if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) s->OnSystemResume();
    return 0;
}

std::atomic<RecordingSession*> g_armed{nullptr};
std::atomic<bool> g_emergency_registered{false};

void EmergencyThunk(void*) {
    RecordingSession* s = g_armed.load(std::memory_order_acquire);
    if (s) s->EmergencyFinalize();
}

} // namespace

struct RecordingSession::Impl {
    RecordingSession* owner = nullptr;
    SessionClock* clock = nullptr;

    // --- command queue (any thread -> ct-session) ---
    enum class Cmd { Prepare, StartNow, Pause, Marker, Stop, Cancel, Disarm, Suspend, Resume, Quit };
    struct Command {
        Cmd kind;
        int64_t qpc = 0;
        std::string label;
        platform::D3DDevice* dev = nullptr;
        SessionConfig cfg;
        SessionListener* listener = nullptr;
        bool arm = false;   // Prepare: arm (RecordingSession::Arm)
        uint32_t arm_gen = 0;
        HRESULT* result = nullptr;
        Event* done = nullptr;
    };
    SrwLock queue_lock;
    std::vector<Command> queue;
    Event wake{false, false};
    Thread thread;
    std::atomic<int> pending{0};   // queued + executing commands

    // --- shared state ---
    std::atomic<State> state{State::Idle};
    std::atomic<bool> prepared{false};
    // Arm requests: arm_state is the UI's view (1 = the latest request is an Arm with arm_cfg, not yet started or
    // released); arm_gen counts Arm/Disarm/Cancel calls, so ct-session builds only the latest Arm.
    std::atomic<int> arm_state{0};
    std::atomic<uint32_t> arm_gen{0};
    mutable SrwLock arm_lock;
    SessionConfig arm_cfg;
    std::atomic<bool> preparing{false};
    std::atomic<bool> quiet{false};   // arming (ct-session): Warn only logs
    std::atomic<int> countdown_s{3};
    mutable SrwLock stats_lock;
    LiveStats stats;
    std::string bundle_path;   // under stats_lock
    std::atomic<double> last_latency_ms{-1.0};
    std::atomic<float> live_mic_db{audio::Meter::kFloorDb}, live_sys_db{audio::Meter::kFloorDb};
    SessionListener* listener = nullptr;

    // Component errors (ct-capture, ct-encode, ...): first one wins, handled on ct-session.
    SrwLock error_lock;
    std::atomic<bool> error_pending{false};
    HRESULT error_hr = S_OK;
    std::string error_message;

    // --- owned by ct-session ---
    platform::D3DDevice* dev = nullptr;
    SessionConfig cfg;
    bool devices_ready = false;
    // Armed (ct-session): the screen part is built; audio streams declared by device presence, opened at StartNow.
    bool armed = false;
    bool arm_mic = false, arm_sys = false;
    bool arm_failed = false;   // the last arm failed; dev/cfg/listener still describe it
    std::string rename_to;   // a started armed bundle: the stem it gets when the recording finishes
    audio::AudioDevices devices;
    BundleWriter bundle;
    std::unique_ptr<capture::ScreenCapturer> capturer;
    std::unique_ptr<media::Mp4Writer> writer;
    std::unique_ptr<capture::WebcamSource> webcam;
    std::unique_ptr<media::Mp4Writer> cam_writer;
    std::unique_ptr<audio::WasapiCapture> mic;
    std::unique_ptr<audio::WasapiLoopback> sys;
    std::unique_ptr<EventRecorder> events;
    ClockTimeSource clock_source;
    int fps = 60;
    media::VideoCodec codec = media::VideoCodec::H264;
    uint32_t mic_stream = 0, sys_stream = 0;
    int64_t trigger_qpc = 0;
    int64_t start_qpc = 0;
    int64_t next_tick_qpc = 0;
    bool latency_logged = false;
    bool disk_warned = false;
    std::string output_dir;
    // Emergency meta.json, prebuilt at StartNow so the crash filter only writes bytes.
    std::string emergency_meta;
    std::wstring emergency_meta_path;
    std::atomic<bool> emergency_done{false};
    // Sleep / resume (docs/03 section 10).
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS power_params{};
    HPOWERNOTIFY power_notify = nullptr;
    bool paused_by_suspend = false;
    Event suspend_done{true, false};   // set when a Suspend command ran (heap-free and outlives a timed-out wait)
    bool access_warned = false;   // the current access-lost streak was reported

    // --- forwarding sinks (component threads) ---
    struct ScreenSink final : capture::FrameSink {
        Impl* s = nullptr;
        std::atomic<int64_t> first_frame_qpc{0};
        bool OnFrame(ID3D11Texture2D* tex, uint32_t index, TimeUs t_us) override {
            if (first_frame_qpc.load(std::memory_order_relaxed) == 0)
                first_frame_qpc.store(Clock::QpcNow(), std::memory_order_relaxed);
            return s->writer->OnFrame(tex, index, t_us);
        }
        void OnError(HRESULT hr) override {
            if (hr == capture::kDisplayRemovedHr)
                s->Fail(hr, "The display being recorded was disconnected. The recording was saved up to that point.");
            else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
                     hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
                s->Fail(hr, "Screen capture stopped (the graphics device was lost).");
            else
                s->Fail(hr, "Screen capture stopped (" + HrToString(hr) + ").");
        }
    } screen_sink;
    struct CamSink final : capture::FrameSink {
        Impl* s = nullptr;
        bool OnFrame(ID3D11Texture2D* tex, uint32_t index, TimeUs t_us) override {
            return s->cam_writer->OnFrame(tex, index, t_us);
        }
        void OnError(HRESULT hr) override { s->Warn("The camera stopped; the screen keeps recording.", hr); }
    } cam_sink;
    struct AudioFwd final : audio::AudioSink {
        Impl* s = nullptr;
        std::atomic<bool> reported{false};
        bool OnAudio(uint32_t stream, const audio::AudioPacket& p) override { return s->writer->OnAudio(stream, p); }
        void OnAudioError(uint32_t, HRESULT hr) override {
            if (!reported.exchange(true)) s->Warn("An audio device stopped; recording silence until it is back.", hr);
        }
    } audio_fwd;
    struct WriterListener final : media::Mp4Writer::Listener {
        Impl* s = nullptr;
        bool cam = false;
        void OnWriterError(HRESULT hr) override {
            media::Mp4Writer* w = cam ? s->cam_writer.get() : s->writer.get();
            const std::string msg = w ? w->ErrorMessage() : std::string("Writing the recording failed.");
            if (cam) s->Warn("Writing the camera file failed: " + msg, hr);
            else s->Fail(hr, msg);
        }
    } writer_listener, cam_writer_listener;

    Impl() {
        screen_sink.s = this;
        cam_sink.s = this;
        audio_fwd.s = this;
        writer_listener.s = this;
        cam_writer_listener.s = this;
        cam_writer_listener.cam = true;
    }

    // ---------------------------------------------------------------------------------------------------------
    void Post(Command c) {
        pending.fetch_add(1, std::memory_order_acq_rel);
        {
            ExclusiveGuard g(queue_lock);
            queue.push_back(std::move(c));
        }
        wake.Set();
    }

    void Notify(std::function<void(SessionListener*)> fn) {
        SessionListener* l = listener;
        if (!l) return;
        Async::PostToUi([l, fn = std::move(fn)]() { fn(l); });
    }

    void SetState(State s) {
        const State old = state.exchange(s, std::memory_order_acq_rel);
        if (old == s) return;
        CT_LOG_INFO("state %s -> %s", StateName(old), StateName(s));
        {
            ExclusiveGuard g(stats_lock);
            stats.state = s;
        }
        Notify([s](SessionListener* l) { l->OnStateChanged(s); });
    }

    void Fail(HRESULT hr, std::string message) {
        {
            ExclusiveGuard g(error_lock);
            if (error_pending.load()) return;
            error_hr = hr;
            error_message = std::move(message);
            error_pending.store(true, std::memory_order_release);
        }
        wake.Set();
    }

    void Warn(std::string message, HRESULT hr) {
        CT_LOG_WARN("%s (%s)", message.c_str(), HrToString(hr).c_str());
        if (quiet) return;   // arming: nobody asked yet
        Notify([message = std::move(message)](SessionListener* l) { l->OnWarning(message); });
    }

    // ---------------------------------------------------------------------------------------------------------
    void Run() {
        while (true) {
            DWORD timeout = INFINITE;
            const State st = state.load();
            if (st == State::Recording || st == State::Paused) {
                const int64_t now = Clock::QpcNow();
                const int64_t left_ms = (next_tick_qpc - now) * 1000 / Clock::QpcFrequency();
                timeout = static_cast<DWORD>(std::clamp<int64_t>(left_ms, 0, kMeterMs));   // meters at 30 Hz
            }
            WaitForSingleObject(wake.Handle(), timeout);

            if (error_pending.load(std::memory_order_acquire)) HandleError();

            std::vector<Command> cmds;
            {
                ExclusiveGuard g(queue_lock);
                cmds.swap(queue);
            }
            bool quit = false;
            for (Command& c : cmds) {
                if (c.kind == Cmd::Quit) quit = true;
                else Execute(c);
                pending.fetch_sub(1, std::memory_order_acq_rel);
            }
            if (quit) break;

            const State now_state = state.load();
            if (now_state == State::Recording || now_state == State::Paused) {
                live_mic_db.store(mic ? PeakDb(mic->Levels()) : audio::Meter::kFloorDb, std::memory_order_relaxed);
                live_sys_db.store(sys ? PeakDb(sys->Levels()) : audio::Meter::kFloorDb, std::memory_order_relaxed);
            }
            if ((now_state == State::Recording || now_state == State::Paused) && Clock::QpcNow() >= next_tick_qpc) {
                next_tick_qpc = Clock::QpcNow() + Clock::QpcFrequency() * kTickMs / 1000;
                Tick();
            }
        }
        // Destructor path: release whatever is still alive.
        if (state.load() == State::Recording || state.load() == State::Paused) Finish(/*keep=*/true, /*notify=*/false);
        Release();
    }

    void Execute(Command& c) {
        switch (c.kind) {
        case Cmd::Prepare: {
            if (c.arm) {
                // Quiet: the user did not ask for anything yet. A real start re-prepares and reports problems.
                if (c.arm_gen != arm_gen.load()) break;   // disarmed or re-armed while queued
                DoDisarm();                               // an older arm
                const HRESULT hr = DoPrepare(*c.dev, c.cfg, c.listener, true);
                arm_failed = FAILED(hr);   // a StartNow that relied on it prepares normally (and reports why)
                break;
            }
            const HRESULT hr = DoPrepare(*c.dev, c.cfg, c.listener, false);
            preparing.store(false, std::memory_order_release);
            if (c.result) *c.result = hr;
            if (c.done) c.done->Set();
            Notify([hr](SessionListener* l) { l->OnPrepared(hr); });
            break;
        }
        case Cmd::StartNow: DoStartNow(c.qpc); break;
        case Cmd::Pause: DoTogglePause(c.qpc); break;
        case Cmd::Marker:
            if (events && (state.load() == State::Recording)) events->Marker(c.label);
            break;
        case Cmd::Stop: {
            const State s = state.load();
            if (s == State::Recording || s == State::Paused) Finish(true, true);
            else if (prepared.load()) DoCancel();
            break;
        }
        case Cmd::Cancel: DoCancel(); break;
        case Cmd::Disarm: DoDisarm(); break;
        case Cmd::Suspend:
            if (state.load() == State::Recording) {
                CT_LOG_INFO("system suspend: pausing the recording");
                DoTogglePause(Clock::QpcNow());
                paused_by_suspend = true;
            }
            suspend_done.Set();
            break;
        case Cmd::Resume:
            if (paused_by_suspend && state.load() == State::Paused) {
                CT_LOG_INFO("system resume: the recording stays paused");
                Warn("The computer went to sleep, so the recording was paused. Resume it when you are ready.", S_OK);
            }
            paused_by_suspend = false;
            break;
        case Cmd::Quit: break;
        }
    }

    void RegisterPower() {
        if (power_notify) return;
        power_params.Callback = &PowerCallback;
        power_params.Context = owner;
        power_notify = RegisterSuspendResumeNotification(&power_params, DEVICE_NOTIFY_CALLBACK);
        if (!power_notify) CT_LOG_WARN("RegisterSuspendResumeNotification failed (%lu); sleep will not pause", GetLastError());
    }
    void UnregisterPower() {
        if (!power_notify) return;
        UnregisterSuspendResumeNotification(power_notify);
        power_notify = nullptr;
        paused_by_suspend = false;
    }

    // ---------------------------------------------------------------------------------------------------------
    HRESULT DoPrepare(platform::D3DDevice& d, const SessionConfig& c, SessionListener* l, bool arm) {
        if (prepared.load() || (state.load() != State::Idle && state.load() != State::Countdown)) return E_ILLEGAL_METHOD_CALL;
        Stopwatch sw;
        sw.Start();
        dev = &d;
        cfg = c;
        listener = l;
        error_pending.store(false);
        disk_warned = false;
        latency_logged = false;
        trigger_qpc = 0;
        audio_fwd.reported.store(false);
        output_dir = cfg.output_dir.empty() ? platform::Paths::DefaultRecordingsDir() : cfg.output_dir;

        quiet = arm;
        const HRESULT hr = BuildComponents(arm);
        quiet = false;
        if (FAILED(hr)) {
            std::string msg;
            {
                ExclusiveGuard g(error_lock);
                msg = error_message.empty() ? "Could not start the recording: " + HrToString(hr) : error_message;
                error_message.clear();
                error_pending.store(false);
            }
            Release();
            bundle.Delete();
            if (arm) {
                CT_LOG_WARN("arming failed (the start will prepare again): %s", msg.c_str());
                return hr;
            }
            CT_LOG_ERROR("prepare failed: %s", msg.c_str());
            if (state.load() == State::Countdown) SetState(State::Idle);
            Notify([msg, hr](SessionListener* li) { li->OnError(msg, hr); });
            return hr;
        }
        armed = arm;
        prepared.store(true, std::memory_order_release);
        {
            ExclusiveGuard g(stats_lock);
            stats = LiveStats{};
            stats.state = state.load();
            stats.encoder_name = writer->EncoderName();
            stats.hardware_encoder = writer->IsHardware();
            stats.fps = fps;
            stats.has_mic = mic != nullptr;
            stats.has_system = sys != nullptr;
            stats.has_camera = cam_writer != nullptr;
        }
        const double ms = sw.ElapsedMs();
        ct::Stats::T("session.prepare").AddUs(static_cast<int64_t>(ms * 1000));
        CT_LOG_INFO("%s in %.1f ms: %dx%d @ %d fps %s via %s (%s), mic %s, system %s, camera %s -> %s", arm ? "armed" : "prepared",
                    ms, capturer->OutputSize().w, capturer->OutputSize().h, fps, media::CodecName(codec),
                    writer->EncoderName().c_str(), writer->IsHardware() ? "hardware" : "software",
                    mic || arm_mic ? "on" : "off", sys || arm_sys ? "on" : "off",
                    cam_writer ? "on" : (arm && cfg.webcam ? "at start" : "off"), bundle.Path().c_str());
        return S_OK;
    }

    void SetPrepareError(std::string msg) {
        ExclusiveGuard g(error_lock);
        error_message = std::move(msg);
    }

    // Microphone / system audio wanted and an endpoint exists (no device is opened to find out).
    bool WantMic() { return cfg.mic && devices_ready && (!cfg.mic_device.empty() || !devices.DefaultId(true).empty()); }
    bool WantSys() { return cfg.system_audio && devices_ready && (!cfg.system_device.empty() || !devices.DefaultId(false).empty()); }

    // Opens and (epoch != 0) starts the declared audio streams of an armed session; a stream that cannot be opened
    // stays in the file as silence (the writer fills it).
    void OpenArmedAudio(int64_t epoch) {
        if (arm_mic) {
            auto mc = std::make_unique<audio::WasapiCapture>();
            audio::CaptureConfig cc;
            cc.device_id = cfg.mic_device;
            cc.stream_id = mic_stream;
            const HRESULT hr = mc->Init(cc, devices);
            if (SUCCEEDED(hr)) {
                mic = std::move(mc);
                if (!mic->Start(epoch, audio_fwd)) Warn("The microphone did not start.", E_FAIL);
            } else {
                Warn("The microphone could not be opened; its track stays silent.", hr);
            }
        } else if (cfg.mic) {
            Warn("No microphone found; recording without it.", S_OK);
        }
        if (arm_sys) {
            auto s = std::make_unique<audio::WasapiLoopback>();
            audio::CaptureConfig cc;
            cc.device_id = cfg.system_device;
            cc.stream_id = sys_stream;
            const HRESULT hr = s->Init(cc, devices);
            if (SUCCEEDED(hr)) {
                sys = std::move(s);
                if (!sys->Start(epoch, audio_fwd)) Warn("System audio did not start.", E_FAIL);
            } else {
                Warn("System audio could not be opened; its track stays silent.", hr);
            }
        }
    }

    HRESULT BuildComponents(bool arm) {
        Stopwatch stage;
        stage.Start();
        double t_devices = 0, t_bundle = 0, t_audio = 0, t_screen = 0, t_capturer = 0, t_writer = 0;
        auto lap = [&stage](double& acc) {
            acc += stage.ElapsedMs();
            stage.Start();
        };
        if (!devices_ready) {
            const HRESULT hr = devices.Init();
            if (FAILED(hr)) CT_LOG_WARN("AudioDevices::Init: %s (no audio)", HrToString(hr).c_str());
            devices_ready = SUCCEEDED(hr);
        }
        lap(t_devices);
        if (arm) BundleWriter::DeleteStaleArmed(output_dir);   // a killed process may have left its arm behind
        std::string path;
        if (!bundle.Create(output_dir, path)) {
            SetPrepareError("Cannot create a recording folder in " + output_dir + ".");
            return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        }
        {
            ExclusiveGuard g(stats_lock);
            bundle_path = path;
        }
        if (arm) bundle.SetHidden(true);

        lap(t_bundle);
        // Audio first: a stream that cannot be opened is left out of the MP4 instead of becoming an empty track.
        // Armed: the streams are declared by endpoint presence and opened at StartNow (no device open while idle).
        uint32_t n_audio = 0;
        arm_mic = arm_sys = false;
        if (arm) {
            arm_mic = WantMic();
            arm_sys = WantSys();
            if (arm_mic) mic_stream = n_audio++;
            if (arm_sys) sys_stream = n_audio++;
        }
        if (!arm && cfg.mic && devices_ready) {
            const bool exists = !cfg.mic_device.empty() || !devices.DefaultId(true).empty();
            if (exists) {
                auto mc = std::make_unique<audio::WasapiCapture>();
                audio::CaptureConfig cc;
                cc.device_id = cfg.mic_device;
                cc.stream_id = n_audio;
                const HRESULT hr = mc->Init(cc, devices);
                if (SUCCEEDED(hr)) {
                    mic = std::move(mc);
                    mic_stream = n_audio++;
                } else {
                    Warn("The microphone could not be opened; recording without it.", hr);
                }
            } else {
                Warn("No microphone found; recording without it.", S_OK);
            }
        }
        if (!arm && cfg.system_audio && devices_ready) {
            const bool exists = !cfg.system_device.empty() || !devices.DefaultId(false).empty();
            if (exists) {
                auto s = std::make_unique<audio::WasapiLoopback>();
                audio::CaptureConfig cc;
                cc.device_id = cfg.system_device;
                cc.stream_id = n_audio;
                const HRESULT hr = s->Init(cc, devices);
                if (SUCCEEDED(hr)) {
                    sys = std::move(s);
                    sys_stream = n_audio++;
                } else {
                    Warn("System audio could not be opened; recording without it.", hr);
                }
            }
        }

        lap(t_audio);
        // Screen + writer with the fps clamp (DECISIONS P04 4.5: the writer does not change the rate itself).
        std::vector<int> rates{std::max(1, cfg.fps)};
        for (int r : {120, 60, 30})
            if (r < rates.front()) rates.push_back(r);
        std::vector<media::VideoCodec> codecs{cfg.codec};
        if (cfg.codec != media::VideoCodec::H264) codecs.push_back(media::VideoCodec::H264);
        HRESULT last = E_FAIL;
        for (media::VideoCodec cd : codecs) {
            for (int rate : rates) {
                capturer = std::make_unique<capture::ScreenCapturer>();
                capture::ScreenCapturer::Config cc;
                cc.display = cfg.display;
                cc.region = cfg.region;
                cc.fps_num = rate;
                cc.fps_den = 1;
                cc.hdr_tonemap = cfg.hdr_tonemap;
                cc.release_duplication_until_start = arm;   // no duplication held while idle
                HRESULT hr = capturer->Init(*dev, cc);
                lap(t_capturer);
                if (FAILED(hr)) {
                    capturer.reset();
                    SetPrepareError("Screen capture could not start on " +
                                    (cfg.display.friendly_name.empty() ? std::string("this display") : cfg.display.friendly_name) +
                                    " (" + HrToString(hr) + ").");
                    return hr;
                }
                writer = std::make_unique<media::Mp4Writer>();
                media::Mp4Writer::Config wc;
                wc.path = bundle.ScreenPath();
                wc.video.codec = cd;
                wc.video.size = capturer->OutputSize();
                wc.video.fps_num = rate;
                wc.video.fps_den = 1;
                wc.video.rc = media::RateControl::Quality;
                wc.video.quality = std::clamp(cfg.quality, 1, 100);
                wc.listener = &writer_listener;
                wc.defer_begin_writing = arm;   // an armed writer that began keeps the AMD MFT thread polling
                if (mic || arm_mic) {
                    media::AudioEncodeConfig ac;
                    ac.label = "mic";
                    wc.audio.push_back(ac);
                }
                if (sys || arm_sys) {
                    media::AudioEncodeConfig ac;
                    ac.label = "system";
                    wc.audio.push_back(ac);
                }
                hr = writer->Init(*dev, wc, &capturer->Nv12Pool());
                lap(t_writer);
                if (SUCCEEDED(hr)) {
                    fps = rate;
                    codec = cd;
                    if (rate != cfg.fps || cd != cfg.codec)
                        CT_LOG_WARN("encoder refused %s @ %d fps; recording %s @ %d fps", media::CodecName(cfg.codec),
                                    cfg.fps, media::CodecName(cd), rate);
                    last = S_OK;
                    break;
                }
                last = hr;
                const std::string msg = writer->ErrorMessage();
                CT_LOG_WARN("Mp4Writer::Init(%s, %d fps): %s", media::CodecName(cd), rate, msg.c_str());
                writer.reset();
                capturer.reset();
                fs::Delete(bundle.ScreenPath());
                if (IsFileError(hr)) {
                    SetPrepareError(msg);
                    return hr;
                }
            }
            if (SUCCEEDED(last)) break;
        }
        if (FAILED(last)) {
            SetPrepareError("No video encoder accepted this recording (" + HrToString(last) + ").");
            return last;
        }
        t_screen = t_capturer + t_writer;

        if (!arm) BuildCamera();
        if (cfg.events) events = std::make_unique<EventRecorder>();
        double t_camera = 0;
        lap(t_camera);
        CT_LOG_INFO("prepare stages: audio devices %.1f ms, bundle %.1f ms, mic/system %.1f ms, screen %.1f ms "
                    "(capturer %.1f, writer %.1f), camera/events %.1f ms",
                    t_devices, t_bundle, t_audio, t_screen, t_capturer, t_writer, t_camera);
        return S_OK;
    }

    // Webcam + cam.mp4 writer (optional: failures only warn).
    void BuildCamera() {
        if (cfg.webcam) {
            auto cam = std::make_unique<capture::WebcamSource>();
            HRESULT hr;
            if (cfg.webcam_synthetic) {
                const SizeI size = cfg.webcam_size.IsEmpty() ? SizeI{1280, 720} : cfg.webcam_size;
                hr = cam->InitWithReader(*dev, capture::CreateSyntheticWebcamReader(*dev, size, 30, true), size, 30, 1);
            } else {
                capture::WebcamSource::Config wc;
                wc.symbolic_link = cfg.webcam_device;
                wc.preferred_size = cfg.webcam_size;
                hr = cam->Init(*dev, wc);
            }
            if (SUCCEEDED(hr)) {
                auto cw = std::make_unique<media::Mp4Writer>();
                media::Mp4Writer::Config wc;
                wc.path = bundle.CamPath();
                wc.video.codec = media::VideoCodec::H264;
                wc.video.size = cam->ActualSize();
                wc.video.fps_num = static_cast<int>(cam->ActualFpsNum());
                wc.video.fps_den = static_cast<int>(cam->ActualFpsDen());
                wc.video.rc = media::RateControl::Quality;
                wc.video.quality = std::clamp(cfg.quality, 1, 100);
                wc.listener = &cam_writer_listener;
                hr = cw->Init(*dev, wc, &cam->Nv12Pool());
                if (SUCCEEDED(hr)) {
                    webcam = std::move(cam);
                    cam_writer = std::move(cw);
                } else {
                    cw.reset();
                    fs::Delete(bundle.CamPath());
                    Warn("The camera file could not be created; recording without the camera.", hr);
                }
            } else {
                Warn("The camera could not be opened; recording without it.", hr);
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    void DoStartNow(int64_t trig) {
        if (!prepared.load() && arm_failed && dev) {
            arm_failed = false;
            if (FAILED(DoPrepare(*dev, cfg, listener, false))) return;
        }
        if (!prepared.load()) {
            CT_LOG_WARN("StartNow without a prepared session ignored");
            if (state.load() == State::Countdown) SetState(State::Idle);
            return;
        }
        if (state.load() != State::Idle && state.load() != State::Countdown) return;
        arm_state.store(0);
        const bool from_arm = armed;
        armed = false;
        if (from_arm && (WantMic() != arm_mic || WantSys() != arm_sys)) {
            // A microphone / output appeared or went away since arming: the file's track layout would be wrong.
            CT_LOG_INFO("audio endpoints changed since arming; preparing again");
            Release();
            bundle.Delete();
            if (FAILED(DoPrepare(*dev, cfg, listener, false))) return;
            DoStartNow(trig);
            return;
        }
        SetState(State::Starting);
        trigger_qpc = trig ? trig : Clock::QpcNow();
        screen_sink.first_frame_qpc.store(0);
        last_latency_ms.store(-1.0);

        const int64_t epoch = Clock::QpcNow();
        clock->Start(epoch);
        clock_source.clock = clock;
        start_qpc = epoch;
        // Screen first (hotkey -> first frame), then audio: audio timestamps come from the device positions, so the
        // order does not move anything in time; the writer pads the few ms before the first packet with silence.
        bool ok = writer->Start();
        if (ok) ok = capturer->Start(epoch, screen_sink);
        if (from_arm) {
            bundle.SetHidden(false);
            rename_to = UtcTime::LocalFileStamp();
            if (ok) OpenArmedAudio(epoch);
            if (ok) BuildCamera();
            ExclusiveGuard g(stats_lock);
            stats.has_mic = mic != nullptr;
            stats.has_system = sys != nullptr;
            stats.has_camera = cam_writer != nullptr;
        } else {
            rename_to.clear();
            if (ok && mic && !mic->Start(epoch, audio_fwd)) Warn("The microphone did not start.", E_FAIL);
            if (ok && sys && !sys->Start(epoch, audio_fwd)) Warn("System audio did not start.", E_FAIL);
        }
        if (ok && cam_writer && !cam_writer->Start()) {
            Warn("The camera writer did not start; recording without the camera.", E_FAIL);
            cam_writer.reset();
            webcam.reset();
        }
        if (ok && webcam && !webcam->Start(clock_source, cam_sink)) Warn("The camera did not start.", E_FAIL);
        if (ok && events) {
            EventRecorder::Config ec;
            ec.region = capturer->Region();
            ec.keystrokes = cfg.keystrokes;
            ec.cursor_hz = cfg.cursor_hz;
            Err err;
            if (!events->Start(ec, bundle.EventsPath(), *clock, &err)) {
                Warn("Cursor and keyboard events are not recorded: " + err.message, E_FAIL);
                events.reset();
            }
        }
        if (!ok) {
            Fail(E_FAIL, "The recording could not start.");
            SetState(State::Recording);   // HandleError finishes from a recording state
            HandleError();
            return;
        }
        BuildEmergencyMeta();
        g_armed.store(owner, std::memory_order_release);
        access_warned = false;
        RegisterPower();
        next_tick_qpc = Clock::QpcNow();
        SetState(State::Recording);
    }

    void DoTogglePause(int64_t qpc) {
        const State s = state.load();
        if (s == State::Recording) {
            clock->PauseAt(qpc);
            capturer->Pause();
            if (mic) mic->SetPaused(true, qpc);
            if (sys) sys->SetPaused(true, qpc);
            if (webcam) webcam->SetPaused(true);
            if (events) events->SetPaused(true);
            SetState(State::Paused);
        } else if (s == State::Paused) {
            clock->ResumeAt(qpc);
            capturer->Resume();
            if (mic) mic->SetPaused(false, qpc);
            if (sys) sys->SetPaused(false, qpc);
            if (webcam) webcam->SetPaused(false);
            if (events) events->SetPaused(false);
            SetState(State::Recording);
        }
    }

    void Tick() {
        LiveStats st;
        {
            ExclusiveGuard g(stats_lock);
            st = stats;
        }
        st.state = state.load();
        st.elapsed_us = clock->NowMediaUs();
        st.file_bytes = fs::FileSize(bundle.ScreenPath()) + (cam_writer ? fs::FileSize(bundle.CamPath()) : 0);
        const capture::CaptureStats& cs = capturer->Stats();
        st.frames = writer->GetStats().video_written.load(std::memory_order_relaxed);
        st.dropped_frames = cs.dropped_pool_empty.load() + cs.dropped_sink.load() + cs.late_ticks.load() +
                           writer->GetStats().video_rejected.load();
        st.mic_peak_db = mic ? PeakDb(mic->Levels()) : audio::Meter::kFloorDb;
        st.sys_peak_db = sys ? PeakDb(sys->Levels()) : audio::Meter::kFloorDb;
        st.free_disk_bytes = fs::FreeSpaceBytes(bundle.Path());
        {
            ExclusiveGuard g(stats_lock);
            stats = st;
        }
        if (!latency_logged) {
            const int64_t first = screen_sink.first_frame_qpc.load(std::memory_order_relaxed);
            if (first) {
                latency_logged = true;
                const double ms = static_cast<double>(first - trigger_qpc) * 1000.0 / Clock::QpcFrequency();
                last_latency_ms.store(ms);
                ct::Stats::T("session.hotkey_to_frame").AddUs(static_cast<int64_t>(ms * 1000));
                CT_LOG_INFO("first frame %.1f ms after the trigger (%.1f ms after StartNow)", ms,
                            static_cast<double>(first - start_qpc) * 1000.0 / Clock::QpcFrequency());
            }
        }
        Notify([st](SessionListener* l) { l->OnTick(st); });

        // Access lost for a while (secure desktop, fullscreen game, display change): frames repeat; say so once.
        const int64_t lost_since = cs.lost_since_qpc.load(std::memory_order_relaxed);
        if (lost_since == 0) {
            access_warned = false;
        } else if (!access_warned && Clock::QpcNow() - lost_since > 3 * Clock::QpcFrequency()) {
            access_warned = true;
            Warn("Windows blocked screen capture (secure desktop or display change). The last image repeats until it is back.",
                 DXGI_ERROR_ACCESS_LOST);
        }

        if (st.free_disk_bytes > 0 && st.free_disk_bytes < kStopFreeBytes) {
            Fail(HRESULT_FROM_WIN32(ERROR_DISK_FULL), "Disk almost full: the recording was stopped and saved.");
        } else if (st.free_disk_bytes > 0 && st.free_disk_bytes < kWarnFreeBytes && !disk_warned) {
            disk_warned = true;
            Warn("Less than 1 GB of disk space left for recordings.", S_OK);
        }
    }

    void HandleError() {
        HRESULT hr;
        std::string msg;
        {
            ExclusiveGuard g(error_lock);
            hr = error_hr;
            msg = error_message;
            error_pending.store(false);
            error_message.clear();
        }
        const State s = state.load();
        if (s != State::Recording && s != State::Paused && s != State::Starting) return;
        CT_LOG_ERROR("recording error: %s (%s)", msg.c_str(), HrToString(hr).c_str());
        SetState(State::Error);
        Notify([msg, hr](SessionListener* l) { l->OnError(msg, hr); });
        Finish(/*keep=*/true, /*notify=*/true);
    }

    // Stop order (P05 5.1): capture -> webcam -> audio -> writers -> events -> meta.
    void StopComponents() {
        if (capturer) capturer->Stop();
        if (webcam) webcam->Stop();
        if (mic) mic->Stop();
        if (sys) sys->Stop();
        if (writer) {
            const HRESULT hr = writer->Finalize();
            if (FAILED(hr)) CT_LOG_WARN("screen.mp4 Finalize: %s", writer->ErrorMessage().c_str());
        }
        if (cam_writer) {
            const HRESULT hr = cam_writer->Finalize();
            if (FAILED(hr)) CT_LOG_WARN("cam.mp4 Finalize: %s", cam_writer->ErrorMessage().c_str());
        }
        if (events) events->Stop();
    }

    BundleMeta MakeMeta() const {
        BundleMeta m;
        m.app_version = CT_VERSION_STRING;
        std::string utc = UtcTime::NowIso8601();   // "...T12:03:44.123Z" -> "...T12:03:44Z"
        const size_t dot = utc.find('.');
        if (dot != std::string::npos) utc = utc.substr(0, dot) + "Z";
        m.created_utc = utc;
        m.epoch_qpc = clock->epoch_qpc;
        m.qpc_frequency = Clock::QpcFrequency();
        const SizeI size = capturer ? capturer->OutputSize() : SizeI{};
        m.width = size.w;
        m.height = size.h;
        m.fps_num = fps;
        m.fps_den = 1;
        m.codec = media::CodecName(codec);
        m.region = capturer ? capturer->Region() : cfg.region;
        m.monitor_device = WideToUtf8(cfg.display.device_name);
        m.monitor_name = cfg.display.friendly_name;
        m.monitor_origin_x = cfg.display.rect.x;
        m.monitor_origin_y = cfg.display.rect.y;
        m.monitor_dpi = static_cast<int>(cfg.display.dpi);
        m.monitor_hdr = cfg.display.hdr;
        if (webcam && cam_writer) {
            m.has_camera = true;
            m.camera_width = webcam->ActualSize().w;
            m.camera_height = webcam->ActualSize().h;
            m.camera_fps_num = static_cast<int>(webcam->ActualFpsNum());
            m.camera_fps_den = static_cast<int>(webcam->ActualFpsDen());
            m.camera_device = cfg.webcam_synthetic ? "Synthetic camera" : webcam->DeviceName();
        }
        if (mic) {
            m.mic.present = true;
            m.mic.stream_index = static_cast<int>(writer->AudioStreamIndex(mic_stream));
            m.mic.device = mic->DeviceName();
        }
        if (sys) {
            m.system_audio.present = true;
            m.system_audio.stream_index = static_cast<int>(writer->AudioStreamIndex(sys_stream));
            m.system_audio.device = sys->DeviceName();
        }
        m.cursor_hz = cfg.cursor_hz;
        m.keystrokes = KeystrokeName(cfg.keystrokes);
        m.os = OsVersionString();
        m.gpu = dev ? dev->AdapterName() : std::string();
        if (writer) m.encoder = writer->EncoderName() + (writer->IsHardware() ? " (hardware)" : " (software)");
        for (const PauseSpan& p : clock->Pauses()) m.pauses.push_back({p.media_t_us, p.duration_us});
        return m;
    }

    void BuildEmergencyMeta() {
        BundleMeta m = MakeMeta();
        m.incomplete = true;
        emergency_meta = BuildMetaJson(m).Dump(2);
        emergency_meta += '\n';
        emergency_meta_path = fs::ToWinPath(bundle.MetaPath());
        emergency_done.store(false);
    }

    void Finish(bool keep, bool notify) {
        g_armed.store(nullptr, std::memory_order_release);
        UnregisterPower();
        SetState(State::Finalizing);
        Stopwatch sw;
        sw.Start();
        StopComponents();
        TimeUs duration = clock->NowMediaUs();
        uint64_t frames = 0;
        if (writer) {
            frames = writer->GetStats().video_written.load();
            if (frames > 0) duration = writer->GetStats().last_video_us.load() + FrameTimeUs(1, fps, 1);
        }
        std::string path = bundle.Path();
        if (keep) {
            BundleMeta m = MakeMeta();
            m.duration_us = duration;
            if (!bundle.WriteMeta(BuildMetaJson(m))) Warn("meta.json could not be written.", E_FAIL);
        }
        const double ms = sw.ElapsedMs();
        CT_LOG_INFO("finalized in %.0f ms: %llu frames, %.3f s, %llu bytes -> %s", ms,
                    static_cast<unsigned long long>(frames), duration / 1e6,
                    static_cast<unsigned long long>(fs::FileSize(bundle.ScreenPath())), path.c_str());
        Release();
        if (!keep) bundle.Delete();
        if (keep && !rename_to.empty() && rename_to != bundle.Stem()) {
            // Armed long before the start: name the bundle after the start time (every file is closed now).
            if (bundle.Rename(rename_to)) {
                path = bundle.Path();
                ExclusiveGuard g(stats_lock);
                bundle_path = path;
            }
        }
        rename_to.clear();
        prepared.store(false);
        SetState(State::Idle);
        if (keep && notify && frames > 0) Notify([path](SessionListener* l) { l->OnFinished(path); });
    }

    void DoDisarm() {
        arm_failed = false;
        const State s = state.load();
        if (!armed || !prepared.load() || (s != State::Idle && s != State::Countdown)) return;
        Release();
        bundle.Delete();
    }

    void DoCancel() {
        const State s = state.load();
        if (s == State::Recording || s == State::Paused) {
            Finish(/*keep=*/false, /*notify=*/false);
            return;
        }
        if (!prepared.load() && s != State::Countdown) return;
        Release();
        bundle.Delete();
        prepared.store(false);
        SetState(State::Idle);
    }

    // Writers before the pools they reference (capturer, webcam).
    void Release() {
        g_armed.store(nullptr, std::memory_order_release);
        UnregisterPower();
        live_mic_db.store(audio::Meter::kFloorDb);
        live_sys_db.store(audio::Meter::kFloorDb);
        if (capturer) capturer->Stop();
        if (webcam) webcam->Stop();
        if (mic) mic->Stop();
        if (sys) sys->Stop();
        if (events) events->Stop();
        writer.reset();
        cam_writer.reset();
        capturer.reset();
        webcam.reset();
        mic.reset();
        sys.reset();
        events.reset();
        armed = false;
        arm_mic = arm_sys = false;
        prepared.store(false);
    }
};

// -------------------------------------------------------------------------------------------------------------------

RecordingSession::RecordingSession() : m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    m_impl->clock = &m_clock;
    Thread::Options opt;
    opt.name = "ct-session";
    opt.com_mta = true;
    m_impl->thread.Start(opt, [this] { m_impl->Run(); });
    if (!g_emergency_registered.exchange(true)) platform::CrashHandler::RegisterEmergency(&EmergencyThunk, nullptr);
}

RecordingSession::~RecordingSession() {
    Cancel();
    Impl::Command q;
    q.kind = Impl::Cmd::Quit;
    m_impl->Post(std::move(q));
    if (!m_impl->thread.Join(15000)) CT_FATAL("ct-session did not stop within 15 s");
    RecordingSession* self = this;
    g_armed.compare_exchange_strong(self, nullptr);
}

HRESULT RecordingSession::Prepare(platform::D3DDevice& dev, const SessionConfig& cfg, SessionListener* listener) {
    HRESULT hr = E_FAIL;
    Event done(true, false);
    Impl::Command c;
    c.kind = Impl::Cmd::Prepare;
    c.dev = &dev;
    c.cfg = cfg;
    c.listener = listener;
    c.result = &hr;
    c.done = &done;
    m_impl->preparing.store(true);
    m_impl->countdown_s.store(cfg.countdown_s);
    m_impl->Post(std::move(c));
    done.Wait();
    return hr;
}

void RecordingSession::PrepareAsync(platform::D3DDevice& dev, const SessionConfig& cfg, SessionListener* listener) {
    Impl::Command c;
    c.kind = Impl::Cmd::Prepare;
    c.dev = &dev;
    c.cfg = cfg;
    c.listener = listener;
    m_impl->preparing.store(true);
    m_impl->countdown_s.store(cfg.countdown_s);
    m_impl->Post(std::move(c));
}

bool RecordingSession::IsPrepared() const { return m_impl->prepared.load(std::memory_order_acquire); }

void RecordingSession::Arm(platform::D3DDevice& dev, const SessionConfig& cfg, SessionListener* listener) {
    {
        ExclusiveGuard g(m_impl->arm_lock);
        m_impl->arm_cfg = cfg;
    }
    m_impl->arm_state.store(1);
    Impl::Command c;
    c.kind = Impl::Cmd::Prepare;
    c.arm = true;
    c.arm_gen = m_impl->arm_gen.fetch_add(1) + 1;
    c.dev = &dev;
    c.cfg = cfg;
    c.listener = listener;
    m_impl->Post(std::move(c));
}

void RecordingSession::Disarm() {
    m_impl->arm_state.store(0);
    m_impl->arm_gen.fetch_add(1);
    Impl::Command c;
    c.kind = Impl::Cmd::Disarm;
    m_impl->Post(std::move(c));
}

bool RecordingSession::IsArmed() const { return m_impl->arm_state.load() != 0; }

bool RecordingSession::IsArmedFor(const SessionConfig& cfg) const {
    if (m_impl->arm_state.load() == 0) return false;
    SharedGuard g(m_impl->arm_lock);
    return SameSetup(m_impl->arm_cfg, cfg);
}

bool RecordingSession::SameSetup(const SessionConfig& a, const SessionConfig& b) {
    const RectI &ra = a.display.rect, &rb = b.display.rect, &ga = a.region, &gb = b.region;
    return a.display.device_name == b.display.device_name && ra.x == rb.x && ra.y == rb.y && ra.w == rb.w && ra.h == rb.h &&
           a.display.rotation == b.display.rotation && a.display.hdr == b.display.hdr && ga.x == gb.x && ga.y == gb.y &&
           ga.w == gb.w && ga.h == gb.h && a.fps == b.fps && a.codec == b.codec && a.quality == b.quality &&
           a.hdr_tonemap == b.hdr_tonemap && a.mic == b.mic && a.mic_device == b.mic_device &&
           a.system_audio == b.system_audio && a.system_device == b.system_device && a.webcam == b.webcam &&
           a.webcam_device == b.webcam_device && a.webcam_size.w == b.webcam_size.w && a.webcam_size.h == b.webcam_size.h &&
           a.webcam_synthetic == b.webcam_synthetic && a.output_dir == b.output_dir && a.keystrokes == b.keystrokes &&
           a.cursor_hz == b.cursor_hz && a.events == b.events;
}

void RecordingSession::StartCountdown() {
    State expected = State::Idle;   // the prepare may still be queued (PrepareAsync posted from another thread)
    if (!m_impl->state.compare_exchange_strong(expected, State::Countdown)) return;
    CT_LOG_INFO("state Idle -> Countdown");
    {
        ExclusiveGuard g(m_impl->stats_lock);
        m_impl->stats.state = State::Countdown;
    }
    const int secs = m_impl->countdown_s.load();
    m_impl->Notify([](SessionListener* l) { l->OnStateChanged(State::Countdown); });
    m_impl->Notify([secs](SessionListener* l) { l->OnCountdown(secs); });
}

void RecordingSession::StartNow(int64_t trigger_qpc) {
    Impl::Command c;
    c.kind = Impl::Cmd::StartNow;
    c.qpc = trigger_qpc;
    m_impl->Post(std::move(c));
}

void RecordingSession::TogglePause() {
    Impl::Command c;
    c.kind = Impl::Cmd::Pause;
    c.qpc = Clock::QpcNow();   // one QPC for every stream
    m_impl->Post(std::move(c));
}

void RecordingSession::AddMarker(std::string label) {
    Impl::Command c;
    c.kind = Impl::Cmd::Marker;
    c.label = std::move(label);
    m_impl->Post(std::move(c));
}

void RecordingSession::Stop() {
    Impl::Command c;
    c.kind = Impl::Cmd::Stop;
    m_impl->Post(std::move(c));
}

void RecordingSession::Cancel() {
    m_impl->arm_state.store(0);
    m_impl->arm_gen.fetch_add(1);
    Impl::Command c;
    c.kind = Impl::Cmd::Cancel;
    m_impl->Post(std::move(c));
}

void RecordingSession::EmergencyFinalize() {
    Impl& s = *m_impl;
    if (s.emergency_done.exchange(true)) return;
    // The writers are never destroyed after Abort (ct-encode may still be inside IMFSinkWriter::Finalize).
    if (s.writer) s.writer->Abort();
    if (s.cam_writer) s.cam_writer->Abort();
    if (s.events) s.events->Flush(500);
    if (s.emergency_meta.empty() || s.emergency_meta_path.empty()) return;
    HANDLE f = CreateFileW(s.emergency_meta_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, s.emergency_meta.data(), static_cast<DWORD>(s.emergency_meta.size()), &written, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
}

void RecordingSession::OnSystemSuspend() {
    // The capture and encode threads must be paused before the GPU powers down; the system waits for this callback.
    m_impl->suspend_done.Reset();
    Impl::Command c;
    c.kind = Impl::Cmd::Suspend;
    m_impl->Post(std::move(c));
    if (!m_impl->suspend_done.Wait(1000)) CT_LOG_WARN("suspend: the session did not pause within 1 s");
}

void RecordingSession::OnSystemResume() {
    Impl::Command c;
    c.kind = Impl::Cmd::Resume;
    m_impl->Post(std::move(c));
}

State RecordingSession::GetState() const { return m_impl->state.load(std::memory_order_acquire); }

bool RecordingSession::IsActive() const { return GetState() != State::Idle; }

LiveStats RecordingSession::Stats() const {
    SharedGuard g(m_impl->stats_lock);
    return m_impl->stats;
}

std::string RecordingSession::BundlePath() const {
    SharedGuard g(m_impl->stats_lock);
    return m_impl->bundle_path;
}

double RecordingSession::LastStartLatencyMs() const { return m_impl->last_latency_ms.load(); }

void RecordingSession::LiveLevels(float* mic_db, float* sys_db) const {
    if (mic_db) *mic_db = m_impl->live_mic_db.load(std::memory_order_relaxed);
    if (sys_db) *sys_db = m_impl->live_sys_db.load(std::memory_order_relaxed);
}

bool RecordingSession::WaitIdle(uint32_t timeout_ms) {
    const int64_t end = Clock::QpcNow() + static_cast<int64_t>(timeout_ms) * Clock::QpcFrequency() / 1000;
    while (Clock::QpcNow() < end) {
        if (m_impl->pending.load() == 0 && GetState() == State::Idle) return true;
        Sleep(10);
    }
    return false;
}

} // namespace ct::recorder
