// player_demo: standalone check of the P06 playback stack (MediaCache + AudioEngine + PlaybackClock + Nv12Blit).
//
//   player_demo <file|bundle.ctrec> [--stats] [--seconds N] [--rate R] [--loop]   window: Space play/pause,
//        Left/Right +-1 frame, Shift+Left/Right +-1 s, J/K/L reverse/pause/forward (repeat = faster), Home, click = seek
//   player_demo <file> --count-frames      decode everything, print the count and the first/last pts
//   player_demo <file> --seek-bench [N]    N (default 50) random seeks, print p50/p95 seek-to-exact-frame ms
//   player_demo <file> --avsync [--audio-stream K]   clap-test analysis (docs/06 section 4)
//
// --seconds N plays for N s and prints frames shown/late, clock vs wall, process CPU, then exits (used for PROGRESS.md).
// Overlay: the window title shows pts, clock, rate, cache MB, late frames (4 Hz).
#define CT_LOG_CATEGORY "player"
#include <ct/audio/devices.h>
#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/audio_decoder.h>
#include <ct/media/media_info.h>
#include <ct/media/video_decoder.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/nv12_blit.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>
#include <ct/platform/window.h>
#include <ct/playback/cache_budget.h>
#include <ct/playback/media_cache.h>
#include <ct/playback/player.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace ct;

namespace {

struct Options {
    std::string path;
    bool stats = false, count_frames = false, seek_bench = false, avsync = false, loop = false, debug_layer = false;
    int seek_count = 50;
    int audio_stream = -1;
    double seconds = 0;
    double rate = 1.0;
};

double CpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto t = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 1e7; };
    return t(k) + t(u);
}

int Cores() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<int>(si.dwNumberOfProcessors);
}

std::string ResolvePath(const std::string& p) {
    // A .ctrec bundle is a folder with screen.mp4 inside.
    const DWORD a = GetFileAttributesW(Utf8ToWide(p).c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return PathJoin(p, "screen.mp4");
    return p;
}

// --- --count-frames ------------------------------------------------------------------------------------------------

int CountFrames(platform::D3DDevice& dev, const Options& opt) {
    media::VideoDecoder dec;
    media::VideoDecoder::Config cfg;
    cfg.path = opt.path;
    if (FAILED(dec.Init(dev, cfg))) {
        std::fprintf(stderr, "cannot decode %s\n", opt.path.c_str());
        return 2;
    }
    Stopwatch sw;
    sw.Start();
    int64_t n = 0;
    TimeUs first = -1, last = -1;
    for (;;) {
        media::DecodedFrame f;
        const HRESULT hr = dec.ReadFrame(f, INT64_MIN);
        if (hr != S_OK) {
            if (FAILED(hr)) std::fprintf(stderr, "ReadFrame: %s\n", HrToString(hr).c_str());
            break;
        }
        if (first < 0) first = f.pts_us;
        last = f.pts_us;
        dec.ReleaseFrame(f.pool_index);
        ++n;
    }
    std::printf("frames %lld  first_pts %lld us  last_pts %lld us  size %dx%d  fps %d/%d  %s  (%.0f ms, %.0f fps)\n",
                static_cast<long long>(n), static_cast<long long>(first), static_cast<long long>(last), dec.Size().w, dec.Size().h,
                dec.FpsNum(), dec.FpsDen(), dec.IsHardware() ? "hw" : "sw", sw.ElapsedMs(), n * 1000.0 / std::max(sw.ElapsedMs(), 1.0));
    return 0;
}

// --- --seek-bench --------------------------------------------------------------------------------------------------

int SeekBench(platform::D3DDevice& dev, const Options& opt) {
    playback::MediaCache cache;
    if (FAILED(cache.Open(dev, opt.path)) || !cache.HasVideo()) {
        std::fprintf(stderr, "cannot open video %s\n", opt.path.c_str());
        return 2;
    }
    const TimeUs dur = cache.DurationUs();
    std::mt19937 rng(20261017);
    std::vector<double> ms;
    int failures = 0;
    // Warm-up: the first request also creates the decoder's output surfaces.
    cache.WaitForFrame(0, 5000);
    for (int i = 0; i < opt.seek_count; ++i) {
        const int64_t frames = FrameIndexAt(dur - 1, cache.FpsNum(), cache.FpsDen());
        const int64_t f = std::uniform_int_distribution<int64_t>(0, std::max<int64_t>(frames - 1, 0))(rng);
        const TimeUs t = FrameTimeUs(f, cache.FpsNum(), cache.FpsDen()) + 500;
        Stopwatch sw;
        sw.Start();
        if (!cache.WaitForFrame(t, 5000)) ++failures;
        ms.push_back(sw.ElapsedMs());
    }
    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * (sorted.size() - 1) + 0.5))]; };
    const playback::MediaCache::Stats st = cache.GetStats();
    std::printf("seek-bench %d seeks: p50 %.1f ms  p95 %.1f ms  max %.1f ms  (decoder seeks %llu, cached frames %zu, %zu MB, %d timeouts)\n",
                opt.seek_count, pct(0.5), pct(0.95), sorted.back(), static_cast<unsigned long long>(st.seeks), st.cached_frames,
                st.cache_bytes >> 20, failures);
    return failures == 0 ? 0 : 1;
}

// --- --avsync ------------------------------------------------------------------------------------------------------

struct Biquad {   // RBJ band-pass (constant 0 dB peak gain)
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void BandPass(double f0, double q, double rate) {
        const double w = 2 * 3.14159265358979323846 * f0 / rate, alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        b0 = alpha / a0;
        b1 = 0;
        b2 = -alpha / a0;
        a1 = -2 * std::cos(w) / a0;
        a2 = (1 - alpha) / a0;
    }
    double Run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// Rising edges of `v` above `threshold` with a refractory period; returns the times. Video frames are discrete, so the
// onset is the first frame above the threshold (interpolate = false); the audio envelope (1 ms steps) is interpolated.
// A series that starts above the threshold counts as an onset at its first sample.
std::vector<double> Onsets(const std::vector<double>& t, const std::vector<double>& v, double threshold, double refractory_s,
                           bool interpolate) {
    std::vector<double> out;
    double last = -1e9;
    if (!v.empty() && v[0] >= threshold) {
        out.push_back(t[0]);
        last = t[0];
    }
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] >= threshold && v[i - 1] < threshold && t[i] - last > refractory_s) {
            const double f = interpolate ? (threshold - v[i - 1]) / (v[i] - v[i - 1]) : 1.0;
            out.push_back(t[i - 1] + f * (t[i] - t[i - 1]));
            last = out.back();
        }
    return out;
}

// Beep detector: two cascaded 2 kHz band-passes (Q 8) on the mono mix, RMS per 1 ms (48 frames), onset where the
// envelope crosses half of its maximum.
struct BeepDetector {
    Biquad b1, b2;
    double acc = 0;
    int in_win = 0;
    BeepDetector() {
        b1.BandPass(2000, 8, 48000);
        b2.BandPass(2000, 8, 48000);
    }
    // Returns true when a 1 ms window completed (rms set).
    bool Push(double x, double& rms) {
        const double y = b2.Run(b1.Run(x));
        acc += y * y;
        if (++in_win < 48) return false;
        rms = std::sqrt(acc / 48);
        acc = 0;
        in_win = 0;
        return true;
    }
};

// The detector's own delay: run a synthetic 20 ms 2 kHz burst starting at exactly 100 ms through it.
double DetectorDelaySeconds() {
    BeepDetector d;
    std::vector<double> t, e;
    for (int i = 0; i < 48000 / 2; ++i) {
        const double x = (i >= 4800 && i < 4800 + 960) ? 0.25 * std::sin(2 * 3.14159265358979323846 * 2000.0 * (i - 4800) / 48000.0) : 0.0;
        double rms = 0;
        if (d.Push(x, rms)) {
            t.push_back((i - 47) / 48000.0);
            e.push_back(rms);
        }
    }
    double emax = 0;
    for (double v : e) emax = std::max(emax, v);
    const std::vector<double> on = Onsets(t, e, emax * 0.5, 0.5, true);
    return on.empty() ? 0.0 : on[0] - 0.1;
}

int AvSync(platform::D3DDevice& dev, const Options& opt) {
    media::MediaInfo info;
    if (FAILED(media::MediaInfo::Load(opt.path, info)) || info.FirstVideo() < 0 || info.AudioStreams().empty()) {
        std::fprintf(stderr, "--avsync needs a file with video and audio: %s\n", opt.path.c_str());
        return 2;
    }
    // Video: mean luma per frame (every 8th pixel of the Y plane through a staging copy).
    media::VideoDecoder dec;
    media::VideoDecoder::Config vc;
    vc.path = opt.path;
    if (FAILED(dec.Init(dev, vc))) return 2;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = static_cast<UINT>(dec.Size().w);
    sd.Height = static_cast<UINT>(dec.Size().h);
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_NV12;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev.Device()->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) return 2;
    std::vector<double> vt, vl;
    for (;;) {
        media::DecodedFrame f;
        if (dec.ReadFrame(f, INT64_MIN) != S_OK) break;
        double sum = 0;
        int count = 0;
        {
            platform::D3DDevice::Lock lock(dev);
            dev.Context()->CopyResource(staging.Get(), f.nv12);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(dev.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                const uint8_t* y = static_cast<const uint8_t*>(m.pData);
                for (int r = 0; r < f.size.h; r += 8)
                    for (int x = 0; x < f.size.w; x += 8) {
                        sum += y[static_cast<size_t>(r) * m.RowPitch + x];
                        ++count;
                    }
                dev.Context()->Unmap(staging.Get(), 0);
            }
        }
        dec.ReleaseFrame(f.pool_index);
        vt.push_back(f.pts_us / 1e6);
        vl.push_back(count ? sum / count : 0);
    }
    if (vl.size() < 2) return 2;
    std::vector<double> sorted_l = vl;
    std::sort(sorted_l.begin(), sorted_l.end());
    const double median = sorted_l[sorted_l.size() / 2], peak = sorted_l.back();
    const std::vector<double> flashes = Onsets(vt, vl, (median + peak) / 2, 0.5, false);

    // Audio: 2 kHz band-pass (two cascaded RBJ sections, Q 8) envelope per 1 ms.
    const std::vector<int> streams = info.AudioStreams();
    const int stream = opt.audio_stream >= 0 && opt.audio_stream < static_cast<int>(streams.size()) ? streams[opt.audio_stream] : streams[0];
    media::AudioDecoder ad;
    media::AudioDecoder::Config acfg;
    acfg.path = opt.path;
    acfg.stream_index = stream;
    if (FAILED(ad.Init(acfg))) return 2;
    BeepDetector det;
    const double det_delay = DetectorDelaySeconds();
    std::vector<double> at, ae;
    std::vector<float> buf(4800 * 2);
    int64_t frame = 0;
    TimeUs first_pts = -1;
    for (;;) {
        uint32_t n = 0;
        TimeUs pts = 0;
        if (ad.Read(buf.data(), 4800, n, pts) != S_OK) break;
        if (first_pts < 0) first_pts = pts;
        for (uint32_t i = 0; i < n; ++i, ++frame) {
            double rms = 0;
            if (det.Push(0.5 * (buf[2 * i] + buf[2 * i + 1]), rms)) {
                at.push_back(first_pts / 1e6 + (frame - 47) / 48000.0 - det_delay);
                ae.push_back(rms);
            }
        }
    }
    double emax = 0;
    for (double e : ae) emax = std::max(emax, e);
    const std::vector<double> beeps = Onsets(at, ae, emax * 0.5, 0.5, true);

    // Pair each flash with the nearest beep within +-250 ms.
    std::printf("avsync: %zu flashes (luma median %.1f peak %.1f), %zu beeps (audio stream %d)\n", flashes.size(), median, peak,
                beeps.size(), stream);
    std::printf("detector delay %.2f ms (compensated); events in the first 250 ms are skipped (encoder priming)\n", det_delay * 1000.0);
    std::vector<double> ft, off;
    for (double fl : flashes) {
        if (fl < 0.25) continue;
        double best = 1e9;
        for (double b : beeps)
            if (std::fabs(b - fl) < std::fabs(best)) best = b - fl;
        if (std::fabs(best) > 0.25) {
            std::printf("  flash %8.3f s: no beep\n", fl);
            continue;
        }
        ft.push_back(fl);
        off.push_back(best * 1000.0);
        std::printf("  flash %8.3f s  beep %+7.2f ms\n", fl, best * 1000.0);
    }
    if (off.size() < 2) {
        std::printf("avsync: FAIL (fewer than 2 pairs)\n");
        return 1;
    }
    // Drift: least-squares slope of offset over time, times the span.
    double mt = 0, mo = 0;
    for (size_t i = 0; i < ft.size(); ++i) {
        mt += ft[i];
        mo += off[i];
    }
    mt /= ft.size();
    mo /= off.size();
    double num = 0, den = 0, worst = 0;
    for (size_t i = 0; i < ft.size(); ++i) {
        num += (ft[i] - mt) * (off[i] - mo);
        den += (ft[i] - mt) * (ft[i] - mt);
        worst = std::max(worst, std::fabs(off[i]));
    }
    const double slope = den > 0 ? num / den : 0;   // ms per s
    const double drift = slope * (ft.back() - ft.front());
    const bool pass = worst < 20.0 && std::fabs(drift) < 20.0;
    std::printf("avsync: %zu pairs  mean offset %+.2f ms  max |offset| %.2f ms  drift %+.2f ms over %.1f s  -> %s\n", off.size(), mo,
                worst, drift, ft.back() - ft.front(), pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// --- window mode ---------------------------------------------------------------------------------------------------

struct App {
    platform::D3DDevice* dev = nullptr;
    platform::Window win;
    platform::SwapChain sc;
    platform::Nv12Blit blit;
    playback::Player player;
    double jkl_rate = 1.0;
    bool dirty = true;
};

LRESULT Handler(void* user, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    App& app = *static_cast<App*>(user);
    if (msg == WM_KEYDOWN) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        TimeUs now = 0;
        playback::FrameRef f;
        app.player.Tick(f, now);
        switch (wp) {
        case VK_SPACE: app.player.TogglePlay(); break;
        case VK_LEFT: shift ? app.player.Seek(now - 1'000'000) : app.player.StepFrames(-1); break;
        case VK_RIGHT: shift ? app.player.Seek(now + 1'000'000) : app.player.StepFrames(1); break;
        case VK_HOME: app.player.Seek(0); break;
        case 'K': app.player.Pause(); break;
        case 'L':
            app.jkl_rate = app.player.Playing() && app.player.Rate() > 0 ? std::min(app.player.Rate() * 2, 4.0) : 1.0;
            app.player.SetRate(app.jkl_rate);
            app.player.Play();
            break;
        case 'J':
            app.jkl_rate = app.player.Playing() && app.player.Rate() < 0 ? std::max(app.player.Rate() * 2, -4.0) : -1.0;
            app.player.SetRate(app.jkl_rate);
            app.player.Play();
            break;
        case VK_ESCAPE: DestroyWindow(hwnd); break;
        default: break;
        }
        app.dirty = true;
        handled = true;
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        const SizeI c = app.win.ClientSizePx();
        const double x = static_cast<double>(static_cast<short>(LOWORD(lp))) / std::max(c.w, 1);
        app.player.Seek(static_cast<TimeUs>(x * app.player.Duration()));
        app.dirty = true;
    }
    (void)wp;
    return 0;
}

void Draw(App& app, const playback::FrameRef& f) {
    platform::D3DDevice::Lock lock(*app.dev);
    ID3D11DeviceContext* ctx = app.dev->Context();
    ID3D11RenderTargetView* rtv = app.sc.Rtv();
    const float black[4] = {0.06f, 0.06f, 0.07f, 1};
    ctx->ClearRenderTargetView(rtv, black);
    if (f.y && f.uv && f.size.w > 0) {
        const SizeI c = app.sc.Size();
        const float s = std::min(static_cast<float>(c.w) / f.size.w, static_cast<float>(c.h) / f.size.h);
        const float w = f.size.w * s, h = f.size.h * s;
        D3D11_VIEWPORT vp{(c.w - w) * 0.5f, (c.h - h) * 0.5f, w, h, 0, 1};
        app.blit.Draw(ctx, f.y, f.uv, f.size, rtv, vp, s < 0.5f);
    }
}

int RunWindow(platform::D3DDevice& dev, audio::AudioDevices& devices, const Options& opt) {
    App app;
    app.dev = &dev;
    if (FAILED(app.player.Open(dev, devices, opt.path))) {
        std::fprintf(stderr, "cannot open %s\n", opt.path.c_str());
        return 2;
    }
    const media::MediaInfo& info = app.player.Info();
    std::printf("%s: %.3f s, %zu streams, audio clock %s\n", opt.path.c_str(), info.duration_us / 1e6, info.streams.size(),
                app.player.HasAudio() ? app.player.Engine().DeviceName().c_str() : "none (QPC)");
    platform::Window::Desc desc;
    desc.class_name = L"CamtesiaPlayerDemo";
    desc.title = L"player_demo";
    desc.w_dip = 1280;
    desc.h_dip = 720;
    desc.quit_on_destroy = true;
    if (!app.win.Create(desc, &Handler, &app) || FAILED(app.sc.Init(dev, app.win.Hwnd())) || FAILED(app.blit.Init(dev))) return 3;
    app.win.SetDarkTitleBar(true);
    app.win.Show();
    if (opt.rate != 1.0) app.player.SetRate(opt.rate);
    if (opt.seconds > 0) app.player.Play();

    const int64_t q_start = Clock::QpcNow();
    const double cpu_start = CpuSeconds();
    int64_t q_title = 0;
    int64_t q_stats = q_start;
    TimeUs clock_start = -1;
    int64_t q_clock_start = 0;
    bool running = true;
    while (running) {
        HANDLE h = app.sc.WaitableObject();
        const bool animate = app.player.Playing() || app.dirty;
        const DWORD wait = MsgWaitForMultipleObjectsEx(h ? 1 : 0, &h, animate ? 100 : INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        if (app.win.ConsumeResize()) {
            const SizeI c = app.win.ClientSizePx();
            if (c.w > 0 && c.h > 0) app.sc.Resize(c);
            app.dirty = true;
        }
        if (wait != WAIT_OBJECT_0 && !(animate && wait == WAIT_TIMEOUT)) continue;   // input only: render on the next vsync
        playback::FrameRef f;
        TimeUs now = 0;
        app.player.Tick(f, now);
        if (clock_start < 0 && app.player.Playing() && now > 0) {
            clock_start = now;
            q_clock_start = Clock::QpcNow();
        }
        Draw(app, f);
        app.sc.Present(true);
        app.dirty = !app.player.Playing() && !(f.pts_us <= now && now < f.pts_us + f.duration_us + 1000);
        const int64_t qn = Clock::QpcNow();
        if (qn - q_title > Clock::QpcFrequency() / 4) {
            q_title = qn;
            const playback::Player::Stats st = app.player.GetStats();
            const std::wstring title = Utf8ToWide(Format("player_demo  pts %.3f  clock %.3f  %s%.2fx  cache %zu MB  shown %llu  late %llu",
                                                         f.pts_us / 1e6, now / 1e6, app.player.Playing() ? "> " : "|| ",
                                                         app.player.Rate(), st.cache_bytes >> 20,
                                                         static_cast<unsigned long long>(st.frames_shown),
                                                         static_cast<unsigned long long>(st.frames_late)));
            app.win.SetTitle(title.c_str());
        }
        if (opt.stats && qn - q_stats >= Clock::QpcFrequency()) {
            q_stats = qn;
            const playback::Player::Stats st = app.player.GetStats();
            std::printf("t %.3f  shown %llu  late %llu  cache %zu MB  seek->frame %.1f ms\n", now / 1e6,
                        static_cast<unsigned long long>(st.frames_shown), static_cast<unsigned long long>(st.frames_late),
                        st.cache_bytes >> 20, st.seek_to_frame_ms);
        }
        if (opt.seconds > 0) {
            const double wall = static_cast<double>(qn - q_start) / Clock::QpcFrequency();
            if (!app.player.Playing() && opt.loop) {
                app.player.Seek(0);
                app.player.Play();
            }
            if (wall >= opt.seconds || (!app.player.Playing() && !opt.loop)) {
                const double cpu = CpuSeconds() - cpu_start;
                const playback::Player::Stats st = app.player.GetStats();
                const double clock_wall = clock_start >= 0 ? static_cast<double>(qn - q_clock_start) / Clock::QpcFrequency() : 0;
                std::printf("played %.2f s wall: frames shown %llu, frames late %llu, clock advanced %.3f s over %.3f s, "
                            "CPU %.2f %% of the machine (%.1f %% of one core, %d cores), cache %zu MB, audio clock %d\n",
                            wall, static_cast<unsigned long long>(st.frames_shown), static_cast<unsigned long long>(st.frames_late),
                            clock_start >= 0 ? (now - clock_start) / 1e6 : 0.0, clock_wall, 100.0 * cpu / wall / Cores(),
                            100.0 * cpu / wall, Cores(), st.cache_bytes >> 20, app.player.ClockUsesAudio() ? 1 : 0);
                const playback::MediaCache::Stats cs = app.player.Cache().GetStats();
                std::printf("cache: decoded %llu, decoder seeks %llu, evictions %llu, exact hits %llu, misses %llu\n",
                            static_cast<unsigned long long>(cs.frames_decoded), static_cast<unsigned long long>(cs.seeks),
                            static_cast<unsigned long long>(cs.evictions), static_cast<unsigned long long>(cs.hits),
                            static_cast<unsigned long long>(cs.misses));
                break;
            }
        }
    }
    app.player.Close();
    app.blit.Shutdown();
    app.sc.Shutdown();
    app.win.Destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--stats") opt.stats = true;
        else if (a == "--count-frames") opt.count_frames = true;
        else if (a == "--seek-bench") {
            opt.seek_bench = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') opt.seek_count = std::max(1, std::atoi(next()));
        } else if (a == "--avsync") opt.avsync = true;
        else if (a == "--audio-stream") opt.audio_stream = std::atoi(next());
        else if (a == "--seconds") opt.seconds = std::atof(next());
        else if (a == "--rate") opt.rate = std::atof(next());
        else if (a == "--loop") opt.loop = true;
        else if (a == "--debug-layer") opt.debug_layer = true;
        else if (!a.empty() && a[0] != '-') opt.path = a;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (opt.path.empty()) {
        std::fprintf(stderr, "usage: player_demo <file> [--count-frames | --seek-bench [N] | --avsync [--audio-stream K]] "
                             "[--stats] [--seconds N] [--rate R] [--loop] [--debug-layer]\n");
        return 2;
    }
    opt.path = ResolvePath(opt.path);
    platform::Paths::Init();
    Log::Init(platform::Paths::LogsDir().c_str());
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    int rc = 0;
    {
        platform::D3DDevice dev;
        if (FAILED(dev.Init(opt.debug_layer))) {
            std::fprintf(stderr, "no D3D11 device\n");
            return 3;
        }
        audio::AudioDevices devices;
        devices.Init();
        if (opt.count_frames) rc = CountFrames(dev, opt);
        else if (opt.seek_bench) rc = SeekBench(dev, opt);
        else if (opt.avsync) rc = AvSync(dev, opt);
        else rc = RunWindow(dev, devices, opt);
        Async::Shutdown();
        devices.Shutdown();
        dev.Shutdown();
        dev.ReportLiveObjects();
    }
    CoUninitialize();
    Log::Shutdown();
    return rc;
}
