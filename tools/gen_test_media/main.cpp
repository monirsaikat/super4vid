// gen_test_media: writes the synthetic test recordings in tests/data (see tests/data/README.md for the layout).
//
//   gen_test_media --all <dir>                      pattern_1080p60_5s.mp4 and pattern_4k60_3s.mp4 into <dir>
//   gen_test_media --size 1920x1080 --fps 60 --seconds 5 --bitrate 6000000 --out file.mp4
//
// Video (D3D11, ClearView rectangles -> ColorConverter -> Mp4Writer::WriteVideoSync, H.264 VBR):
//   - 8 vertical colour bars (75 % white, yellow, cyan, green, magenta, red, blue, black) scrolling 4 px per frame,
//   - a black box (32,32)-(432,96) with the frame number as 12 seven-segment digits (most significant first),
//   - every 60th frame (n % 60 == 0) the whole frame except the counter box is white (the "flash").
// Audio (one AAC stream, 48 kHz stereo): 1 kHz sine at -12 dBFS; for 20 ms from each flash frame's time the sine is
// 2 kHz instead (the "beep").
#include <ct/capture/color_convert.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/mf_runtime.h>
#include <ct/media/mp4_writer.h>
#include <ct/media/probe.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/paths.h>
#include <ct/platform/win.h>

#include <objbase.h>

#include <algorithm>
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

struct Job {
    int w = 1920, h = 1080, fps = 60, seconds = 5;
    uint32_t bitrate = 6'000'000;
    std::string out;
};

// Segment rectangles of one digit cell, relative to its top-left corner (x0, y0); 28 x 56 px, stroke 4 px.
//   a top, b top-right, c bottom-right, d bottom, e bottom-left, f top-left, g middle
constexpr D3D11_RECT kSeg[7] = {
    {4, 0, 24, 4}, {24, 4, 28, 26}, {24, 30, 28, 52}, {4, 52, 24, 56}, {0, 30, 4, 52}, {0, 4, 4, 26}, {4, 26, 24, 30}};
constexpr uint8_t kDigitSegs[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};   // bit i = segment a..g

void Fill(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* rtv, const float c[4], int l, int t, int r, int b) {
    const D3D11_RECT rc{l, t, r, b};
    ctx->ClearView(rtv, c, &rc, 1);
}

void DrawFrame(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* rtv, int w, int h, int64_t n) {
    static const float bars[8][4] = {{0.75f, 0.75f, 0.75f, 1}, {0.75f, 0.75f, 0, 1}, {0, 0.75f, 0.75f, 1}, {0, 0.75f, 0, 1},
                                     {0.75f, 0, 0.75f, 1},     {0.75f, 0, 0, 1},     {0, 0, 0.75f, 1},     {0, 0, 0, 1}};
    static const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
    if (n % 60 == 0) {
        ctx->ClearView(rtv, white, nullptr, 0);
    } else {
        const int bw = w / 8;
        const int shift = static_cast<int>((n * 4) % w);
        for (int i = 0; i < 8; ++i) {
            int x = (i * bw + shift) % w;
            const int x_end = x + bw;
            Fill(ctx, rtv, bars[i], x, 0, std::min(x_end, w), h);
            if (x_end > w) Fill(ctx, rtv, bars[i], 0, 0, x_end - w, h);
        }
    }
    Fill(ctx, rtv, black, 32, 32, 432, 96);
    int64_t v = n;
    for (int d = 11; d >= 0; --d) {
        const int digit = static_cast<int>(v % 10);
        v /= 10;
        const int x0 = 34 + d * 33, y0 = 36;
        for (int s = 0; s < 7; ++s)
            if (kDigitSegs[digit] & (1 << s))
                Fill(ctx, rtv, white, x0 + kSeg[s].left, y0 + kSeg[s].top, x0 + kSeg[s].right, y0 + kSeg[s].bottom);
    }
}

int Generate(platform::D3DDevice& dev, const Job& job) {
    std::printf("%s: %dx%d @ %d fps, %d s, VBR %.1f Mbps\n", job.out.c_str(), job.w, job.h, job.fps, job.seconds, job.bitrate / 1e6);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(job.w);
    d.Height = static_cast<UINT>(job.h);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> bgra, nv12;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(dev.Device()->CreateTexture2D(&d, nullptr, bgra.GetAddressOf()))) return 1;
    if (FAILED(dev.Device()->CreateRenderTargetView(bgra.Get(), nullptr, rtv.GetAddressOf()))) return 1;
    capture::ColorConverter conv;
    capture::ColorConverter::Config cc;
    cc.src_size = SizeI{job.w, job.h};
    cc.dst_size = cc.src_size;
    HRESULT hr = conv.Init(dev, cc);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ColorConverter::Init: %s\n", HrToString(hr).c_str());
        return 1;
    }
    d.Format = DXGI_FORMAT_NV12;
    d.BindFlags = conv.DstBindFlags();
    if (FAILED(dev.Device()->CreateTexture2D(&d, nullptr, nv12.GetAddressOf()))) return 1;

    media::Mp4Writer w;
    media::Mp4Writer::Config c;
    c.path = job.out;
    c.video.size = SizeI{job.w, job.h};
    c.video.fps_num = job.fps;
    c.video.rc = media::RateControl::Vbr;
    c.video.bitrate_bps = job.bitrate;
    c.video.max_bitrate_bps = job.bitrate * 3 / 2;
    media::AudioEncodeConfig a;
    a.label = "tone";
    c.audio.push_back(a);
    hr = w.Init(dev, c, nullptr);
    if (FAILED(hr)) {
        std::fprintf(stderr, "Mp4Writer::Init: %s (%s)\n", HrToString(hr).c_str(), w.ErrorMessage().c_str());
        return 1;
    }
    std::printf("  encoder %s (%s)\n", w.EncoderName().c_str(), w.IsHardware() ? "hardware" : "software");
    if (!w.Start()) return 1;
    ComPtr<ID3D11DeviceContext1> ctx1;
    dev.Context()->QueryInterface(IID_PPV_ARGS(ctx1.GetAddressOf()));
    const int64_t frames = static_cast<int64_t>(job.fps) * job.seconds;
    std::vector<float> pcm;
    int64_t audio_frame = 0;
    double phase = 0;
    for (int64_t n = 0; n < frames; ++n) {
        {
            platform::D3DDevice::Lock lock(dev);
            DrawFrame(ctx1.Get(), rtv.Get(), job.w, job.h, n);
            hr = conv.Convert(dev.Context(), bgra.Get(), nv12.Get());
        }
        if (SUCCEEDED(hr)) hr = w.WriteVideoSync(nv12.Get(), FrameTimeUs(n, job.fps, 1));
        // Audio up to the next frame's start.
        const int64_t end = (n + 1) * 48000 / job.fps;
        const uint32_t count = static_cast<uint32_t>(end - audio_frame);
        pcm.resize(static_cast<size_t>(count) * 2);
        for (uint32_t i = 0; i < count; ++i) {
            const int64_t f = audio_frame + i;
            // Beep: 20 ms (960 frames) of 2 kHz from each flash (every 60th frame = every second at 60 fps).
            const int64_t flash_frame = (f * job.fps / 48000) / 60 * 60;
            const int64_t flash_start = flash_frame * 48000 / job.fps;
            const bool beep = f - flash_start < 960;
            phase += 2.0 * kPi * (beep ? 2000.0 : 1000.0) / 48000.0;
            if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
            const float v = 0.25118864f * static_cast<float>(std::sin(phase));
            pcm[2 * i] = pcm[2 * i + 1] = v;
        }
        if (SUCCEEDED(hr)) hr = w.WriteAudioSync(0, pcm.data(), count, audio_frame * 1'000'000 / 48000);
        audio_frame = end;
        if (FAILED(hr)) {
            std::fprintf(stderr, "frame %lld: %s\n", static_cast<long long>(n), HrToString(hr).c_str());
            w.Finalize();
            return 1;
        }
    }
    hr = w.Finalize();
    if (FAILED(hr)) {
        std::fprintf(stderr, "Finalize: %s\n", w.ErrorMessage().c_str());
        return 1;
    }
    media::ProbeResult p;
    media::StreamScan v;
    if (FAILED(media::Probe(job.out, p)) || FAILED(media::ScanStream(job.out, 0, v))) return 1;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(Utf8ToWide(job.out).c_str(), GetFileExInfoStandard, &fa);
    const uint64_t size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    std::printf("  %llu frames (%llu key), duration %.3f s, %dx%d %s + %d audio, %.2f MB (%.2f Mbps overall)\n",
                static_cast<unsigned long long>(v.samples), static_cast<unsigned long long>(v.keyframes), p.duration_us / 1e6,
                p.video_width, p.video_height, p.video_codec.c_str(), p.audio_streams, size / 1048576.0,
                size * 8.0 / (p.duration_us / 1e6) / 1e6);
    return v.samples == static_cast<uint64_t>(frames) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<Job> jobs;
    Job one;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (s == "--all" && v) {
            const std::string dir = argv[++i];
            jobs.push_back(Job{1920, 1080, 60, 5, 6'000'000, dir + "\\pattern_1080p60_5s.mp4"});
            jobs.push_back(Job{3840, 2160, 60, 3, 14'000'000, dir + "\\pattern_4k60_3s.mp4"});
        } else if (s == "--size" && v && std::sscanf(argv[++i], "%dx%d", &one.w, &one.h) == 2) {
        } else if (s == "--fps" && v) one.fps = std::atoi(argv[++i]);
        else if (s == "--seconds" && v) one.seconds = std::atoi(argv[++i]);
        else if (s == "--bitrate" && v) one.bitrate = static_cast<uint32_t>(std::atoll(argv[++i]));
        else if (s == "--out" && v) one.out = argv[++i];
        else {
            std::fprintf(stderr, "usage: gen_test_media --all <dir> | --size WxH --fps N --seconds N --bitrate bps --out file.mp4\n");
            return 2;
        }
    }
    if (!one.out.empty()) jobs.push_back(one);
    if (jobs.empty()) {
        std::fprintf(stderr, "nothing to do (see --all / --out)\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    platform::Paths::Init();
    Log::Init(platform::Paths::LogsDir().c_str());
    int rc = 0;
    {
        media::MfScope mf;
        platform::D3DDevice dev;
        if (FAILED(dev.Init(false))) return 1;
        for (const Job& j : jobs) rc |= Generate(dev, j);
        dev.Shutdown();
    }
    Log::Shutdown();
    CoUninitialize();
    return rc;
}
