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
#include <ct/media/probe.h>
#include <ct/platform/crash_handler.h>
#include <ct/platform/paths.h>
#include <ct/recorder/bundle_writer.h>
#include <ct/version.h>

#include <audioclient.h>

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

const char* FailureCauseName(FailureCause c) {
    switch (c) {
    case FailureCause::Other: return "other";
    case FailureCause::DiskFull: return "disk-full";
    case FailureCause::OutputFolder: return "output-folder";
    case FailureCause::DeviceLost: return "device-lost";
    case FailureCause::NoEncoder: return "no-encoder";
    case FailureCause::CaptureFailed: return "capture-failed";
    case FailureCause::AccessLost: return "access-lost";
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

bool IsDiskFull(HRESULT hr) { return hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL); }

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// Writer errors (screen.mp4) by HRESULT.
FailureCause WriterCause(HRESULT hr) {
    if (IsDiskFull(hr)) return FailureCause::DiskFull;
    if (IsDeviceLost(hr)) return FailureCause::DeviceLost;
    if (IsFileError(hr)) return FailureCause::OutputFolder;
    return FailureCause::Other;
}

bool IsMicBusy(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED ||
           hr == AUDCLNT_E_EXCLUSIVE_MODE_ONLY;
}

int64_t MsToQpc(int ms) { return static_cast<int64_t>(ms) * Clock::QpcFrequency() / 1000; }

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
    enum class Cmd { Prepare, StartNow, Pause, Marker, Stop, Cancel, Quit };
    struct Command {
        Cmd kind;
        int64_t qpc = 0;
        std::string label;
        platform::D3DDevice* dev = nullptr;
        SessionConfig cfg;
        SessionListener* listener = nullptr;
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
    std::atomic<bool> preparing{false};
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
    FailureCause error_cause = FailureCause::Other;
    // Fault injection deadlines (ct-session; 0 = not armed / already fired).
    int64_t fault_capture_qpc = 0, fault_audio_qpc = 0;

    // --- owned by ct-session ---
    platform::D3DDevice* dev = nullptr;
    SessionConfig cfg;
    bool devices_ready = false;
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
            if (IsDeviceLost(hr)) s->Fail(hr, "Screen capture stopped (the graphics device was lost).", FailureCause::DeviceLost);
            else if (hr == DXGI_ERROR_ACCESS_LOST) s->Fail(hr, "Screen capture lost access to the display.", FailureCause::AccessLost);
            else s->Fail(hr, "Screen capture stopped (" + HrToString(hr) + ").", FailureCause::CaptureFailed);
        }
    } screen_sink;
    struct CamSink final : capture::FrameSink {
        Impl* s = nullptr;
        bool OnFrame(ID3D11Texture2D* tex, uint32_t index, TimeUs t_us) override {
            return s->cam_writer->OnFrame(tex, index, t_us);
        }
        void OnError(HRESULT hr) override { s->Warn("The camera stopped; the screen keeps recording.", hr, WarningCause::Camera); }
    } cam_sink;
    struct AudioFwd final : audio::AudioSink {
        Impl* s = nullptr;
        std::atomic<bool> reported{false};
        bool OnAudio(uint32_t stream, const audio::AudioPacket& p) override { return s->writer->OnAudio(stream, p); }
        // The capture engine keeps the stream going with silence and reopens the device (P03); report once.
        void OnAudioError(uint32_t stream, HRESULT hr) override {
            if (reported.exchange(true)) return;
            const bool is_mic = s->mic && stream == s->mic_stream;
            s->Warn(is_mic ? "The microphone stopped; recording silence until it is back."
                           : "System audio stopped; recording silence until it is back.",
                    hr, is_mic ? WarningCause::AudioDeviceLost : WarningCause::SystemAudio);
        }
    } audio_fwd;
    struct WriterListener final : media::Mp4Writer::Listener {
        Impl* s = nullptr;
        bool cam = false;
        void OnWriterError(HRESULT hr) override {
            media::Mp4Writer* w = cam ? s->cam_writer.get() : s->writer.get();
            const std::string msg = w ? w->ErrorMessage() : std::string("Writing the recording failed.");
            if (cam) s->Warn("Writing the camera file failed: " + msg, hr, WarningCause::Camera);
            else s->Fail(hr, msg, WriterCause(hr));
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

    void Fail(HRESULT hr, std::string message, FailureCause cause) {
        {
            ExclusiveGuard g(error_lock);
            if (error_pending.load()) return;
            error_hr = hr;
            error_message = std::move(message);
            error_cause = cause;
            error_pending.store(true, std::memory_order_release);
        }
        wake.Set();
    }

    void Warn(std::string message, HRESULT hr, WarningCause cause = WarningCause::Other) {
        CT_LOG_WARN("%s (%s)", message.c_str(), HrToString(hr).c_str());
        SessionWarning w;
        w.cause = cause;
        w.message = std::move(message);
        w.hr = hr;
        Notify([w = std::move(w)](SessionListener* l) { l->OnWarningInfo(w); });
    }

    SessionFailure MakeFailure(FailureCause cause, std::string message, HRESULT hr) {
        SessionFailure f;
        f.cause = cause;
        f.message = std::move(message);
        f.hr = hr;
        f.output_dir = output_dir;
        {
            SharedGuard g(stats_lock);
            f.bundle_path = bundle_path;
        }
        return f;
    }

    // Fault injection (ct-session, while recording): fire the armed capture / audio errors when their time came.
    void PollFaults() {
        const int64_t now = Clock::QpcNow();
        if (fault_capture_qpc && now >= fault_capture_qpc) {
            fault_capture_qpc = 0;
            CT_LOG_WARN("fault injection: capture error %s", HrToString(cfg.faults.capture_error_hr).c_str());
            screen_sink.OnError(cfg.faults.capture_error_hr);
        }
        if (fault_audio_qpc && now >= fault_audio_qpc) {
            fault_audio_qpc = 0;
            CT_LOG_WARN("fault injection: audio error %s", HrToString(cfg.faults.audio_error_hr).c_str());
            audio_fwd.OnAudioError(mic ? mic_stream : sys_stream, cfg.faults.audio_error_hr);
        }
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

            if (st == State::Recording) PollFaults();
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
            const HRESULT hr = DoPrepare(*c.dev, c.cfg, c.listener);
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
        case Cmd::Quit: break;
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    HRESULT DoPrepare(platform::D3DDevice& d, const SessionConfig& c, SessionListener* l) {
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

        const HRESULT hr = BuildComponents();
        if (FAILED(hr)) {
            std::string msg;
            FailureCause cause;
            {
                ExclusiveGuard g(error_lock);
                msg = error_message.empty() ? "Could not start the recording: " + HrToString(hr) : error_message;
                cause = error_cause;
                error_message.clear();
                error_cause = FailureCause::Other;
                error_pending.store(false);
            }
            CT_LOG_ERROR("prepare failed (%s): %s", FailureCauseName(cause), msg.c_str());
            SessionFailure f = MakeFailure(cause, msg, hr);
            f.during_prepare = true;
            Release();
            bundle.Delete();
            if (state.load() == State::Countdown) SetState(State::Idle);
            Notify([f = std::move(f)](SessionListener* li) { li->OnFailure(f); });
            return hr;
        }
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
        CT_LOG_INFO("prepared in %.1f ms: %dx%d @ %d fps %s via %s (%s), mic %s, system %s, camera %s -> %s", ms,
                    capturer->OutputSize().w, capturer->OutputSize().h, fps, media::CodecName(codec),
                    writer->EncoderName().c_str(), writer->IsHardware() ? "hardware" : "software", mic ? "on" : "off",
                    sys ? "on" : "off", cam_writer ? "on" : "off", bundle.Path().c_str());
        return S_OK;
    }

    void SetPrepareError(std::string msg, FailureCause cause) {
        ExclusiveGuard g(error_lock);
        error_message = std::move(msg);
        error_cause = cause;
    }

    // One Mp4Writer::Init attempt, with the P11 fault injection in front of it.
    HRESULT InitWriter(media::Mp4Writer& w, media::Mp4Writer::Config& wc) {
        if (FAILED(cfg.faults.encoder_init_hr)) return cfg.faults.encoder_init_hr;
        if (wc.video.prefer_hardware && FAILED(cfg.faults.hw_encoder_init_hr)) return cfg.faults.hw_encoder_init_hr;
        return w.Init(*dev, wc, &capturer->Nv12Pool());
    }

    HRESULT BuildComponents() {
        if (!devices_ready) {
            const HRESULT hr = devices.Init();
            if (FAILED(hr)) CT_LOG_WARN("AudioDevices::Init: %s (no audio)", HrToString(hr).c_str());
            devices_ready = SUCCEEDED(hr);
        }
        std::string path;
        if (!bundle.Create(output_dir, path)) {
            SetPrepareError("Cannot create a recording folder in " + output_dir + ".", FailureCause::OutputFolder);
            return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        }
        {
            ExclusiveGuard g(stats_lock);
            bundle_path = path;
        }

        // Audio first: a stream that cannot be opened is left out of the MP4 instead of becoming an empty track.
        uint32_t n_audio = 0;
        if (cfg.mic && (devices_ready || FAILED(cfg.faults.mic_open_hr))) {
            const bool injected = FAILED(cfg.faults.mic_open_hr);
            const bool exists = injected || !cfg.mic_device.empty() || !devices.DefaultId(true).empty();
            if (exists) {
                auto mc = std::make_unique<audio::WasapiCapture>();
                audio::CaptureConfig cc;
                cc.device_id = cfg.mic_device;
                cc.stream_id = n_audio;
                const HRESULT hr = injected ? cfg.faults.mic_open_hr : mc->Init(cc, devices);
                if (SUCCEEDED(hr)) {
                    mic = std::move(mc);
                    mic_stream = n_audio++;
                } else if (IsMicBusy(hr)) {
                    // Left out of the file (no empty track); the recording goes on (P11 11.5, DECISIONS).
                    Warn("The microphone is in use by another app; recording without it.", hr, WarningCause::MicBusy);
                } else {
                    Warn("The microphone could not be opened; recording without it.", hr, WarningCause::MicMissing);
                }
            } else {
                Warn("No microphone found; recording without it.", S_OK, WarningCause::MicMissing);
            }
        }
        if (cfg.system_audio && devices_ready) {
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
                    Warn("System audio could not be opened; recording without it.", hr, WarningCause::SystemAudio);
                }
            }
        }

        // Screen + writer with the fps clamp (DECISIONS P04 4.5: the writer does not change the rate itself).
        std::vector<int> rates{std::max(1, cfg.fps)};
        for (int r : {120, 60, 30})
            if (r < rates.front()) rates.push_back(r);
        // Attempts: the wanted codec, then H.264, each at the clamped rates, preferring a hardware encoder (Mp4Writer
        // itself retries its sink setup with the software MFT); finally an explicit software-only H.264 pass, for
        // hardware encoders that fail after the sink setup (P11 11.5: "encoder missing -> software MF").
        struct Attempt { media::VideoCodec codec; bool hw; };
        std::vector<Attempt> attempts{{cfg.codec, true}};
        if (cfg.codec != media::VideoCodec::H264) attempts.push_back({media::VideoCodec::H264, true});
        attempts.push_back({media::VideoCodec::H264, false});
        HRESULT last = E_FAIL;
        for (const Attempt& at : attempts) {
            const media::VideoCodec cd = at.codec;
            for (int rate : rates) {
                capturer = std::make_unique<capture::ScreenCapturer>();
                capture::ScreenCapturer::Config cc;
                cc.display = cfg.display;
                cc.region = cfg.region;
                cc.fps_num = rate;
                cc.fps_den = 1;
                cc.hdr_tonemap = cfg.hdr_tonemap;
                HRESULT hr = capturer->Init(*dev, cc);
                if (FAILED(hr)) {
                    capturer.reset();
                    SetPrepareError("Screen capture could not start on " +
                                    (cfg.display.friendly_name.empty() ? std::string("this display") : cfg.display.friendly_name) +
                                    " (" + HrToString(hr) + ").",
                                    IsDeviceLost(hr) ? FailureCause::DeviceLost : FailureCause::CaptureFailed);
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
                wc.video.prefer_hardware = at.hw;
                wc.listener = &writer_listener;
                if (mic) {
                    media::AudioEncodeConfig ac;
                    ac.label = "mic";
                    wc.audio.push_back(ac);
                }
                if (sys) {
                    media::AudioEncodeConfig ac;
                    ac.label = "system";
                    wc.audio.push_back(ac);
                }
                hr = InitWriter(*writer, wc);
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
                const std::string msg = writer->ErrorMessage().empty() ? HrToString(hr) : writer->ErrorMessage();
                CT_LOG_WARN("Mp4Writer::Init(%s, %d fps, %s): %s", media::CodecName(cd), rate, at.hw ? "hardware first" : "software",
                            msg.c_str());
                writer.reset();
                capturer.reset();
                fs::Delete(bundle.ScreenPath());
                if (IsFileError(hr)) {
                    SetPrepareError(msg, WriterCause(hr));
                    return hr;
                }
            }
            if (SUCCEEDED(last)) break;
        }
        if (FAILED(last)) {
            SetPrepareError("No video encoder accepted this recording (" + HrToString(last) + ").", FailureCause::NoEncoder);
            return last;
        }
        if (!writer->IsHardware())
            Warn("No hardware video encoder is available; recording with the software encoder (higher CPU use).", S_OK,
                 WarningCause::SoftwareEncoder);
        if (FAILED(cfg.faults.writer_error_hr))
            writer->InjectWriteErrorForTest(cfg.faults.writer_error_after_samples, cfg.faults.writer_error_hr);

        // Webcam (optional: failures only warn).
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
                    Warn("The camera file could not be created; recording without the camera.", hr, WarningCause::Camera);
                }
            } else {
                Warn("The camera could not be opened; recording without it.", hr, WarningCause::Camera);
            }
        }

        if (cfg.events) events = std::make_unique<EventRecorder>();
        return S_OK;
    }

    // ---------------------------------------------------------------------------------------------------------
    void DoStartNow(int64_t trig) {
        if (!prepared.load()) {
            CT_LOG_WARN("StartNow without a prepared session ignored");
            if (state.load() == State::Countdown) SetState(State::Idle);
            return;
        }
        if (state.load() != State::Idle && state.load() != State::Countdown) return;
        SetState(State::Starting);
        trigger_qpc = trig ? trig : Clock::QpcNow();
        screen_sink.first_frame_qpc.store(0);
        last_latency_ms.store(-1.0);

        const int64_t epoch = Clock::QpcNow();
        clock->Start(epoch);
        clock_source.clock = clock;
        start_qpc = epoch;
        bool ok = writer->Start();
        if (ok && cam_writer && !cam_writer->Start()) {
            Warn("The camera writer did not start; recording without the camera.", E_FAIL, WarningCause::Camera);
            cam_writer.reset();
            webcam.reset();
        }
        if (ok && mic && !mic->Start(epoch, audio_fwd)) Warn("The microphone did not start.", E_FAIL, WarningCause::MicMissing);
        if (ok && sys && !sys->Start(epoch, audio_fwd)) Warn("System audio did not start.", E_FAIL, WarningCause::SystemAudio);
        if (ok) ok = capturer->Start(epoch, screen_sink);
        if (ok && webcam && !webcam->Start(clock_source, cam_sink)) Warn("The camera did not start.", E_FAIL, WarningCause::Camera);
        if (ok && events) {
            EventRecorder::Config ec;
            ec.region = capturer->Region();
            ec.keystrokes = cfg.keystrokes;
            ec.cursor_hz = cfg.cursor_hz;
            Err err;
            if (!events->Start(ec, bundle.EventsPath(), *clock, &err)) {
                Warn("Cursor and keyboard events are not recorded: " + err.message, E_FAIL, WarningCause::Events);
                events.reset();
            }
        }
        if (!ok) {
            Fail(E_FAIL, "The recording could not start.", FailureCause::CaptureFailed);
            SetState(State::Recording);   // HandleError finishes from a recording state
            HandleError();
            return;
        }
        BuildEmergencyMeta();
        g_armed.store(owner, std::memory_order_release);
        next_tick_qpc = Clock::QpcNow();
        const SessionFaults& fl = cfg.faults;
        fault_capture_qpc = FAILED(fl.capture_error_hr) ? Clock::QpcNow() + MsToQpc(std::max(0, fl.capture_error_after_ms)) : 0;
        fault_audio_qpc = FAILED(fl.audio_error_hr) ? Clock::QpcNow() + MsToQpc(std::max(0, fl.audio_error_after_ms)) : 0;
        if (fl.Any()) CT_LOG_WARN("fault injection armed for this recording");
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
        const bool fake_disk = cfg.faults.free_disk_bytes && Clock::QpcNow() - start_qpc >= MsToQpc(cfg.faults.free_disk_after_ms);
        st.free_disk_bytes = fake_disk ? cfg.faults.free_disk_bytes : fs::FreeSpaceBytes(bundle.Path());
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

        if (st.free_disk_bytes > 0 && st.free_disk_bytes < kStopFreeBytes) {
            Fail(HRESULT_FROM_WIN32(ERROR_DISK_FULL), "Disk almost full: the recording was stopped and saved.", FailureCause::DiskFull);
        } else if (st.free_disk_bytes > 0 && st.free_disk_bytes < kWarnFreeBytes && !disk_warned) {
            disk_warned = true;
            Warn("Less than 1 GB of disk space left for recordings.", S_OK, WarningCause::LowDisk);
        }
    }

    void HandleError() {
        HRESULT hr;
        std::string msg;
        FailureCause cause;
        {
            ExclusiveGuard g(error_lock);
            hr = error_hr;
            msg = error_message;
            cause = error_cause;
            error_pending.store(false);
            error_message.clear();
            error_cause = FailureCause::Other;
        }
        const State s = state.load();
        if (s != State::Recording && s != State::Paused && s != State::Starting) return;
        CT_LOG_ERROR("recording error (%s): %s (%s)", FailureCauseName(cause), msg.c_str(), HrToString(hr).c_str());
        SetState(State::Error);
        SessionFailure f = MakeFailure(cause, std::move(msg), hr);
        f.during_prepare = s == State::Starting;
        // Stop cleanly and keep what was written (P11 11.5): the writer already finalized what it had on a write
        // error; Finish probes screen.mp4 and marks meta.json "incomplete" when the file is not readable.
        FinishResult r;
        Finish(/*keep=*/true, /*notify=*/true, &r);
        f.saved = r.frames > 0;
        f.finalized = r.playable;
        Notify([f = std::move(f)](SessionListener* l) { l->OnFailure(f); });
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

    struct FinishResult {
        uint64_t frames = 0;
        bool playable = true;
    };

    void Finish(bool keep, bool notify, FinishResult* result = nullptr) {
        g_armed.store(nullptr, std::memory_order_release);
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
        // After a failure (result != null) check that the file is readable: a writer error or a lost device may
        // have left it without an index.
        bool playable = true;
        if (result && keep && frames > 0) {
            media::ProbeResult pr;
            const HRESULT ph = media::Probe(bundle.ScreenPath(), pr);
            playable = SUCCEEDED(ph) && pr.video_streams == 1;
            if (!playable) CT_LOG_WARN("screen.mp4 is not readable after the failure: %s", HrToString(ph).c_str());
        }
        std::string path = bundle.Path();
        if (keep) {
            BundleMeta m = MakeMeta();
            m.duration_us = duration;
            m.incomplete = !playable;
            if (!bundle.WriteMeta(BuildMetaJson(m))) Warn("meta.json could not be written.", E_FAIL);
        }
        const double ms = sw.ElapsedMs();
        CT_LOG_INFO("finalized in %.0f ms: %llu frames, %.3f s, %llu bytes -> %s", ms,
                    static_cast<unsigned long long>(frames), duration / 1e6,
                    static_cast<unsigned long long>(fs::FileSize(bundle.ScreenPath())), path.c_str());
        Release();
        if (!keep) bundle.Delete();
        prepared.store(false);
        SetState(State::Idle);
        if (result) {
            result->frames = frames;
            result->playable = playable;
        }
        if (keep && notify && frames > 0) Notify([path](SessionListener* l) { l->OnFinished(path); });
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
