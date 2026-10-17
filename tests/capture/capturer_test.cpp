// ct/capture/screen_capturer.h
#include "test_util.h"

#include <ct/capture/screen_capturer.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;

namespace {

struct RecordingSink : FrameSink {
    platform::TexturePool* pool = nullptr;
    std::vector<TimeUs> times;   // reserved up front; the capture thread only writes
    std::atomic<int> errors{0};
    bool OnFrame(ID3D11Texture2D* nv12, uint32_t index, TimeUs t_us) override {
        (void)nv12;
        if (times.size() < times.capacity()) times.push_back(t_us);
        pool->Release(index);
        return true;
    }
    void OnError(HRESULT) override { errors.fetch_add(1); }
};

void WaitUntil(int64_t qpc) {
    while (Clock::QpcNow() < qpc) Sleep(5);
}

bool InitCapturer(D3DDevice& dev, ScreenCapturer& cap) {
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    if (displays.empty()) return false;
    ScreenCapturer::Config cfg;
    cfg.display = displays[0];
    cfg.region = RectI{};   // whole primary display
    cfg.fps_num = 60;
    return SUCCEEDED(cap.Init(dev, cfg));
}

} // namespace

CT_TEST_DEVICE(screen_capturer_3s_60fps) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    ScreenCapturer cap;
    CT_REQUIRE(InitCapturer(dev, cap));
    CT_CHECK(cap.OutputSize().w > 0 && cap.OutputSize().w % 2 == 0);
    RecordingSink sink;
    sink.pool = &cap.Nv12Pool();
    sink.times.reserve(400);
    const int64_t freq = Clock::QpcFrequency();
    const int64_t epoch = Clock::QpcNow();
    CT_REQUIRE(cap.Start(epoch, sink));
    // Stop half a frame before the 3 s mark: ticks 0..179 have happened.
    WaitUntil(epoch + freq * 3 - freq / 120);
    cap.Stop();
    const CaptureStats& s = cap.Stats();
    std::printf("  ticks %llu new %llu repeats %llu drops pool %llu sink %llu late %llu; acquire avg %.3f ms, convert avg %.3f ms, gpu avg %.3f ms; first frame t=%lld us\n",
                static_cast<unsigned long long>(s.ticks.load()), static_cast<unsigned long long>(s.new_frames.load()),
                static_cast<unsigned long long>(s.repeats.load()), static_cast<unsigned long long>(s.dropped_pool_empty.load()),
                static_cast<unsigned long long>(s.dropped_sink.load()), static_cast<unsigned long long>(s.late_ticks.load()),
                s.acquire.AvgMs(), s.convert.AvgMs(), s.gpu.AvgMs(), static_cast<long long>(s.first_frame_us.load()));
    CT_CHECK(s.ticks.load() >= 178 && s.ticks.load() <= 182);
    CT_CHECK_EQ(s.dropped_pool_empty.load(), 0ull);
    CT_CHECK_EQ(s.dropped_sink.load(), 0ull);
    CT_CHECK(s.new_frames.load() >= 1);
    CT_CHECK_EQ(sink.errors.load(), 0);
    CT_CHECK_EQ(cap.Nv12Pool().InUse(), 0u);
    // Every delivered frame is tick-aligned and consecutive (no late ticks expected on an idle machine).
    CT_REQUIRE(!sink.times.empty());
    CT_CHECK_EQ(sink.times.front(), s.first_frame_us.load());
    int64_t n0 = FrameIndexAt(sink.times.front(), 60, 1);
    CT_CHECK_EQ(FrameTimeUs(n0, 60, 1), sink.times.front());
    if (s.late_ticks.load() == 0) {
        for (size_t i = 0; i < sink.times.size(); ++i) CT_CHECK_EQ(sink.times[i], FrameTimeUs(n0 + static_cast<int64_t>(i), 60, 1));
        CT_CHECK_EQ(sink.times.size() + static_cast<size_t>(n0), static_cast<size_t>(s.ticks.load()));
    }
}

CT_TEST_DEVICE(screen_capturer_pause_resume_gapless) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    ScreenCapturer cap;
    CT_REQUIRE(InitCapturer(dev, cap));
    RecordingSink sink;
    sink.pool = &cap.Nv12Pool();
    sink.times.reserve(400);
    const int64_t freq = Clock::QpcFrequency();
    const int64_t epoch = Clock::QpcNow();
    CT_REQUIRE(cap.Start(epoch, sink));
    WaitUntil(epoch + freq / 2);
    cap.Pause();
    const uint64_t ticks_at_pause = cap.Stats().ticks.load();
    Sleep(400);
    const uint64_t ticks_while_paused = cap.Stats().ticks.load();
    cap.Resume();
    Sleep(500);
    cap.Stop();
    const CaptureStats& s = cap.Stats();
    CT_CHECK(ticks_while_paused <= ticks_at_pause + 1);   // at most the tick that was in flight
    // ~1 s of ticking in total, not 1.4 s.
    CT_CHECK(s.ticks.load() >= 54 && s.ticks.load() <= 66);
    CT_REQUIRE(sink.times.size() > 2);
    int64_t max_step = 0;
    for (size_t i = 1; i < sink.times.size(); ++i) max_step = std::max<int64_t>(max_step, sink.times[i] - sink.times[i - 1]);
    std::printf("  ticks %llu, frames %zu, largest timestamp step %lld us, late %llu\n",
                static_cast<unsigned long long>(s.ticks.load()), sink.times.size(), static_cast<long long>(max_step),
                static_cast<unsigned long long>(s.late_ticks.load()));
    if (s.late_ticks.load() == 0) CT_CHECK(max_step <= 16667);
}

CT_TEST_DEVICE(screen_capturer_region_and_restart) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    CT_REQUIRE(!displays.empty());
    ScreenCapturer cap;
    ScreenCapturer::Config cfg;
    cfg.display = displays[0];
    cfg.region = RectI{displays[0].rect.x + 11, displays[0].rect.y + 13, 333, 201};   // odd: clamped to even
    cfg.fps_num = 30;
    cfg.nv12_pool_size = 4;
    CT_REQUIRE(SUCCEEDED(cap.Init(dev, cfg)));
    CT_CHECK_EQ(cap.OutputSize().w, 332);
    CT_CHECK_EQ(cap.OutputSize().h, 200);
    CT_CHECK_EQ(cap.Nv12Pool().Count(), 4u);
    RecordingSink sink;
    sink.pool = &cap.Nv12Pool();
    sink.times.reserve(100);
    for (int run = 0; run < 2; ++run) {
        sink.times.clear();
        CT_REQUIRE(cap.Start(Clock::QpcNow(), sink));
        Sleep(300);
        cap.Stop();
        CT_CHECK(cap.Stats().ticks.load() >= 7);
        CT_CHECK(!sink.times.empty());
    }
}
