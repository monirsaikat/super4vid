// capture_bench: measures the P02 capture pipeline (DXGI duplication -> NV12 on the GPU) at a constant frame rate.
//
//   capture_bench --monitor 0 [--region x,y,w,h] --fps 60 --seconds 30 [--no-convert] [--dump-frame 100 out.png]
//                 [--animate] [--cursor] [--video-processor] [--copy-path] [--pixel-shader] [--debug-layer]
//
// --animate      starts a second capture_bench process that shows a 60 fps animation (a stand-in for a playing video)
//                over most of the monitor, so every tick has a new desktop image. Its cost is not in our numbers.
// --cursor       polls CursorTracker at 120 Hz on the main thread (P05 does this on ct-hooks).
// --no-convert   duplication + crop only (own pacing loop, no NV12 conversion).
// --copy-path    crop-copy into a BGRA texture before converting (ScreenCapturer::Config::zero_copy = false).
// --pixel-shader two pixel-shader passes instead of the compute conversion (Config::allow_compute = false).
// Prints the capture stats every second and a summary: ticks, new frames, repeats, drops, acquire/convert/GPU times,
// process CPU % and RSS. GPU % is read from Task Manager or the "GPU Engine" performance counters.
//
// P04 encode options:
//   --encode out.mp4   the frames go to ct::media::Mp4Writer (hardware H.264) instead of the checksum sink
//   --mic / --system   record the default microphone / system audio (loopback) as AAC streams 1.. in that order
//   --rc quality|vbr|cbr  --bitrate bps   rate control (default quality 70 with the table bitrate as peak cap)
// After the run the file is probed: frame count vs ticks, duration, size vs bitrate * seconds, first/last timestamps.

#include <ct/capture/cursor_tracker.h>
#include <ct/capture/desktop_duplicator.h>
#include <ct/capture/display.h>
#include <ct/capture/screen_capturer.h>
#include <ct/audio/devices.h>
#include <ct/audio/wasapi_capture.h>
#include <ct/audio/wasapi_loopback.h>
#include <ct/media/mf_runtime.h>
#include <ct/media/mp4_writer.h>
#include <ct/media/probe.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/stats.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/image_io.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>

#include <objbase.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;
using Microsoft::WRL::ComPtr;

namespace {

struct Args {
    int monitor = 0;
    RectI region{};
    int fps = 60;
    int seconds = 30;
    bool no_convert = false;
    int64_t dump_frame = -1;
    std::string dump_path;
    bool animate = false;
    bool cursor = false;
    bool video_processor = false;
    bool copy_path = false;
    bool pixel_shader = false;
    bool debug_layer = false;
    std::string encode;
    bool mic = false, system = false;
    media::RateControl rc = media::RateControl::Quality;
    uint32_t bitrate = 0;
    int animator_monitor = -1;   // internal: run as the animation window
    int animator_seconds = 0;
};

bool ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&](const char*& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        const char* v = nullptr;
        if (s == "--monitor" && next(v)) a.monitor = std::atoi(v);
        else if (s == "--fps" && next(v)) a.fps = std::max(1, std::atoi(v));
        else if (s == "--seconds" && next(v)) a.seconds = std::max(1, std::atoi(v));
        else if (s == "--region" && next(v)) {
            if (std::sscanf(v, "%d,%d,%d,%d", &a.region.x, &a.region.y, &a.region.w, &a.region.h) != 4) return false;
        } else if (s == "--dump-frame" && next(v)) {
            a.dump_frame = std::atoll(v);
            if (!next(v)) return false;
            a.dump_path = v;
        } else if (s == "--no-convert") a.no_convert = true;
        else if (s == "--animate") a.animate = true;
        else if (s == "--cursor") a.cursor = true;
        else if (s == "--video-processor") a.video_processor = true;
        else if (s == "--copy-path") a.copy_path = true;
        else if (s == "--pixel-shader") a.pixel_shader = true;
        else if (s == "--debug-layer") a.debug_layer = true;
        else if (s == "--encode" && next(v)) a.encode = v;
        else if (s == "--mic") a.mic = true;
        else if (s == "--system") a.system = true;
        else if (s == "--bitrate" && next(v)) a.bitrate = static_cast<uint32_t>(std::atoll(v));
        else if (s == "--rc" && next(v)) {
            const std::string m = v;
            if (m == "quality") a.rc = media::RateControl::Quality;
            else if (m == "vbr") a.rc = media::RateControl::Vbr;
            else if (m == "cbr") a.rc = media::RateControl::Cbr;
            else return false;
        }
        else if (s == "--animator" && next(v)) {
            a.animator_monitor = std::atoi(v);
            if (!next(v)) return false;
            a.animator_seconds = std::atoi(v);
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", s.c_str());
            return false;
        }
    }
    return true;
}

// ---- process metrics -------------------------------------------------------------------------------------------

int64_t ProcessCpu100ns() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return v(k) + v(u);
}

void Memory(double& rss_mb, double& peak_mb) {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    rss_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    peak_mb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
}

int CpuCount() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<int>(si.dwNumberOfProcessors);
}

// ---- animator (child process) ----------------------------------------------------------------------------------

LRESULT CALLBACK AnimProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

int RunAnimator(int monitor, int seconds) {
    D3DDevice dev;
    if (FAILED(dev.Init(false))) return 2;
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    if (monitor < 0 || monitor >= static_cast<int>(displays.size())) return 2;
    const RectI r = displays[static_cast<size_t>(monitor)].rect;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &AnimProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CtCaptureBenchAnimator";
    RegisterClassExW(&wc);
    // 75 % of the monitor, centred, topmost, click-through: like a video playing in a large window.
    const int w = r.w * 3 / 4, h = r.h * 3 / 4;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
                                wc.lpszClassName, L"capture_bench animator", WS_POPUP, r.x + (r.w - w) / 2,
                                r.y + (r.h - h) / 2, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 3;
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    platform::SwapChain sc;
    if (FAILED(sc.Init(dev, hwnd))) return 4;
    Stopwatch sw;
    sw.Start();
    // A moving bar over a slowly changing background: every vsync presents a different image.
    D3D11_RECT bar{};
    for (int64_t frame = 0; sw.ElapsedMs() < seconds * 1000.0; ++frame) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        WaitForSingleObjectEx(sc.WaitableObject(), 100, FALSE);
        {
            D3DDevice::Lock lock(dev);
            const float t = static_cast<float>(frame % 240) / 240.f;
            const float bg[4] = {0.2f + 0.6f * t, 0.3f, 0.8f - 0.6f * t, 1.f};
            dev.Context()->ClearRenderTargetView(sc.Rtv(), bg);
            const int x = static_cast<int>((frame * 12) % std::max(1, w - 200));
            bar = D3D11_RECT{x, 0, x + 200, h};
            const float fg[4] = {1.f, 1.f, 1.f, 1.f};
            ComPtr<ID3D11DeviceContext1> ctx1;
            if (SUCCEEDED(dev.Context()->QueryInterface(IID_PPV_ARGS(ctx1.GetAddressOf()))))
                ctx1->ClearView(sc.Rtv(), fg, &bar, 1);
        }
        if (FAILED(sc.Present(true))) break;
    }
    sc.Shutdown();
    DestroyWindow(hwnd);
    dev.Shutdown();
    return 0;
}

PROCESS_INFORMATION StartAnimator(int monitor, int seconds) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --animator " + std::to_wstring(monitor) + L" " + std::to_wstring(seconds);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::fprintf(stderr, "could not start the animator: %lu\n", GetLastError());
        pi = PROCESS_INFORMATION{};
    }
    return pi;
}

// ---- sink --------------------------------------------------------------------------------------------------------

class BenchSink final : public FrameSink {
public:
    HRESULT Init(D3DDevice& dev, platform::TexturePool& pool, int64_t dump_frame) {
        m_dev = &dev;
        m_pool = &pool;
        m_dump_frame = dump_frame;
        D3D11_TEXTURE2D_DESC d = pool.Desc();
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        CT_RETURN_IF_FAILED(dev.Device()->CreateTexture2D(&d, nullptr, m_check.GetAddressOf()));
        if (dump_frame >= 0) CT_RETURN_IF_FAILED(dev.Device()->CreateTexture2D(&d, nullptr, m_dump.GetAddressOf()));
        m_checksums.reserve(4096);
        return S_OK;
    }

    // Capture thread, device lock held.
    bool OnFrame(ID3D11Texture2D* nv12, uint32_t index, TimeUs) override {
        ID3D11DeviceContext* ctx = m_dev->Context();
        if (m_frames % 60 == 0) {
            // The copy issued 60 frames ago is long finished: map it without stalling, then queue the next one.
            if (m_check_pending) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (ctx->Map(m_check.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
                    const D3D11_TEXTURE2D_DESC& d = m_pool->Desc();
                    uint32_t h = 2166136261u;
                    for (UINT y = 0; y < d.Height; y += 8) {
                        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                        h = (h ^ Fnv1a32(row, d.Width)) * 16777619u;
                    }
                    ctx->Unmap(m_check.Get(), 0);
                    if (m_checksums.size() < m_checksums.capacity()) m_checksums.push_back(h);
                }
            }
            ctx->CopyResource(m_check.Get(), nv12);
            m_check_pending = true;
        }
        if (m_dump && static_cast<int64_t>(m_frames) == m_dump_frame) {
            ctx->CopyResource(m_dump.Get(), nv12);
            m_dumped = true;
        }
        ++m_frames;
        m_pool->Release(index);
        return true;
    }
    void OnError(HRESULT hr) override { m_error = hr; }

    uint64_t Frames() const { return m_frames; }
    HRESULT Error() const { return m_error; }
    size_t DistinctChecksums() const { return std::set<uint32_t>(m_checksums.begin(), m_checksums.end()).size(); }
    size_t Checksums() const { return m_checksums.size(); }

    bool WriteDump(const std::string& path) {
        if (!m_dumped) {
            std::fprintf(stderr, "frame %lld was not captured (only %llu frames)\n", static_cast<long long>(m_dump_frame),
                         static_cast<unsigned long long>(m_frames));
            return false;
        }
        const D3D11_TEXTURE2D_DESC& d = m_pool->Desc();
        std::vector<uint8_t> rgba(static_cast<size_t>(d.Width) * d.Height * 4);
        {
            D3DDevice::Lock lock(*m_dev);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(m_dev->Context()->Map(m_dump.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
            const auto* yp = static_cast<const uint8_t*>(m.pData);
            const uint8_t* uvp = yp + static_cast<size_t>(m.RowPitch) * d.Height;
            // BT.709 limited range back to full-range RGB.
            for (UINT y = 0; y < d.Height; ++y)
                for (UINT x = 0; x < d.Width; ++x) {
                    const float Y = 1.164383f * (yp[static_cast<size_t>(y) * m.RowPitch + x] - 16.f);
                    const uint8_t* c = uvp + static_cast<size_t>(y / 2) * m.RowPitch + (x / 2) * 2;
                    const float U = c[0] - 128.f, V = c[1] - 128.f;
                    auto clamp8 = [](float v) { return static_cast<uint8_t>(std::clamp(v + 0.5f, 0.f, 255.f)); };
                    uint8_t* o = &rgba[(static_cast<size_t>(y) * d.Width + x) * 4];
                    o[0] = clamp8(Y + 1.792741f * V);
                    o[1] = clamp8(Y - 0.213249f * U - 0.532909f * V);
                    o[2] = clamp8(Y + 2.112402f * U);
                    o[3] = 255;
                }
            m_dev->Context()->Unmap(m_dump.Get(), 0);
        }
        return platform::ImageIo::WritePngRgba(path.c_str(), rgba.data(), static_cast<int>(d.Width), static_cast<int>(d.Height),
                                               static_cast<int>(d.Width) * 4);
    }

private:
    D3DDevice* m_dev = nullptr;
    platform::TexturePool* m_pool = nullptr;
    ComPtr<ID3D11Texture2D> m_check, m_dump;
    std::vector<uint32_t> m_checksums;
    uint64_t m_frames = 0;
    int64_t m_dump_frame = -1;
    bool m_check_pending = false;
    bool m_dumped = false;
    HRESULT m_error = S_OK;
};

// ---- encode (P04) ------------------------------------------------------------------------------------------------

struct EncodeRun {
    audio::AudioDevices devices;
    audio::WasapiCapture mic;
    audio::WasapiLoopback sys;
    media::Mp4Writer writer;
    media::Mp4Writer::Config cfg;
    bool use_mic = false, use_sys = false;
    uint32_t mic_stream = 0, sys_stream = 0;

    HRESULT Init(D3DDevice& dev, ScreenCapturer& cap, const Args& a) {
        CT_RETURN_IF_FAILED(devices.Init());
        use_mic = a.mic && !devices.DefaultId(true).empty();
        if (a.mic && !use_mic) std::printf("no microphone on this machine: --mic ignored\n");
        use_sys = a.system;
        cfg.path = a.encode;
        cfg.video.size = cap.OutputSize();
        cfg.video.fps_num = a.fps;
        cfg.video.rc = a.rc;
        cfg.video.bitrate_bps = a.bitrate;
        uint32_t n = 0;
        if (use_mic) {
            mic_stream = n++;
            media::AudioEncodeConfig ac;
            ac.label = "mic";
            cfg.audio.push_back(ac);
        }
        if (use_sys) {
            sys_stream = n++;
            media::AudioEncodeConfig ac;
            ac.label = "system";
            cfg.audio.push_back(ac);
        }
        const HRESULT hr = writer.Init(dev, cfg, &cap.Nv12Pool());
        if (FAILED(hr)) {
            std::fprintf(stderr, "Mp4Writer::Init: %s\n", writer.ErrorMessage().c_str());
            return hr;
        }
        std::printf("encoder %s (%s), %s, mean %.2f Mbps, peak %.2f Mbps, %zu audio stream(s)\n", writer.EncoderName().c_str(),
                    writer.IsHardware() ? "hardware" : "software",
                    a.rc == media::RateControl::Quality ? "quality 70" : a.rc == media::RateControl::Vbr ? "VBR" : "CBR",
                    media::EffectiveBitrate(cfg.video) / 1e6, media::EffectiveMaxBitrate(cfg.video) / 1e6, cfg.audio.size());
        audio::CaptureConfig cc;
        if (use_mic) {
            cc.stream_id = mic_stream;
            CT_RETURN_IF_FAILED(mic.Init(cc, devices));
        }
        if (use_sys) {
            cc.stream_id = sys_stream;
            CT_RETURN_IF_FAILED(sys.Init(cc, devices));
            std::printf("system audio: %s\n", sys.DeviceName().c_str());
        }
        return S_OK;
    }

    bool StartAudio(int64_t epoch) {
        if (use_mic && !mic.Start(epoch, writer)) return false;
        if (use_sys && !sys.Start(epoch, writer)) return false;
        return true;
    }

    void StopAudio() {
        if (use_mic) mic.Stop();
        if (use_sys) sys.Stop();
    }

    void PrintSecond() const {
        const auto& s = writer.GetStats();
        std::printf("        encode: written %llu rejected %llu queue %llu textures-in-MF max %u | latency %.3f/%.3f WriteSample %.3f/%.3f ms",
                    static_cast<unsigned long long>(s.video_written.load()), static_cast<unsigned long long>(s.video_rejected.load()),
                    static_cast<unsigned long long>(s.queue_depth.load()), s.max_textures_in_mf.load(), s.video_latency.AvgMs(),
                    s.video_latency.MaxMs(),
                    s.video_write.AvgMs(), s.video_write.MaxMs());
        for (size_t i = 0; i < cfg.audio.size(); ++i)
            std::printf(" | %s %llu frames (rej %llu)", cfg.audio[i].label, static_cast<unsigned long long>(s.audio_written[i].load()),
                        static_cast<unsigned long long>(s.audio_rejected_frames[i].load()));
        std::printf("\n");
    }

    // Probes the finished file; returns false when a check failed.
    bool Report(const CaptureStats& cs, const Args& a, double wall_s) {
        const auto& s = writer.GetStats();
        bool ok = true;
        std::printf("encode: written %llu rejected %llu, finalize %lld ms; latency avg %.3f max %.3f ms; WriteSample avg %.3f max %.3f ms\n",
                    static_cast<unsigned long long>(s.video_written.load()), static_cast<unsigned long long>(s.video_rejected.load()),
                    static_cast<long long>(s.finalize_ms.load()), s.video_latency.AvgMs(), s.video_latency.MaxMs(), s.video_write.AvgMs(),
                    s.video_write.MaxMs());
        for (size_t i = 0; i < cfg.audio.size(); ++i) {
            std::printf("  audio %s: %llu frames written (%.3f s), lead pad %llu, rejected %llu", cfg.audio[i].label,
                        static_cast<unsigned long long>(s.audio_written[i].load()), s.audio_written[i].load() / 48000.0,
                        static_cast<unsigned long long>(s.audio_lead_pad_frames[i].load()),
                        static_cast<unsigned long long>(s.audio_rejected_frames[i].load()));
            const audio::CaptureStats as = (use_sys && i == sys_stream) ? sys.GetStats() : mic.GetStats();
            std::printf("; capture: inserted %llu dropped %llu discontinuities %llu drift %.2f ms\n",
                        static_cast<unsigned long long>(as.frames_inserted), static_cast<unsigned long long>(as.frames_dropped),
                        static_cast<unsigned long long>(as.discontinuities), as.drift_ms);
            if (s.audio_rejected_frames[i].load() != 0) ok = false;
        }
        media::ProbeResult p;
        if (FAILED(media::Probe(cfg.path, p))) {
            std::printf("PROBE FAILED for %s\n", cfg.path.c_str());
            return false;
        }
        media::StreamScan v;
        media::ScanStream(cfg.path, 0, v);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(Utf8ToWide(cfg.path).c_str(), GetFileExInfoStandard, &fa);
        const uint64_t size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        const double dur_s = p.duration_us / 1e6;
        std::printf("file %s: %.2f MB, duration %.3f s, %dx%d %s @ %d/%d, %d video + %d audio\n", cfg.path.c_str(), size / 1048576.0,
                    dur_s, p.video_width, p.video_height, p.video_codec.c_str(), p.fps_num, p.fps_den, p.video_streams, p.audio_streams);
        const uint64_t expected = static_cast<uint64_t>(a.fps) * static_cast<uint64_t>(a.seconds);
        const uint64_t delivered = cs.new_frames.load() + cs.repeats.load();
        std::printf("  video: %llu frames in the file (delivered %llu, ticks %llu, expected fps*seconds %llu), first %lld us, last %lld us, "
                    "max step %lld us, %llu keyframes, %.2f MB\n",
                    static_cast<unsigned long long>(v.samples), static_cast<unsigned long long>(delivered),
                    static_cast<unsigned long long>(cs.ticks.load()), static_cast<unsigned long long>(expected),
                    static_cast<long long>(v.first_us), static_cast<long long>(v.last_us), static_cast<long long>(v.max_gap_us),
                    static_cast<unsigned long long>(v.keyframes), v.bytes / 1048576.0);
        if (v.samples != delivered) ok = false;
        if (v.samples + 1 < expected || v.samples > expected + 1) ok = false;
        uint64_t audio_bytes = 0;
        for (size_t i = 0; i < cfg.audio.size(); ++i) {
            media::StreamScan as;
            media::ScanStream(cfg.path, static_cast<uint32_t>(1 + i), as);
            audio_bytes += as.bytes;
            std::printf("  audio %s: first %lld us, end %lld us (video end %lld us, delta %+.1f ms), %.2f MB\n", cfg.audio[i].label,
                        static_cast<long long>(as.first_us), static_cast<long long>(as.end_us), static_cast<long long>(v.end_us),
                        (as.end_us - v.end_us) / 1000.0, as.bytes / 1048576.0);
        }
        const double target = media::EffectiveBitrate(cfg.video) * (v.end_us - v.first_us) / 1e6 / 8.0;
        std::printf("  video bitrate %.3f Mbps vs target %.3f Mbps: video bytes %.1f %% of target (%s mode)\n",
                    v.bytes * 8.0 / ((v.end_us - v.first_us) / 1e6) / 1e6, media::EffectiveBitrate(cfg.video) / 1e6,
                    100.0 * v.bytes / target,
                    a.rc == media::RateControl::Quality ? "quality" : a.rc == media::RateControl::Vbr ? "VBR" : "CBR");
        std::printf("  file bytes incl. audio/container: %.1f %% of (video + audio bitrate) * duration\n",
                    100.0 * size / (target + audio_bytes));
        (void)wall_s;
        return ok;
    }
};

// ---- reporting ---------------------------------------------------------------------------------------------------

void PrintSecond(int second, const CaptureStats& s) {
    std::string extra;
    Stats::Dump(extra);
    std::printf("[%3d s] ticks %llu new %llu repeats %llu drop_pool %llu drop_sink %llu late %llu lost %llu reinit %llu | "
                "acquire %.3f/%.3f convert %.3f/%.3f gpu %.3f/%.3f ms (avg/max) | %s\n",
                second, static_cast<unsigned long long>(s.ticks.load()), static_cast<unsigned long long>(s.new_frames.load()),
                static_cast<unsigned long long>(s.repeats.load()), static_cast<unsigned long long>(s.dropped_pool_empty.load()),
                static_cast<unsigned long long>(s.dropped_sink.load()), static_cast<unsigned long long>(s.late_ticks.load()),
                static_cast<unsigned long long>(s.access_lost.load()), static_cast<unsigned long long>(s.reinits.load()),
                s.acquire.AvgMs(), s.acquire.MaxMs(), s.convert.AvgMs(), s.convert.MaxMs(), s.gpu.AvgMs(), s.gpu.MaxMs(),
                extra.c_str());
    std::fflush(stdout);
}

// --no-convert: the duplicator alone, paced like ScreenCapturer (wait to tick - 1 ms, acquire with <= 4 ms timeout).
int RunNoConvert(D3DDevice& dev, const DisplayInfo& display, const RectI& region, const Args& a) {
    DesktopDuplicator dup;
    DesktopDuplicator::Config dc;
    dc.display = display;
    dc.region = region;
    if (FAILED(dup.Init(dev, dc))) return 1;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(dup.Size().w);
    d.Height = static_cast<UINT>(dup.Size().h);
    d.MipLevels = d.ArraySize = 1;
    d.Format = dup.Format();
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev.Device()->CreateTexture2D(&d, nullptr, tex.GetAddressOf()))) return 1;
    HighResTimer timer;
    timer.Init();
    const int64_t freq = Clock::QpcFrequency(), epoch = Clock::QpcNow(), cpu0 = ProcessCpu100ns();
    uint64_t ticks = 0, frames = 0, lost = 0;
    Stats::Timing acquire;
    const int64_t total = static_cast<int64_t>(a.seconds) * a.fps;
    for (int64_t n = 0; n < total; ++n) {
        const int64_t tick = Clock::UsToQpc(FrameTimeUs(n, a.fps, 1), epoch);
        timer.WaitUntilQpc(tick - freq / 1000);
        D3DDevice::Lock lock(dev);
        const int64_t now = Clock::QpcNow();
        const uint32_t to = static_cast<uint32_t>(std::clamp<int64_t>((tick + 2 * freq / 1000 - now) * 1000 / freq, 0, 4));
        int64_t present = 0;
        const HRESULT hr = dup.AcquireInto(tex.Get(), to, present);
        if (hr == S_OK) {
            acquire.AddUs(Clock::QpcToUs(Clock::QpcNow(), now));
            ++frames;
        } else if (hr == DXGI_ERROR_ACCESS_LOST) {
            ++lost;
            dup.Reinit();
        }
        ++ticks;
    }
    const double wall_s = static_cast<double>(Clock::QpcNow() - epoch) / static_cast<double>(freq);
    const double cpu_s = static_cast<double>(ProcessCpu100ns() - cpu0) / 1e7;
    double rss = 0, peak = 0;
    Memory(rss, peak);
    std::printf("summary (no convert): ticks %llu new %llu lost %llu acquire avg %.3f max %.3f ms; CPU %.2f %% of machine "
                "(%.1f %% of one core); RSS %.1f MB (peak %.1f MB)\n",
                static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(frames), static_cast<unsigned long long>(lost),
                acquire.AvgMs(), acquire.MaxMs(), 100.0 * cpu_s / wall_s / CpuCount(), 100.0 * cpu_s / wall_s, rss, peak);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Args a;
    if (!ParseArgs(argc, argv, a)) {
        std::fprintf(stderr, "usage: capture_bench --monitor 0 [--region x,y,w,h] --fps 60 --seconds 30 [--no-convert] "
                             "[--dump-frame N out.png] [--animate] [--cursor] [--video-processor] [--debug-layer]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (a.animator_monitor >= 0) return RunAnimator(a.animator_monitor, a.animator_seconds);

    platform::Paths::Init();
    Log::Init(platform::Paths::LogsDir().c_str());

    D3DDevice dev;
    HRESULT hr = dev.Init(a.debug_layer);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11 device: %s\n", HrToString(hr).c_str());
        return 1;
    }
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    if (a.monitor < 0 || a.monitor >= static_cast<int>(displays.size())) {
        std::fprintf(stderr, "monitor %d not found (%zu displays)\n", a.monitor, displays.size());
        return 1;
    }
    const DisplayInfo& display = displays[static_cast<size_t>(a.monitor)];
    RectI region = a.region;   // virtual-screen px; empty = the whole monitor
    region = ClampRegionToDisplay(display, region);
    std::printf("adapter %s; monitor %d %ls \"%s\" %dx%d @ %u Hz%s; region %dx%d at (%d,%d); %d fps for %d s%s%s\n",
                dev.AdapterName().c_str(), a.monitor, display.device_name.c_str(), display.friendly_name.c_str(), display.rect.w,
                display.rect.h, display.refresh_hz, display.hdr ? " HDR" : "", region.w, region.h, region.x, region.y, a.fps,
                a.seconds, a.animate ? ", animated" : "", a.video_processor ? ", video processor" : "");

    PROCESS_INFORMATION animator{};
    if (a.animate) {
        animator = StartAnimator(a.monitor, a.seconds + 3);
        Sleep(1000);   // let it start presenting
    }

    int rc = 0;
    if (a.no_convert) {
        rc = RunNoConvert(dev, display, region, a);
    } else {
        ScreenCapturer cap;
        ScreenCapturer::Config cfg;
        cfg.display = display;
        cfg.region = region;
        cfg.fps_num = a.fps;
        cfg.fps_den = 1;
        cfg.force_video_processor = a.video_processor;
        cfg.zero_copy = !a.copy_path;
        cfg.allow_compute = !a.pixel_shader;
        hr = cap.Init(dev, cfg);
        if (FAILED(hr)) {
            std::fprintf(stderr, "ScreenCapturer::Init: %s\n", HrToString(hr).c_str());
            return 1;
        }
        BenchSink sink;
        if (FAILED(sink.Init(dev, cap.Nv12Pool(), a.dump_frame))) return 1;
        std::unique_ptr<EncodeRun> enc;
        if (!a.encode.empty()) {
            enc = std::make_unique<EncodeRun>();
            if (FAILED(enc->Init(dev, cap, a)) || !enc->writer.Start()) return 1;
        }
        FrameSink& frame_sink = enc ? static_cast<FrameSink&>(enc->writer) : static_cast<FrameSink&>(sink);

        auto samples = std::make_unique<SpscQueue<CursorSample, 4096>>();
        auto shapes = std::make_unique<SpscQueue<CursorShape, 64>>();
        CursorTracker cursor;
        uint64_t cursor_samples = 0, cursor_shapes = 0;

        HighResTimer timer;
        timer.Init();
        timer.SetSpinUs(0);   // the main thread only needs coarse waits
        const int64_t freq = Clock::QpcFrequency();
        const int64_t epoch = Clock::QpcNow();
        if (enc) std::printf("epoch_qpc %lld (QPC frequency %lld)\n", static_cast<long long>(epoch), static_cast<long long>(freq));
        const int64_t cpu0 = ProcessCpu100ns();
        if (!cap.Start(epoch, frame_sink)) return 1;
        if (enc && !enc->StartAudio(epoch)) {
            std::fprintf(stderr, "audio capture did not start\n");
            return 1;
        }
        const int64_t end = epoch + freq * a.seconds;
        int64_t next_print = epoch + freq;
        int second = 0;
        const int64_t cursor_step = freq / 120;
        int64_t next_cursor = epoch;
        // Stop after exactly fps * seconds ticks (poll every 1 ms over the last 20 ms), so the file has that many frames.
        const uint64_t target_ticks = static_cast<uint64_t>(a.fps) * static_cast<uint64_t>(a.seconds);
        const int64_t near_end = end - freq / 50;
        while (cap.Stats().ticks.load() < target_ticks && cap.IsRunning()) {
            const int64_t wake = a.cursor ? std::min(next_cursor, next_print) : next_print;
            const int64_t t_now = Clock::QpcNow();
            timer.WaitUntilQpc(t_now >= near_end ? t_now + freq / 1000 : std::min(wake, near_end));
            const int64_t now = Clock::QpcNow();
            if (a.cursor && now >= next_cursor) {
                cursor.Poll(Clock::QpcToUs(now, epoch), cap.Region(), *samples, *shapes);
                CursorSample s;
                while (samples->Pop(s)) ++cursor_samples;
                CursorShape sh;
                while (shapes->Pop(sh)) ++cursor_shapes;
                next_cursor += cursor_step;
            }
            if (now >= next_print) {
                PrintSecond(++second, cap.Stats());
                if (enc) enc->PrintSecond();
                next_print += freq;
            }
        }
        cap.Stop();
        HRESULT enc_hr = S_OK;
        if (enc) {
            enc->StopAudio();
            enc_hr = enc->writer.Finalize();
            if (FAILED(enc_hr)) std::fprintf(stderr, "Mp4Writer::Finalize: %s\n", enc->writer.ErrorMessage().c_str());
        }
        const double wall_s = static_cast<double>(Clock::QpcNow() - epoch) / static_cast<double>(freq);
        const double cpu_s = static_cast<double>(ProcessCpu100ns() - cpu0) / 1e7;
        double rss = 0, peak = 0;
        Memory(rss, peak);
        const CaptureStats& s = cap.Stats();
        std::printf("\nsummary: ticks %llu new_frames %llu repeats %llu dropped_pool_empty %llu dropped_sink %llu late_ticks %llu "
                    "access_lost %llu reinits %llu\n",
                    static_cast<unsigned long long>(s.ticks.load()), static_cast<unsigned long long>(s.new_frames.load()),
                    static_cast<unsigned long long>(s.repeats.load()), static_cast<unsigned long long>(s.dropped_pool_empty.load()),
                    static_cast<unsigned long long>(s.dropped_sink.load()), static_cast<unsigned long long>(s.late_ticks.load()),
                    static_cast<unsigned long long>(s.access_lost.load()), static_cast<unsigned long long>(s.reinits.load()));
        std::printf("acquire avg %.3f max %.3f ms | convert avg %.3f max %.3f ms | gpu convert avg %.3f max %.3f ms\n",
                    s.acquire.AvgMs(), s.acquire.MaxMs(), s.convert.AvgMs(), s.convert.MaxMs(), s.gpu.AvgMs(), s.gpu.MaxMs());
        std::printf("process CPU %.2f %% of the machine (%d logical CPUs; %.1f %% of one core) | RSS %.1f MB (peak %.1f MB)\n",
                    100.0 * cpu_s / wall_s / CpuCount(), CpuCount(), 100.0 * cpu_s / wall_s, rss, peak);
        std::printf("frames delivered %llu; luma checksums %zu distinct of %zu sampled (every 60th frame)%s\n",
                    static_cast<unsigned long long>(sink.Frames()), sink.DistinctChecksums(), sink.Checksums(),
                    FAILED(sink.Error()) ? "; CAPTURE ERROR" : "");
        if (a.cursor)
            std::printf("cursor: %llu samples, %llu shapes, %llu polls\n", static_cast<unsigned long long>(cursor_samples),
                        static_cast<unsigned long long>(cursor_shapes), static_cast<unsigned long long>(cursor.Polls()));
        if (a.dump_frame >= 0) {
            if (sink.WriteDump(a.dump_path)) std::printf("frame %lld written to %s\n", static_cast<long long>(a.dump_frame), a.dump_path.c_str());
            else rc = 1;
        }
        if (FAILED(sink.Error())) rc = 1;
        if (enc) {
            if (FAILED(enc_hr) || !enc->Report(s, a, wall_s)) rc = 1;
            enc.reset();   // before the capturer: the writer detaches from its pool
        }
    }

    if (animator.hProcess) {
        if (WaitForSingleObject(animator.hProcess, 5000) != WAIT_OBJECT_0) TerminateProcess(animator.hProcess, 0);
        CloseHandle(animator.hProcess);
        CloseHandle(animator.hThread);
    }
    dev.Shutdown();
    dev.ReportLiveObjects();
    Log::Shutdown();
    CoUninitialize();
    return rc;
}
