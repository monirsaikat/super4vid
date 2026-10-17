// avsync_source: the A/V sync "clap" source and a headless analyzer (docs/06 section 4).
//
//   avsync_source [--seconds 130] [--period-ms 2000] [--log events.csv]
//       A 400x400 topmost window centred on the primary monitor, black, turning white for 3 frames every period (docs/06
//       says one frame; a single 16 ms frame can be coalesced away by a 60 Hz capture under load), and a
//       2 kHz 20 ms beep through the default output scheduled to play at the same QPC. Logs per event: the nominal QPC,
//       the QPC at which the beep's first frame is predicted to play (IAudioClock mapping) and the QPC of the vsync that
//       showed the white frame (DXGI frame statistics).
//   avsync_source --analyze rec.mp4 [--log events.csv --epoch QPC] [--audio-track N] [--region x,y,w,h]
//       Decodes the recording: flash frames (mean luma of the region > 128) and beep onsets (a rise of the 2 kHz
//       amplitude over 10 ms windows, refined to the sample with the 2 kHz envelope, so other sound playing on the
//       output does not hide them) in audio track N (default: the last one),
//       pairs them, prints the offset per event (video - audio, ms) and the drift slope. With --log/--epoch it also
//       prints each stream's error against the source's logged emission time (removes the source's own vsync jitter).
//       Exit code 0 when every |offset| < 20 ms (after correction when a log is given).
// The P06 player_demo --avsync is the in-product version of the analyzer; this one exists so P04 can measure headlessly.
#include <ct/audio/wasapi_render.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/mf_runtime.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ct;
using Microsoft::WRL::ComPtr;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kWin = 400;

// ---- source --------------------------------------------------------------------------------------------------------

struct Beeper final : audio::WasapiRender::Callback {
    audio::WasapiRender* render = nullptr;
    int64_t first_qpc = 0, period_qpc = 0, freq = 0;
    int events = 0;
    std::vector<int64_t> beep_qpc;   // predicted play QPC of each beep's first frame (written once per event)
    int64_t beep_left = 0;           // frames of the current beep still to write
    double phase = 0;
    void Render(float* out, uint32_t frames, int ch, int64_t pos) override {
        int64_t dev_frames = 0, clk_qpc = 0;
        const bool clock = render && SUCCEEDED(render->GetClock(dev_frames, clk_qpc)) && clk_qpc != 0;
        for (uint32_t i = 0; i < frames; ++i) {
            float v = 0;
            if (beep_left == 0 && clock) {
                const int64_t f = pos + i;
                const int64_t play = clk_qpc + (f - dev_frames) * freq / 48000;
                const int k = static_cast<int>(beep_qpc.size());
                if (k < events && play >= first_qpc + k * period_qpc) {
                    beep_qpc.push_back(play);
                    beep_left = 960;
                    phase = 0;
                }
            }
            if (beep_left > 0) {
                v = 0.5f * static_cast<float>(std::sin(phase));
                phase += 2.0 * kPi * 2000.0 / 48000.0;
                --beep_left;
            }
            for (int c = 0; c < ch; ++c) out[i * ch + c] = v;
        }
    }
};

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

int RunSource(int seconds, int period_ms, const std::string& log_path) {
    platform::D3DDevice dev;
    if (FAILED(dev.Init(false))) return 1;
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int x = (mi.rcMonitor.left + mi.rcMonitor.right - kWin) / 2, y = (mi.rcMonitor.top + mi.rcMonitor.bottom - kWin) / 2;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CtAvsyncSource";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                wc.lpszClassName, L"avsync_source", WS_POPUP, x, y, kWin, kWin, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    platform::SwapChain sc;
    if (FAILED(sc.Init(dev, hwnd))) return 1;
    ComPtr<IDXGISwapChain> dxgi_sc;
    {
        // SwapChain exposes no IDXGISwapChain getter; frame statistics come from the RTV's resource's parent swapchain.
        ComPtr<ID3D11Resource> res;
        sc.Rtv()->GetResource(res.GetAddressOf());
        ComPtr<IDXGISurface> surf;
        res.As(&surf);
        if (surf) surf->GetParent(IID_PPV_ARGS(dxgi_sc.GetAddressOf()));
    }
    const int64_t freq = Clock::QpcFrequency();
    const int64_t start = Clock::QpcNow();
    const int64_t period = freq * period_ms / 1000;
    const int events = static_cast<int>(static_cast<int64_t>(seconds) * 1000 / period_ms) - 1;
    Beeper beeper;
    audio::WasapiRender render;
    beeper.render = &render;
    beeper.first_qpc = start + period;
    beeper.period_qpc = period;
    beeper.freq = freq;
    beeper.events = events;
    beeper.beep_qpc.reserve(static_cast<size_t>(events) + 1);
    if (FAILED(render.Init(L"", 40, "avsync-out")) || !render.Start(beeper)) {
        std::fprintf(stderr, "audio output failed\n");
        return 1;
    }
    std::printf("avsync_source: window (%d,%d) %dx%d on the primary monitor, %d events every %d ms, start qpc %lld\n", x, y, kWin,
                kWin, events, period_ms, static_cast<long long>(start));
    std::fflush(stdout);
    std::vector<int64_t> present_qpc, display_qpc;
    int next = 0;
    UINT white_present = 0;
    bool pending = false;
    int hold = 0;
    const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};
    while (Clock::QpcNow() < start + freq * seconds) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        WaitForSingleObjectEx(sc.WaitableObject(), 100, FALSE);
        if (pending && dxgi_sc) {
            DXGI_FRAME_STATISTICS st{};
            const HRESULT hr_st = dxgi_sc->GetFrameStatistics(&st);
            if (SUCCEEDED(hr_st) && st.PresentCount >= white_present) {
                // SyncQPCTime is the vsync that showed present PresentCount: exact only when it is our white frame.
                display_qpc.push_back(st.PresentCount == white_present ? st.SyncQPCTime.QuadPart : 0);
                pending = false;
            } else if (FAILED(hr_st) && Clock::QpcNow() - present_qpc.back() > freq / 5) {
                display_qpc.push_back(0);   // no statistics (disjoint): the analyzer uses present + median latency
                pending = false;
            }
        }
        // Present white in the frame that will be shown at the vsync nearest to the event time (about one refresh
        // after this wait returns).
        const int64_t now = Clock::QpcNow();
        const bool flash = next < events && !pending && hold == 0 && now + freq / 120 >= start + (next + 1) * period;
        {
            platform::D3DDevice::Lock lock(dev);
            dev.Context()->ClearRenderTargetView(sc.Rtv(), flash || hold > 0 ? white : black);
        }
        if (FAILED(sc.Present(true))) break;
        if (hold > 0) --hold;
        if (flash) {
            present_qpc.push_back(now);
            if (dxgi_sc) dxgi_sc->GetLastPresentCount(&white_present);
            pending = dxgi_sc != nullptr;
            if (!dxgi_sc) display_qpc.push_back(0);
            hold = 2;   // white for 3 frames: a single frame can be coalesced away by the 60 Hz capture under load
            ++next;
        }
    }
    render.Stop();
    sc.Shutdown();
    DestroyWindow(hwnd);
    dev.Shutdown();
    if (!log_path.empty()) {
        FILE* f = std::fopen(log_path.c_str(), "w");
        if (f) {
            std::fprintf(f, "# qpc_frequency %lld region %d,%d,%d,%d\nevent,nominal_qpc,beep_qpc,present_qpc,display_qpc\n",
                         static_cast<long long>(freq), x - mi.rcMonitor.left, y - mi.rcMonitor.top, kWin, kWin);
            for (int k = 0; k < next; ++k)
                std::fprintf(f, "%d,%lld,%lld,%lld,%lld\n", k, static_cast<long long>(start + (k + 1) * period),
                             static_cast<long long>(k < static_cast<int>(beeper.beep_qpc.size()) ? beeper.beep_qpc[static_cast<size_t>(k)] : 0),
                             static_cast<long long>(present_qpc[static_cast<size_t>(k)]),
                             static_cast<long long>(k < static_cast<int>(display_qpc.size()) ? display_qpc[static_cast<size_t>(k)] : 0));
            std::fclose(f);
        }
    }
    std::printf("avsync_source: %d flashes, %zu beeps\n", next, beeper.beep_qpc.size());
    return 0;
}

// ---- analyzer ------------------------------------------------------------------------------------------------------

std::vector<DWORD> AudioReaderStreams(IMFSourceReader* r) {   // file order (the MPEG-4 source lists tracks reversed)
    std::vector<DWORD> v;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(r->GetNativeMediaType(i, 0, t.GetAddressOf()))) break;
        GUID major{};
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio) v.insert(v.begin(), i);
    }
    return v;
}

bool FlashTimes(const std::string& path, RectI region, std::vector<int64_t>& flashes, int64_t& frames) {
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(Utf8ToWide(path).c_str(), nullptr, r.GetAddressOf()))) return false;
    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    r->SetStreamSelection(vs, TRUE);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(t.GetAddressOf());
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(r->SetCurrentMediaType(vs, nullptr, t.Get()))) return false;
    ComPtr<IMFMediaType> cur;
    r->GetCurrentMediaType(vs, cur.GetAddressOf());
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    const LONG stride = static_cast<LONG>(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, w));
    const RectI box = region.Intersect(RectI{0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)});
    bool was_white = false;
    frames = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample(vs, 0, nullptr, &flags, &ts, s.GetAddressOf()))) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        ++frames;
        ComPtr<IMFMediaBuffer> b;
        s->ConvertToContiguousBuffer(b.GetAddressOf());
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(b->Lock(&p, nullptr, &len))) return false;
        double sum = 0;
        int n = 0;
        for (int yy = box.y; yy < box.Bottom(); yy += 8)
            for (int xx = box.x; xx < box.Right(); xx += 8) {
                sum += p[static_cast<size_t>(yy) * stride + xx];
                ++n;
            }
        b->Unlock();
        const bool white = n && sum / n > 128;
        if (white && !was_white) flashes.push_back(ts / 10);
        was_white = white;
    }
    return true;
}

bool BeepTimes(const std::string& path, int track, std::vector<int64_t>& beeps, int64_t& first_us) {
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(Utf8ToWide(path).c_str(), nullptr, r.GetAddressOf()))) return false;
    std::vector<DWORD> audio = AudioReaderStreams(r.Get());
    if (audio.empty()) return false;
    if (track < 0 || track >= static_cast<int>(audio.size())) track = static_cast<int>(audio.size()) - 1;
    const DWORD as = audio[static_cast<size_t>(track)];
    r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    r->SetStreamSelection(as, TRUE);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(t.GetAddressOf());
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(r->SetCurrentMediaType(as, nullptr, t.Get()))) return false;
    ComPtr<IMFMediaType> cur;
    r->GetCurrentMediaType(as, cur.GetAddressOf());
    const int ch = static_cast<int>(MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2));
    std::vector<float> mono;
    first_us = -1;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample(as, 0, nullptr, &flags, &ts, s.GetAddressOf()))) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        if (first_us < 0) first_us = ts / 10;
        ComPtr<IMFMediaBuffer> b;
        s->ConvertToContiguousBuffer(b.GetAddressOf());
        BYTE* p = nullptr;
        DWORD len = 0;
        b->Lock(&p, nullptr, &len);
        const auto* f = reinterpret_cast<const float*>(p);
        for (DWORD i = 0; i + static_cast<DWORD>(ch) <= len / 4; i += static_cast<DWORD>(ch)) mono.push_back(f[i]);
        b->Unlock();
    }
    if (first_us < 0) first_us = 0;
    // Narrowband detection, so other sound on the system output (music, a video) does not hide the beeps: the 2 kHz
    // amplitude over 10 ms windows stepped by 1 ms; a beep is a window above 0.03 whose next window is above 0.1,
    // dominates its neighbours (1.5 / 2.5 kHz) and is 4x the window before. The onset is
    // then refined to the sample with the 2 kHz envelope over one period.
    auto amplitude = [&](size_t i, double hz) {
        const double c = 2.0 * std::cos(2.0 * kPi * hz / 48000.0);
        double s1 = 0, s2 = 0;
        for (size_t k = i; k < i + 480; ++k) {
            const double s0 = mono[k] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / 480.0;
    };
    size_t next_allowed = 480;   // a beep needs the 10 ms before it
    for (size_t i = 480; i + 1500 <= mono.size(); i += 48) {
        if (i < next_allowed) continue;
        const double a2 = amplitude(i, 2000);
        const size_t full = i + 480;   // a window lying inside the beep: a pure 2 kHz tone dominates its neighbours
        const double a_full = a2 > 0.03 ? amplitude(full, 2000) : 0.0;
        if (a_full > 0.1 && amplitude(i - 480, 2000) < 0.25 * a_full &&
            a_full > 3 * std::max(amplitude(full, 1500), amplitude(full, 2500))) {
            // The plateau: the strongest 10 ms window in the next 20 ms lies inside the beep. Walk back from inside
            // it along the 2 kHz envelope over one period (24 samples, which also cancels the 4 kHz product term):
            // it rises linearly from the first beep sample s to s + 23, so the last sample still at half the
            // plateau is s + 11. (A forward search could stop early on other sound: a 24-sample window is wideband.)
            size_t peak = i;
            double plateau = 0;
            for (size_t j = i; j <= i + 960; j += 24) {
                const double aj = amplitude(j, 2000);
                if (aj > plateau) {
                    plateau = aj;
                    peak = j;
                }
            }
            const double w = 2.0 * kPi * 2000.0 / 48000.0;
            auto envelope = [&](size_t n) {   // over [n - 23, n]
                double re = 0, im = 0;
                for (size_t m = n - 23; m <= n; ++m) {
                    re += mono[m] * std::cos(w * static_cast<double>(m));
                    im -= mono[m] * std::sin(w * static_cast<double>(m));
                }
                return 2.0 * std::sqrt(re * re + im * im) / 24.0;
            };
            size_t n = peak + 240;
            const size_t lowest = n > 1440 + 24 ? n - 1440 : 24;
            while (n > lowest && envelope(n - 1) >= 0.5 * plateau) --n;
            const size_t best = n - 11;
            beeps.push_back(first_us + static_cast<int64_t>(best) * 1'000'000 / 48000);
            next_allowed = best + 4800;   // 100 ms: a false detection in other sound cannot hide the next beep
        }
    }
    return true;
}

int Analyze(const std::string& path, const std::string& log_path, int64_t epoch, int track, RectI region) {
    struct Ev { int64_t nominal, beep, display, present; };
    std::vector<Ev> log;
    int64_t freq = Clock::QpcFrequency();
    if (!log_path.empty()) {
        FILE* f = std::fopen(log_path.c_str(), "r");
        if (!f) {
            std::fprintf(stderr, "cannot read %s\n", log_path.c_str());
            return 2;
        }
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            long long fq = 0;
            int rx, ry, rw, rh;
            if (std::sscanf(line, "# qpc_frequency %lld region %d,%d,%d,%d", &fq, &rx, &ry, &rw, &rh) == 5) {
                freq = fq;
                if (region.IsEmpty()) region = RectI{rx, ry, rw, rh};
                continue;
            }
            int k;
            long long nom, beep, pres, disp;
            if (std::sscanf(line, "%d,%lld,%lld,%lld,%lld", &k, &nom, &beep, &pres, &disp) == 5) log.push_back(Ev{nom, beep, disp, pres});
        }
        std::fclose(f);
        // Events without an exact vsync time: present time + the median present -> display latency of the others.
        std::vector<int64_t> lat;
        for (const Ev& e : log)
            if (e.display) lat.push_back(e.display - e.present);
        std::sort(lat.begin(), lat.end());
        const int64_t median = lat.empty() ? freq / 60 : lat[lat.size() / 2];
        int estimated = 0;
        for (Ev& e : log)
            if (!e.display) {
                e.display = e.present + median;
                ++estimated;
            }
        std::printf("log: %zu events, present->display latency median %.2f ms (%d events estimated with it)\n", log.size(),
                    median * 1000.0 / freq, estimated);
    }
    if (region.IsEmpty()) region = RectI{760, 340, kWin, kWin};
    std::vector<int64_t> flashes, beeps;
    int64_t frames = 0, audio_first = 0;
    if (!FlashTimes(path, RectI{region.x + 40, region.y + 40, region.w - 80, region.h - 80}, flashes, frames) ||
        !BeepTimes(path, track, beeps, audio_first)) {
        std::fprintf(stderr, "decoding %s failed\n", path.c_str());
        return 2;
    }
    std::printf("%s: %lld video frames, %zu flashes, %zu beeps (audio starts at %lld us)\n", path.c_str(), static_cast<long long>(frames),
                flashes.size(), beeps.size(), static_cast<long long>(audio_first));
    std::string list;
    for (int64_t b : beeps) AppendFormat(list, " %.3f", b / 1e6);
    std::printf("beeps at (s):%s\n", list.c_str());
    list.clear();
    for (int64_t f : flashes) AppendFormat(list, " %.3f", f / 1e6);
    std::printf("flashes at (s):%s\n", list.c_str());
    auto to_media_us = [&](int64_t qpc) { return (qpc - epoch) * 1'000'000 / freq; };
    int bad = 0, pairs = 0;
    double max_abs = 0, max_corr = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    size_t bi = 0;
    for (size_t fi = 0; fi < flashes.size(); ++fi) {
        // With a log: the event whose nominal time is nearest to the flash, and the beep nearest to that event's
        // emission (another sound on the output can produce a false beep; a missed flash must not shift the pairs).
        const Ev* best = nullptr;
        if (!log.empty() && epoch) {
            for (const Ev& e : log)
                if (!best || std::llabs(to_media_us(e.nominal) - flashes[fi]) < std::llabs(to_media_us(best->nominal) - flashes[fi])) best = &e;
            if (best && !best->beep) best = nullptr;
        }
        if (best) {
            bi = 0;
            for (size_t k = 1; k < beeps.size(); ++k)
                if (std::llabs(beeps[k] - to_media_us(best->beep)) < std::llabs(beeps[bi] - to_media_us(best->beep))) bi = k;
            if (bi >= beeps.size() || std::llabs(beeps[bi] - to_media_us(best->beep)) > 100'000) continue;
        } else {
            while (bi < beeps.size() && beeps[bi] < flashes[fi] - 500'000) ++bi;
        }
        if (bi >= beeps.size() || std::llabs(beeps[bi] - flashes[fi]) > 500'000) continue;
        const double off = (flashes[fi] - beeps[bi]) / 1000.0;
        ++pairs;
        max_abs = std::max(max_abs, std::fabs(off));
        sx += flashes[fi] / 1e6;
        sy += off;
        sxx += (flashes[fi] / 1e6) * (flashes[fi] / 1e6);
        sxy += (flashes[fi] / 1e6) * off;
        std::printf("  event at %9.3f s: video - audio %+7.2f ms", flashes[fi] / 1e6, off);
        if (!log.empty() && epoch) {
            if (best) {
                const double v_err = (flashes[fi] - to_media_us(best->display)) / 1000.0;
                const double a_err = (beeps[bi] - to_media_us(best->beep)) / 1000.0;
                const double corr = v_err - a_err;
                max_corr = std::max(max_corr, std::fabs(corr));
                std::printf(" | source emitted %+6.2f ms apart; video %+6.2f ms, audio %+6.2f ms vs emission; corrected %+6.2f ms",
                            (to_media_us(best->display) - to_media_us(best->beep)) / 1000.0, v_err, a_err, corr);
                if (std::fabs(corr) >= 20) ++bad;
            }
        } else if (std::fabs(off) >= 20) {
            ++bad;
        }
        std::printf("\n");
        if (!best) ++bi;
    }
    const double slope = pairs > 1 ? (pairs * sxy - sx * sy) / (pairs * sxx - sx * sx) : 0;   // ms per second
    std::printf("pairs %d, max |video - audio| %.2f ms%s, drift slope %+.4f ms/s (%+.2f ms over the recording)\n", pairs, max_abs,
                log.empty() ? "" : Format(", max |corrected| %.2f ms", max_corr).c_str(), slope,
                slope * (flashes.empty() ? 0 : (flashes.back() - flashes.front()) / 1e6));
    return pairs > 0 && bad == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    int seconds = 130, period_ms = 2000, track = -1;
    std::string log_path, analyze;
    int64_t epoch = 0;
    RectI region{};
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        const bool has = i + 1 < argc;
        if (s == "--seconds" && has) seconds = std::atoi(argv[++i]);
        else if (s == "--period-ms" && has) period_ms = std::max(500, std::atoi(argv[++i]));
        else if (s == "--log" && has) log_path = argv[++i];
        else if (s == "--analyze" && has) analyze = argv[++i];
        else if (s == "--epoch" && has) epoch = std::atoll(argv[++i]);
        else if (s == "--audio-track" && has) track = std::atoi(argv[++i]);
        else if (s == "--region" && has) std::sscanf(argv[++i], "%d,%d,%d,%d", &region.x, &region.y, &region.w, &region.h);
        else {
            std::fprintf(stderr, "usage: avsync_source [--seconds N] [--period-ms N] [--log f.csv] | --analyze rec.mp4 [--log f.csv --epoch QPC] "
                                 "[--audio-track N] [--region x,y,w,h]\n");
            return 2;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    platform::Paths::Init();
    Log::Init(platform::Paths::LogsDir().c_str());
    int rc;
    if (!analyze.empty()) {
        media::MfScope mf;
        rc = Analyze(analyze, log_path, epoch, track, region);
    } else {
        rc = RunSource(seconds, period_ms, log_path);
    }
    Log::Shutdown();
    CoUninitialize();
    return rc;
}
