// audio_bench: records system audio (and optionally the microphone) through the real capture pipeline into WAV
// files and checks the result.
//
//   audio_bench --seconds 60 [--tone] [--mic] [--out dir]
//
// --tone plays a 1 kHz sine at -12 dBFS on the default output while recording; the final analysis then checks
// loopback.wav for gaps (> 5 ms below 1e-4 while the tone plays), the tone frequency (zero crossings, 1000 +- 1 Hz)
// and that the stream length matches the device's QPC timeline (|delta| < 5 ms). Exit code 0 = all checks passed.
#include <ct/audio/devices.h>
#include <ct/audio/dsp.h>
#include <ct/audio/wasapi_capture.h>
#include <ct/audio/wasapi_loopback.h>
#include <ct/audio/wasapi_render.h>
#include <ct/audio/wav.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/ring_buffer.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/win.h>

#include <objbase.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kToneAmp = 0.25118864f;   // -12 dBFS

struct ToneSource final : WasapiRender::Callback {
    double phase = 0;
    void Render(float* p, uint32_t n, int ch, int64_t) override {
        const double step = 2.0 * kPi * 1000.0 / 48000.0;
        for (uint32_t i = 0; i < n; ++i) {
            const float v = kToneAmp * static_cast<float>(std::sin(phase));
            phase += step;
            if (phase >= 2.0 * kPi) phase -= 2.0 * kPi;
            for (int c = 0; c < ch; ++c) p[i * ch + c] = v;
        }
    }
};

// Capture-thread side: copy into a lock-free ring; the main thread writes the WAV file.
struct RingSink final : AudioSink {
    AudioRing ring;
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> overflow{0};
    std::atomic<long> errors{0};
    bool OnAudio(uint32_t, const AudioPacket& p) override {
        packets.fetch_add(1, std::memory_order_relaxed);
        if (ring.Write(p.data, p.frames) != p.frames) {
            overflow.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    void OnAudioError(uint32_t, HRESULT) override { errors.fetch_add(1, std::memory_order_relaxed); }
};

struct Stream {
    const char* name = "";
    RingSink sink;
    WavWriter wav;
    std::vector<float> scratch;
    void Drain() {
        for (;;) {
            const size_t n = sink.ring.Read(scratch.data(), scratch.size() / 2);
            if (n == 0) break;
            wav.Write(scratch.data(), n);
        }
    }
};

uint64_t ProcessCpu100ns() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    const auto v = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return v(k) + v(u);
}

struct Analysis {
    bool tone_found = false;
    int64_t tone_first = 0, tone_last = 0;
    int gaps = 0;
    int64_t longest_quiet = 0;
    double freq_hz = 0;
    double peak_db = -120;
};

Analysis Analyze(const std::string& path) {
    Analysis a;
    WavReader r;
    if (!r.Open(path) || r.Channels() != 2) return a;
    std::vector<float> all(r.Frames() * 2);
    r.Read(all.data(), r.Frames());
    const int64_t frames = static_cast<int64_t>(r.Frames());
    auto L = [&](int64_t i) { return all[static_cast<size_t>(2 * i)]; };
    // Tone region: first/last frame above 0.01.
    a.tone_first = -1;
    for (int64_t i = 0; i < frames; ++i)
        if (std::fabs(L(i)) > 0.01f) {
            a.tone_first = i;
            break;
        }
    for (int64_t i = frames - 1; i >= 0; --i)
        if (std::fabs(L(i)) > 0.01f) {
            a.tone_last = i;
            break;
        }
    if (a.tone_first < 0 || a.tone_last - a.tone_first < 48000) return a;
    a.tone_found = true;
    // Gaps: runs of > 5 ms (240 frames) with |x| < 1e-4 on both channels while the tone plays.
    int64_t run = 0;
    float peak = 0;
    for (int64_t i = a.tone_first; i <= a.tone_last; ++i) {
        const float l = std::fabs(L(i)), rr = std::fabs(all[static_cast<size_t>(2 * i + 1)]);
        peak = std::fmax(peak, std::fmax(l, rr));
        if (l < 1e-4f && rr < 1e-4f) {
            ++run;
            if (run == 241) ++a.gaps;
            if (run > a.longest_quiet) a.longest_quiet = run;
        } else {
            run = 0;
        }
    }
    a.peak_db = 20.0 * std::log10(std::fmax(peak, 1e-6f));
    // Frequency from rising zero crossings (linear interpolation), skipping 100 ms at both ends.
    double first_cross = -1, last_cross = -1;
    int64_t crossings = 0;
    for (int64_t i = a.tone_first + 4800; i < a.tone_last - 4800; ++i) {
        const float x0 = L(i), x1 = L(i + 1);
        if (x0 < 0.0f && x1 >= 0.0f) {
            const double t = static_cast<double>(i) + x0 / (x0 - x1);
            if (first_cross < 0) first_cross = t;
            last_cross = t;
            ++crossings;
        }
    }
    if (crossings > 1) a.freq_hz = static_cast<double>(crossings - 1) * 48000.0 / (last_cross - first_cross);
    return a;
}

void Usage() {
    std::printf("usage: audio_bench --seconds N [--tone] [--mic] [--out dir]\n");
}

} // namespace

int main(int argc, char** argv) {
    int seconds = 10;
    bool tone = false, mic = false;
    std::string out_dir = ".";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--tone") tone = true;
        else if (a == "--mic") mic = true;
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else {
            Usage();
            return 2;
        }
    }
    if (seconds < 1) seconds = 1;
    if (!fs::CreateDirectories(out_dir)) {
        std::printf("cannot create %s\n", out_dir.c_str());
        return 2;
    }
    Log::Init(out_dir.c_str());
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    dsp::Init();

    AudioDevices devices;
    if (FAILED(devices.Init())) {
        std::printf("audio device enumeration failed\n");
        return 3;
    }
    if (mic && devices.DefaultId(true).empty()) {
        std::printf("no capture endpoint; --mic ignored\n");
        mic = false;
    }

    WasapiRender tone_out;
    ToneSource tone_src;
    if (tone) {
        if (FAILED(tone_out.Init(L"", 40)) || !tone_out.Start(tone_src)) {
            std::printf("could not start the test tone\n");
            return 3;
        }
        Sleep(200);   // the tone is already playing when recording starts
    }

    Stream sys, mic_stream;
    sys.name = "loopback";
    mic_stream.name = "mic";
    for (Stream* s : {&sys, &mic_stream}) {
        s->sink.ring.Init(4 * 48000, 2);
        s->scratch.resize(9600 * 2);
    }
    const std::string sys_path = PathJoin(out_dir, "loopback.wav");
    const std::string mic_path = PathJoin(out_dir, "mic.wav");
    if (!sys.wav.Open(sys_path, 48000, 2, true) || (mic && !mic_stream.wav.Open(mic_path, 48000, 2, true))) {
        std::printf("cannot write WAV files in %s\n", out_dir.c_str());
        return 3;
    }

    WasapiLoopback loop;
    WasapiCapture capture;
    CaptureConfig sys_cfg;
    sys_cfg.stream_id = 1;
    CaptureConfig mic_cfg;
    mic_cfg.stream_id = 0;
    HRESULT hr = loop.Init(sys_cfg, devices);
    if (FAILED(hr)) {
        std::printf("loopback init failed: %s\n", HrToString(hr).c_str());
        return 3;
    }
    if (mic && FAILED(hr = capture.Init(mic_cfg, devices))) {
        std::printf("mic init failed: %s\n", HrToString(hr).c_str());
        return 3;
    }
    std::printf("loopback: %s%s%s\n", loop.DeviceName().c_str(), mic ? "   mic: " : "",
                mic ? capture.DeviceName().c_str() : "");
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const double ncpu = si.dwNumberOfProcessors;

    const int64_t epoch = Clock::QpcNow();
    const uint64_t cpu0 = ProcessCpu100ns();
    if (!loop.Start(epoch, sys.sink) || (mic && !capture.Start(epoch, mic_stream.sink))) {
        std::printf("start failed\n");
        return 3;
    }
    std::printf("   t  packets  inserted  dropped  drift_ms  max|drift|  peak_L  peak_R  rms_L    cpu%%(machine) cpu%%(core)\n");
    uint64_t last_packets = 0;
    uint64_t last_cpu = cpu0;
    int64_t last_tick = epoch;
    double max_abs_drift = 0;
    const int64_t freq = Clock::QpcFrequency();
    for (int sec = 1; sec <= seconds; ++sec) {
        const int64_t due = epoch + static_cast<int64_t>(sec) * freq;
        while (Clock::QpcNow() < due) {
            Sleep(50);
            sys.Drain();
            if (mic) mic_stream.Drain();
            const double d = std::fabs(loop.GetStats().drift_ms);
            if (d > max_abs_drift) max_abs_drift = d;
        }
        const CaptureStats st = loop.GetStats();
        const uint64_t packets = sys.sink.packets.load();
        const uint64_t cpu = ProcessCpu100ns();
        const int64_t now = Clock::QpcNow();
        const double wall_100ns = static_cast<double>(now - last_tick) * 1e7 / freq;
        const double core_pct = 100.0 * static_cast<double>(cpu - last_cpu) / wall_100ns;
        std::printf("%4d  %7llu  %8llu  %7llu  %8.3f  %10.3f  %6.1f  %6.1f  %6.1f   %8.3f      %8.3f\n", sec,
                    static_cast<unsigned long long>(packets - last_packets),
                    static_cast<unsigned long long>(st.frames_inserted),
                    static_cast<unsigned long long>(st.frames_dropped), st.drift_ms, max_abs_drift,
                    loop.Levels().PeakDb(0), loop.Levels().PeakDb(1), loop.Levels().RmsDb(0), core_pct / ncpu,
                    core_pct);
        if (mic) {
            const CaptureStats ms = capture.GetStats();
            std::printf("      mic: packets %llu inserted %llu dropped %llu drift %.3f ms peak %.1f dB\n",
                        static_cast<unsigned long long>(mic_stream.sink.packets.load()),
                        static_cast<unsigned long long>(ms.frames_inserted),
                        static_cast<unsigned long long>(ms.frames_dropped), ms.drift_ms, capture.Levels().PeakDb(0));
        }
        std::fflush(stdout);
        last_packets = packets;
        last_cpu = cpu;
        last_tick = now;
    }
    const int64_t stop_qpc = Clock::QpcNow();
    loop.Stop();
    if (mic) capture.Stop();
    const uint64_t cpu1 = ProcessCpu100ns();
    if (tone) tone_out.Stop();
    sys.Drain();
    mic_stream.Drain();
    sys.wav.Close();
    if (mic) mic_stream.wav.Close();

    const CaptureStats st = loop.GetStats();
    const double elapsed_s = static_cast<double>(stop_qpc - epoch) / freq;
    const double cpu_core_pct = 100.0 * static_cast<double>(cpu1 - cpu0) / (elapsed_s * 1e7);
    const double cpu_machine_pct = cpu_core_pct / ncpu;
    const uint64_t frames = sys.wav.FramesWritten();
    const double frames_ms = static_cast<double>(frames) / 48.0;
    const double device_ms = static_cast<double>(st.last_device_end_us) / 1000.0;
    const double delta_ms = frames_ms - device_ms;

    std::printf("\n%s: %llu frames (%.3f s), device timeline %.3f s, stop at %.3f s\n", sys_path.c_str(),
                static_cast<unsigned long long>(frames), frames_ms / 1000.0, device_ms / 1000.0, elapsed_s);
    std::printf("  leading pad %.1f ms, inserted %llu, dropped %llu, discontinuities %llu, reinits %llu, ring overflow %llu, "
                "errors %ld\n",
                static_cast<double>(st.leading_pad_frames) / 48.0, static_cast<unsigned long long>(st.frames_inserted),
                static_cast<unsigned long long>(st.frames_dropped), static_cast<unsigned long long>(st.discontinuities),
                static_cast<unsigned long long>(st.reinits), static_cast<unsigned long long>(sys.sink.overflow.load()),
                sys.sink.errors.load());
    std::printf("  stream end vs last device frame (QPC): %+.3f ms   [stream end vs stop time: %+.3f ms, presentation"
                " offset + poll latency]\n", delta_ms, frames_ms - elapsed_s * 1000.0);
    std::printf("  max |drift| %.3f ms; process CPU %.3f %% of the machine (%d logical CPUs) = %.3f %% of one core\n",
                max_abs_drift, cpu_machine_pct, static_cast<int>(ncpu), cpu_core_pct);

    bool ok = std::fabs(delta_ms) < 5.0 && cpu_machine_pct < 1.0 && sys.sink.overflow.load() == 0;
    if (tone) {
        const Analysis a = Analyze(sys_path);
        if (!a.tone_found) {
            std::printf("  tone: NOT FOUND in loopback.wav (is the output muted or another app exclusive?)\n");
            ok = false;
        } else {
            std::printf("  tone: %.3f s .. %.3f s, peak %.2f dBFS, gaps %d (longest quiet run %.2f ms), frequency %.4f Hz\n",
                        a.tone_first / 48000.0, a.tone_last / 48000.0, a.peak_db, a.gaps, a.longest_quiet / 48.0,
                        a.freq_hz);
            ok = ok && a.gaps == 0 && std::fabs(a.freq_hz - 1000.0) <= 1.0;
        }
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    devices.Shutdown();
    CoUninitialize();
    Log::Shutdown();
    return ok ? 0 : 1;
}
