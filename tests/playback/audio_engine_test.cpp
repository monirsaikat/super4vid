#include <ct/playback/audio_engine.h>

#include <ct/core/time.h>
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include "../common/timing.h"

#include <atomic>
#include <cmath>
#include <cstdio>

using namespace ct;
using namespace ct::playback;

namespace {

// A quiet 1 kHz tone (-40 dBFS: the test runs on the default speakers) that checks the from_us contiguity.
struct ToneSource final : AudioEngine::Source {
    std::atomic<int64_t> frames{0};
    std::atomic<int> discontinuities{0};
    TimeUs expect_from = -1;
    int64_t phase = 0;
    uint32_t Pull(float* out, uint32_t n, TimeUs from_us, double) override {
        if (expect_from >= 0 && std::llabs(from_us - expect_from) > 1) discontinuities.fetch_add(1);
        for (uint32_t i = 0; i < n; ++i) {
            const float v = 0.01f * static_cast<float>(std::sin(2 * 3.14159265358979 * 1000.0 * (phase + i) / 48000.0));
            out[2 * i] = out[2 * i + 1] = v;
        }
        phase += n;
        frames.fetch_add(n);
        expect_from = from_us + static_cast<TimeUs>(n) * 1'000'000 / 48000;
        return n;
    }
};

struct Fixture {
    audio::AudioDevices devices;
    AudioEngine engine;
    ToneSource tone;
    bool ok = false;
    Fixture() {
        ok = SUCCEEDED(devices.Init()) && SUCCEEDED(engine.Init(devices)) && engine.Start(tone);
    }
};

} // namespace

CT_TEST_DEVICE(audio_engine_position_tracks_500ms) {
    Fixture fx;
    if (!fx.ok) CT_SKIP("no render endpoint");
    CT_CHECK_EQ(fx.engine.PositionUs(), 0);
    const int64_t q_play = Clock::QpcNow();
    fx.engine.Play(1'000'000, 1.0);
    CT_CHECK(fx.engine.Playing());
    CT_CHECK_EQ(fx.engine.PositionUs(), 1'000'000);   // before the device plays anything
    Sleep(200);
    const int64_t q0 = Clock::QpcNow();
    const TimeUs p0 = fx.engine.PositionUs();
    // Monotonic while playing.
    TimeUs last = p0;
    bool monotonic = true;
    // Smoothness: position vs QPC deviation (a stair-stepped clock would make video frames repeat and skip).
    double max_dev_ms = 0;
    while ((Clock::QpcNow() - q0) * 1000 / Clock::QpcFrequency() < 500) {
        const TimeUs p = fx.engine.PositionUs();
        const double wall = static_cast<double>(Clock::QpcNow() - q0) * 1e6 / Clock::QpcFrequency();
        max_dev_ms = std::max(max_dev_ms, std::fabs((p - p0) - wall) / 1000.0);
        monotonic &= p >= last;
        last = p;
        Sleep(1);
    }
    std::printf("    max |position - wall| while playing: %.2f ms\n", max_dev_ms);
    CT_CHECK(max_dev_ms < test::TimingLimit(8.0, 50.0));
    const TimeUs p1 = fx.engine.PositionUs();
    const double wall_ms = static_cast<double>(Clock::QpcNow() - q0) * 1000.0 / Clock::QpcFrequency();
    const double adv_ms = (p1 - p0) / 1000.0;
    std::printf("    p0 %.1f ms after 200 ms (latency %.1f ms), advanced %.1f ms over %.1f ms wall, peak %.4f\n",
                (p0 - 1'000'000) / 1000.0, fx.engine.LatencyMs(), adv_ms, wall_ms, fx.engine.PeakLevel());
    CT_CHECK(monotonic);
    CT_CHECK_NEAR(adv_ms, wall_ms, 30.0);
    // p0 trails the time since Play by the output latency (+ one buffer until the render thread applied Play).
    const double since_play_ms = static_cast<double>(q0 - q_play) * 1000.0 / Clock::QpcFrequency();
    CT_CHECK((p0 - 1'000'000) / 1000.0 <= since_play_ms && (p0 - 1'000'000) / 1000.0 >= since_play_ms - 80.0);
    CT_CHECK_EQ(fx.tone.discontinuities.load(), 0);
    // Pause freezes the position.
    fx.engine.Pause();
    const TimeUs paused = fx.engine.PositionUs();
    Sleep(100);
    CT_CHECK_EQ(fx.engine.PositionUs(), paused);
    CT_CHECK(!fx.engine.Playing());
    CT_CHECK_NEAR(paused, p1, 30'000);
}

CT_TEST_DEVICE(audio_engine_varispeed_rate_2) {
    Fixture fx;
    if (!fx.ok) CT_SKIP("no render endpoint");
    fx.engine.Play(0, 2.0);
    Sleep(200);
    const int64_t f0 = fx.tone.frames.load();
    const int64_t q0 = Clock::QpcNow();
    const TimeUs p0 = fx.engine.PositionUs();
    Sleep(500);
    const TimeUs p1 = fx.engine.PositionUs();
    const double wall_ms = static_cast<double>(Clock::QpcNow() - q0) * 1000.0 / Clock::QpcFrequency();
    const int64_t f1 = fx.tone.frames.load();
    const double adv_ms = (p1 - p0) / 1000.0;
    const double pulled_ms = (f1 - f0) * 1000.0 / 48000.0;
    std::printf("    rate 2: advanced %.1f ms over %.1f ms wall, pulled %.1f ms of source\n", adv_ms, wall_ms, pulled_ms);
    CT_CHECK_NEAR(adv_ms, 2 * wall_ms, 60.0);
    CT_CHECK_NEAR(pulled_ms, 2 * wall_ms, 60.0);
    CT_CHECK_EQ(fx.tone.discontinuities.load(), 0);
    // Volume ramps without touching the clock.
    fx.engine.SetVolume(0.5f);
    fx.engine.SetMuted(true);
    Sleep(60);
    CT_CHECK(fx.engine.PeakLevel() < 1e-6f);
    fx.engine.Pause();
}
