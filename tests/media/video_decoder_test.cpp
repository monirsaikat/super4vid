#include <ct/media/video_decoder.h>

#include "../common/frame_index.h"
#include "../common/nv12_readback.h"
#include "../common/test_device.h"
#include "../common/test_media.h"

#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using namespace ct;
using namespace ct::media;

namespace {

int64_t FrameIndexOf(platform::D3DDevice& dev, const DecodedFrame& f) {
    test::Nv12Image img;
    if (!test::ReadbackNv12(dev, f.nv12, img)) return -2;
    return test::ReadFrameIndexY(img.y.data(), img.width, img.height, img.width);
}

// Seek, then decode forward to the frame shown at t. Returns its index (or < 0) and the elapsed ms.
int64_t SeekToExact(platform::D3DDevice& dev, VideoDecoder& dec, TimeUs t, double& ms) {
    Stopwatch sw;
    sw.Start();
    if (FAILED(dec.Seek(t))) return -3;
    for (;;) {
        DecodedFrame f;
        const HRESULT hr = dec.ReadFrame(f, t);
        if (hr != S_OK) return -4;
        if (f.pts_us <= t && t < f.pts_us + f.duration_us) {
            {
                platform::D3DDevice::Lock lock(dev);
                dev.Context()->Flush();
            }
            ms = sw.ElapsedMs();
            const int64_t idx = FrameIndexOf(dev, f);
            dec.ReleaseFrame(f.pool_index);
            return idx;
        }
        dec.ReleaseFrame(f.pool_index);
        if (f.pts_us > t) return -5;
    }
}

} // namespace

CT_TEST(aperture_box_computation) {
    // No aperture: the whole frame, even.
    RectI b = VideoDecoder::ComputeApertureBox({1920, 1088}, {});
    CT_CHECK(b.x == 0 && b.y == 0 && b.w == 1920 && b.h == 1088);
    // 1920x1088 coded, 1920x1080 shown.
    b = VideoDecoder::ComputeApertureBox({1920, 1088}, {0, 0, 1920, 1080});
    CT_CHECK(b.x == 0 && b.y == 0 && b.w == 1920 && b.h == 1080);
    // Odd sizes round down to even.
    b = VideoDecoder::ComputeApertureBox({1280, 736}, {0, 0, 1279, 721});
    CT_CHECK(b.w == 1278 && b.h == 720);
    // Odd origin moves inward, the box stays inside the aperture.
    b = VideoDecoder::ComputeApertureBox({640, 480}, {1, 3, 600, 400});
    CT_CHECK(b.x == 2 && b.y == 4 && b.w == 598 && b.h == 398);
    // Aperture larger than the frame is clamped.
    b = VideoDecoder::ComputeApertureBox({640, 480}, {0, 0, 4000, 4000});
    CT_CHECK(b.w == 640 && b.h == 480);
    // Odd frame without aperture.
    b = VideoDecoder::ComputeApertureBox({641, 361}, {});
    CT_CHECK(b.w == 640 && b.h == 360);
}

CT_TEST_DEVICE(video_decoder_all_frames_1080p) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    VideoDecoder dec;
    VideoDecoder::Config cfg;
    cfg.path = path;
    CT_REQUIRE(SUCCEEDED(dec.Init(*dev, cfg)));
    CT_CHECK_EQ(dec.Size().w, 1920);
    CT_CHECK_EQ(dec.Size().h, 1080);
    CT_CHECK_EQ(dec.FpsNum() / dec.FpsDen(), 60);
    int count = 0;
    int64_t last_index = -1;
    TimeUs last_pts = -1;
    bool increasing = true;
    for (;;) {
        DecodedFrame f;
        const HRESULT hr = dec.ReadFrame(f);
        CT_REQUIRE(SUCCEEDED(hr));
        if (hr == S_FALSE) break;
        const int64_t idx = FrameIndexOf(*dev, f);
        if (idx <= last_index || f.pts_us <= last_pts) increasing = false;
        if (count == 0) CT_CHECK_EQ(idx, 0);
        last_index = idx;
        last_pts = f.pts_us;
        dec.ReleaseFrame(f.pool_index);
        ++count;
    }
    CT_CHECK_EQ(count, 300);
    CT_CHECK_EQ(last_index, 299);
    CT_CHECK(increasing);
    // The first frame is a keyframe; seek landings add more (2 s GOP: a seek to 3.1 s lands on 2.0 s).
    CT_REQUIRE(dec.KnownKeyframes().size() == 1u);
    CT_CHECK_EQ(dec.KnownKeyframes()[0], 0);
    CT_REQUIRE(SUCCEEDED(dec.Seek(3'100'000)));
    DecodedFrame k;
    CT_REQUIRE(dec.ReadFrame(k, 3'100'000) == S_OK);
    dec.ReleaseFrame(k.pool_index);
    CT_REQUIRE(dec.KnownKeyframes().size() == 2u);
    CT_CHECK_EQ(dec.KnownKeyframes()[1], 2'000'000);
    std::printf("    keyframes known after a full decode: %zu\n",dec.KnownKeyframes().size());
    std::printf("    hardware: %d\n", dec.IsHardware() ? 1 : 0);
}

CT_TEST_DEVICE(video_decoder_seek_exact_1080p) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    VideoDecoder dec;
    VideoDecoder::Config cfg;
    cfg.path = path;
    CT_REQUIRE(SUCCEEDED(dec.Init(*dev, cfg)));
    double ms = 0;
    CT_CHECK_EQ(SeekToExact(*dev, dec, 2'500'000, ms), 150);
    CT_CHECK(ms < 400.0);   // 30 frames after the keyframe: 52-70 ms in Release, up to ~160 ms with the debug layer
    // Random exact seeks, p95 reported (budget 150 ms).
    std::mt19937 rng(1234);
    std::vector<double> times;
    for (int i = 0; i < 20; ++i) {
        const int64_t frame = std::uniform_int_distribution<int64_t>(0, 299)(rng);
        const TimeUs t = FrameTimeUs(frame, 60, 1) + 1000;
        double t_ms = 0;
        const int64_t idx = SeekToExact(*dev, dec, t, t_ms);
        CT_CHECK_EQ(idx, frame);
        times.push_back(t_ms);
    }
    std::sort(times.begin(), times.end());
    std::printf("    first seek %.1f ms; 20 random seeks p50 %.1f ms p95 %.1f ms max %.1f ms\n", ms, times[10], times[18], times[19]);
    // The 150 ms budget is reported, 400 ms enforced: on the reference iGPU the DXVA H.264 decoder runs ~1.8 ms per
    // 1080p frame independent of bitrate, so a seek to the end of a 2 s GOP (119 frames) takes ~210 ms
    // (DECISIONS.md P06 6.2). The MediaCache prefetch window hides this while playing and scrubbing nearby.
    if (times[18] >= 150.0) std::printf("    WARNING: p95 %.1f ms is over the 150 ms budget\n", times[18]);
    CT_CHECK(times[18] < 400.0);
}

CT_TEST_DEVICE(video_decoder_software_path) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    VideoDecoder dec;
    VideoDecoder::Config cfg;
    cfg.path = path;
    cfg.allow_hardware = false;
    cfg.pool_size = 2;
    CT_REQUIRE(SUCCEEDED(dec.Init(*dev, cfg)));
    CT_CHECK_EQ(dec.Size().w, 1920);
    CT_CHECK_EQ(dec.Size().h, 1080);
    for (int i = 0; i < 5; ++i) {
        DecodedFrame f;
        CT_REQUIRE(dec.ReadFrame(f) == S_OK);
        CT_CHECK_EQ(FrameIndexOf(*dev, f), i);
        dec.ReleaseFrame(f.pool_index);
    }
    CT_CHECK(!dec.IsHardware());
    if (test::Verbose()) {
        dec.Seek(FrameTimeUs(200, 60, 1));
        for (int i = 0; i < 6; ++i) {
            DecodedFrame f;
            if (dec.ReadFrame(f) != S_OK) break;
            std::printf("    after seek: pts %lld dur %lld index %lld\n", static_cast<long long>(f.pts_us),
                        static_cast<long long>(f.duration_us), static_cast<long long>(FrameIndexOf(*dev, f)));
            dec.ReleaseFrame(f.pool_index);
        }
    }
    double ms = 0;
    CT_CHECK_EQ(SeekToExact(*dev, dec, FrameTimeUs(200, 60, 1), ms), 200);
}

CT_TEST_DEVICE(video_decoder_pool_exhaustion_is_pending) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    VideoDecoder dec;
    VideoDecoder::Config cfg;
    cfg.path = path;
    cfg.pool_size = 2;
    CT_REQUIRE(SUCCEEDED(dec.Init(*dev, cfg)));
    DecodedFrame a, b, c;
    CT_REQUIRE(dec.ReadFrame(a) == S_OK);
    CT_REQUIRE(dec.ReadFrame(b) == S_OK);
    CT_CHECK(dec.ReadFrame(c) == E_PENDING);
    dec.ReleaseFrame(a.pool_index);
    CT_REQUIRE(dec.ReadFrame(c) == S_OK);
    CT_CHECK_EQ(FrameIndexOf(*dev, c), 2);   // nothing was lost while the pool was full
    dec.ReleaseFrame(b.pool_index);
    dec.ReleaseFrame(c.pool_index);
}

CT_TEST_DEVICE(video_decoder_eos_and_rewind) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_4k60_3s.mp4");
    VideoDecoder dec;
    VideoDecoder::Config cfg;
    cfg.path = path;
    CT_REQUIRE(SUCCEEDED(dec.Init(*dev, cfg)));
    CT_CHECK_EQ(dec.Size().w, 3840);
    CT_CHECK_EQ(dec.Size().h, 2160);
    double ms = 0;
    CT_CHECK_EQ(SeekToExact(*dev, dec, FrameTimeUs(179, 60, 1), ms), 179);
    DecodedFrame f;
    CT_CHECK(dec.ReadFrame(f) == S_FALSE);
    CT_CHECK(dec.AtEnd());
    CT_CHECK_EQ(SeekToExact(*dev, dec, 0, ms), 0);
}

namespace {
void ThroughputReport(platform::D3DDevice* dev, const std::string& path, bool hw) {
    VideoDecoder dec;
    VideoDecoder::Config cfg;
    cfg.path = path;
    cfg.allow_hardware = hw;
    if (FAILED(dec.Init(*dev, cfg))) return;
    Stopwatch sw;
    sw.Start();
    int n = 0;
    for (;;) {
        DecodedFrame f;
        const HRESULT hr = dec.ReadFrame(f);
        if (hr != S_OK) break;
        dec.ReleaseFrame(f.pool_index);
        ++n;
    }
    const double all_ms = sw.ElapsedMs();
    double key_ms = 0, mid_ms = 0, end_ms = 0;
    SeekToExact(*dev, dec, FrameTimeUs(120, 60, 1), key_ms);
    SeekToExact(*dev, dec, FrameTimeUs(180, 60, 1), mid_ms);
    FILETIME c0, e0, k0, u0, k1, u1;
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
    SeekToExact(*dev, dec, FrameTimeUs(239, 60, 1), end_ms);
    GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k1, &u1);
    auto ft = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 10000.0; };
    std::printf("    cpu during the +119 seek: %.1f ms (kernel %.1f)\n", ft(k1) + ft(u1) - ft(k0) - ft(u0), ft(k1) - ft(k0));
    std::printf("    %s: %d frames in %.1f ms (%.0f fps); seek to keyframe %.1f ms, +60 frames %.1f ms, +119 frames %.1f ms\n",
                hw ? "hw" : "sw", n, all_ms, n * 1000.0 / all_ms, key_ms, mid_ms, end_ms);
}
} // namespace

CT_BENCH(video_decoder_throughput_report, 1) {   // --bench only: ~7 s
    (void)CT_BENCH_N;
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    ThroughputReport(dev, path, true);
    ThroughputReport(dev, path, false);
    // Two hardware decoders at once: tells whether one decoder is latency-bound (aggregate fps doubles).
    double ms[2] = {0, 0};
    Thread threads[2];
    for (int i = 0; i < 2; ++i) {
        threads[i].Start({"ct-test-dec"}, [&, i] {
            VideoDecoder dec;
            VideoDecoder::Config cfg;
            cfg.path = path;
            if (FAILED(dec.Init(*dev, cfg))) return;
            Stopwatch sw;
            sw.Start();
            for (;;) {
                DecodedFrame f;
                if (dec.ReadFrame(f, INT64_MAX) != S_OK) break;
                dec.ReleaseFrame(f.pool_index);
            }
            ms[i] = sw.ElapsedMs();
        });
    }
    threads[0].Join();
    threads[1].Join();
    std::printf("    two parallel hw decoders (no copies): %.1f ms and %.1f ms for 300 frames each\n", ms[0], ms[1]);
}
