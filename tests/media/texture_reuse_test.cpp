// ct/media/mp4_writer.h: texture pool reuse with repeated frames, the synchronous export path, and real capture.
#include "media_test_util.h"

#include <ct/capture/display.h>
#include <ct/capture/screen_capturer.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/mf_runtime.h>
#include <ct/media/mp4_writer.h>
#include <ct/media/probe.h>
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace ct;
using namespace ct::media;
using namespace ct::media_test;
using ct::platform::D3DDevice;
using ct::platform::TexturePool;

namespace {

ULONG BufferRefs(TexturePool& pool, uint32_t index) {
    auto* b = static_cast<IUnknown*>(pool.UserData(index));
    if (!b) return 0;
    b->AddRef();
    return b->Release();
}

} // namespace

// One texture, sent 10 times in a row (a repeat burst), must not be handed out for writing until every sample that
// references it was encoded; frames written after the reuse must not leak into the earlier ones.
CT_TEST_DEVICE(texture_reuse_waits_for_every_repeat) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    TexturePool pool;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 1920, 1080, 1, true, nullptr));   // a single texture: reuse is forced
    const std::string path = TempPath("reuse_repeats.mp4");
    Mp4Writer::Config c;
    c.path = path;
    c.video.size = SizeI{1920, 1080};
    c.video.fps_num = 60;
    c.video_queue = 16;
    Mp4Writer w;
    CT_REQUIRE(SUCCEEDED(w.Init(dev, c, &pool)));
    CT_REQUIRE(w.Start());

    int64_t n = 0;
    int blocked_polls = 0, max_refs = 0;
    double reuse_ms[2] = {0, 0};
    const uint8_t luma[2] = {40, 200};
    for (int burst = 0; burst < 2; ++burst) {
        // Acquire for writing: only possible once the encoder released every sample of the previous burst.
        Stopwatch sw;
        sw.Start();
        uint32_t idx = 0;
        ID3D11Texture2D* tex = nullptr;
        while (!(tex = pool.Acquire(idx)) && sw.ElapsedMs() < 2000) {
            const ULONG rc = BufferRefs(pool, 0);
            if (pool.InUse() == 0 && rc > 1) ++blocked_polls;   // released by the writer, still held by MF
            max_refs = std::max(max_refs, static_cast<int>(rc));
            Sleep(0);
        }
        reuse_ms[burst] = sw.ElapsedMs();
        CT_REQUIRE(tex != nullptr);
        CT_CHECK_EQ(BufferRefs(pool, idx), 1ul);   // nobody but the slot table holds the buffer
        CT_REQUIRE(FillNv12(dev, tex, luma[burst]));
        CT_REQUIRE(w.OnFrame(tex, idx, FrameTimeUs(n++, 60, 1)));
        for (int k = 1; k < 10; ++k) {   // repeats: the same texture again, one hand-out each
            pool.AddRef(idx);
            CT_REQUIRE(w.OnFrame(tex, idx, FrameTimeUs(n++, 60, 1)));
        }
    }
    CT_CHECK_HR(w.Finalize());
    std::printf("  waited %.2f / %.2f ms for the texture; %d polls saw it released by the writer but held by MF (max refs %d)\n",
                reuse_ms[0], reuse_ms[1], blocked_polls, max_refs);
    CT_CHECK(blocked_polls > 0);   // the predicate, not the hand-out count, kept the texture out of circulation
    CT_CHECK(max_refs > 2);        // several samples referenced the same buffer at once
    CT_CHECK_EQ(w.GetStats().video_written.load(), 20ull);
    CT_CHECK_EQ(pool.InUse(), 0u);

    StreamScan v;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_CHECK_EQ(v.samples, 20ull);
    std::vector<double> means;
    CT_REQUIRE(DecodeLumaMeans(path, 20, means));
    CT_REQUIRE(means.size() == 20);
    for (size_t i = 0; i < 20; ++i) CT_CHECK_NEAR(means[i], luma[i / 10], 3.0);
}

// Export path: the caller overwrites its one texture right after every WriteVideoSync; the file must still hold every
// frame's own content (the writer copies into its internal pool).
CT_TEST_DEVICE(write_video_sync_copies_the_caller_texture) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    TexturePool src;
    CT_REQUIRE(MakeNv12Pool(dev, src, 1280, 720, 1, false, nullptr));
    uint32_t idx = 0;
    ID3D11Texture2D* tex = src.Acquire(idx);
    CT_REQUIRE(tex != nullptr);
    const std::string path = TempPath("sync_export.mp4");
    Mp4Writer::Config c;
    c.path = path;
    c.video.size = SizeI{1280, 720};
    c.video.fps_num = 30;
    c.video.rc = RateControl::Vbr;
    c.video.bitrate_bps = 4'000'000;
    c.audio.push_back(AudioEncodeConfig{});
    c.fast_start = true;   // ignored (unsupported); must not break the file
    Mp4Writer w;
    CT_REQUIRE(SUCCEEDED(w.Init(dev, c, nullptr)));
    CT_REQUIRE(w.Start());
    std::vector<float> pcm(1600 * 2);
    Stopwatch sw;
    sw.Start();
    for (int i = 0; i < 90; ++i) {
        CT_REQUIRE(FillNv12(dev, tex, static_cast<uint8_t>(20 + (i * 13) % 200)));
        CT_REQUIRE(SUCCEEDED(w.WriteVideoSync(tex, FrameTimeUs(i, 30, 1))));
        Sine(pcm.data(), 1600, 1000.0, 0.25f, i * 1600ll);
        CT_REQUIRE(SUCCEEDED(w.WriteAudioSync(0, pcm.data(), 1600, i * 1600ll * 1'000'000 / 48000)));
    }
    const double feed_ms = sw.ElapsedMs();
    // Wrong size or format is refused.
    TexturePool other;
    CT_REQUIRE(MakeNv12Pool(dev, other, 640, 360, 1, false, nullptr));
    CT_CHECK_EQ(w.WriteVideoSync(other.At(0), FrameTimeUs(90, 30, 1)), E_INVALIDARG);
    CT_CHECK_HR(w.Finalize());
    std::printf("  90 frames + 3 s audio queued in %.0f ms (%.1fx real time), finalize %lld ms\n", feed_ms,
                3000.0 / feed_ms, static_cast<long long>(w.GetStats().finalize_ms.load()));
    CT_CHECK(FAILED(w.WriteVideoSync(tex, 0)));   // after Finalize
    ProbeResult p;
    CT_REQUIRE(SUCCEEDED(Probe(path, p)));
    CT_CHECK_NEAR(p.duration_us, 3'000'000, 50'000);
    CT_CHECK_EQ(p.audio_streams, 1);
    StreamScan v;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_CHECK_EQ(v.samples, 90ull);
    std::vector<double> means;
    CT_REQUIRE(DecodeLumaMeans(path, 90, means));
    std::printf("  decoded %zu frames\n", means.size());
    CT_REQUIRE(means.size() == 90);
    int bad = 0;
    for (int i = 0; i < 90; ++i)
        if (std::abs(means[static_cast<size_t>(i)] - (20 + (i * 13) % 200)) > 3.0) ++bad;
    CT_CHECK_EQ(bad, 0);
    src.Release(idx);
}

// Real capture: ScreenCapturer (compute converter, UAV-bound NV12 pool, repeats) straight into the writer.
CT_TEST_DEVICE(capture_into_writer_3s) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    std::vector<capture::DisplayInfo> displays = capture::EnumerateDisplays(dev.Factory());
    if (displays.empty()) CT_SKIP("no display");
    capture::ScreenCapturer cap;
    capture::ScreenCapturer::Config cc;
    cc.display = displays[0];
    cc.fps_num = 60;
    CT_REQUIRE(SUCCEEDED(cap.Init(dev, cc)));
    const D3D11_TEXTURE2D_DESC& d = cap.Nv12Pool().Desc();
    const std::string path = TempPath("capture_3s.mp4");
    Mp4Writer::Config c;
    c.path = path;
    c.video.size = cap.OutputSize();
    c.video.fps_num = 60;
    Mp4Writer w;
    CT_REQUIRE(SUCCEEDED(w.Init(dev, c, &cap.Nv12Pool())));
    CT_REQUIRE(w.Start());
    const int64_t epoch = Clock::QpcNow();
    CT_REQUIRE(cap.Start(epoch, w));
    Sleep(3000);
    cap.Stop();
    CT_CHECK_HR(w.Finalize());
    const capture::CaptureStats& s = cap.Stats();
    const auto& ws = w.GetStats();
    std::printf("  %ux%u NV12 bind 0x%x (UAV %s), encoder %s; ticks %llu new %llu repeats %llu drop pool %llu sink %llu late %llu;\n"
                "  written %llu, latency avg %.3f max %.3f ms, WriteSample avg %.3f max %.3f ms, finalize %lld ms\n",
                d.Width, d.Height, d.BindFlags, (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ? "yes" : "no", w.EncoderName().c_str(),
                static_cast<unsigned long long>(s.ticks.load()), static_cast<unsigned long long>(s.new_frames.load()),
                static_cast<unsigned long long>(s.repeats.load()), static_cast<unsigned long long>(s.dropped_pool_empty.load()),
                static_cast<unsigned long long>(s.dropped_sink.load()), static_cast<unsigned long long>(s.late_ticks.load()),
                static_cast<unsigned long long>(ws.video_written.load()), ws.video_latency.AvgMs(), ws.video_latency.MaxMs(),
                ws.video_write.AvgMs(), ws.video_write.MaxMs(), static_cast<long long>(ws.finalize_ms.load()));
    CT_CHECK_EQ(s.dropped_pool_empty.load(), 0ull);
    CT_CHECK_EQ(s.dropped_sink.load(), 0ull);
    CT_CHECK_EQ(ws.video_rejected.load(), 0ull);
    CT_CHECK_EQ(cap.Nv12Pool().InUse(), 0u);
    const uint64_t delivered = s.new_frames.load() + s.repeats.load();
    CT_CHECK_EQ(ws.video_written.load(), delivered);
    StreamScan v;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_CHECK_EQ(v.samples, delivered);
    CT_CHECK_EQ(v.first_us, s.first_frame_us.load());   // the first tick's time is kept (no shift to 0)
    std::printf("  file: %llu frames, first %lld us, max gap %lld us\n", static_cast<unsigned long long>(v.samples),
                static_cast<long long>(v.first_us), static_cast<long long>(v.max_gap_us));
    CT_CHECK(v.max_gap_us <= 16'667 || s.late_ticks.load() > 0);
}
