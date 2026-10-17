#include <ct/playback/playback_clock.h>
#include <ct/playback/player.h>

#include "../common/frame_index.h"
#include "../common/nv12_readback.h"
#include "../common/test_device.h"
#include "../common/test_media.h"

#include <ct/core/time.h>
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <cstdio>

using namespace ct;
using namespace ct::playback;

namespace {
int64_t g_fake_qpc = 0;
int64_t FakeQpc() { return g_fake_qpc; }
constexpr int64_t kFreq = 10'000'000;
void Advance(TimeUs us) { g_fake_qpc += us * kFreq / 1'000'000; }
} // namespace

CT_TEST(playback_clock_qpc_math) {
    PlaybackClock c;
    c.SetQpcSourceForTest(&FakeQpc, kFreq);
    g_fake_qpc = 123'456'789;
    CT_CHECK_EQ(c.Now(), 0);
    CT_CHECK(!c.Playing());
    c.Play(1'000'000, 1.0);
    Advance(500'000);
    CT_CHECK_EQ(c.Now(), 1'500'000);
    c.SetRate(2.0);   // keeps the position, then runs twice as fast
    CT_CHECK_EQ(c.Now(), 1'500'000);
    Advance(250'000);
    CT_CHECK_EQ(c.Now(), 2'000'000);
    c.Pause();
    Advance(1'000'000);
    CT_CHECK_EQ(c.Now(), 2'000'000);
    c.Seek(300'000);   // paused seek
    CT_CHECK_EQ(c.Now(), 300'000);
    CT_CHECK(!c.Playing());
    c.Play(c.Now(), -1.0);   // reverse
    Advance(100'000);
    CT_CHECK_EQ(c.Now(), 200'000);
    c.Seek(1'000'000);       // seek while playing keeps playing
    CT_CHECK(c.Playing());
    Advance(100'000);
    CT_CHECK_EQ(c.Now(), 900'000);
    CT_CHECK(!c.UsingAudio());
    // Rate clamps.
    CT_CHECK_EQ(PlaybackClock::ClampRate(10.0), 4.0);
    CT_CHECK_EQ(PlaybackClock::ClampRate(0.1), 0.25);
    CT_CHECK_EQ(PlaybackClock::ClampRate(-8.0), -4.0);
    CT_CHECK_EQ(PlaybackClock::ClampRate(0.0), 1.0);
}

CT_TEST_DEVICE(player_plays_in_sync_and_steps) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    audio::AudioDevices devices;
    devices.Init();
    Player player;
    CT_REQUIRE(SUCCEEDED(player.Open(*dev, devices, path)));
    std::printf("    audio clock: %d\n", player.HasAudio() ? 1 : 0);
    FrameRef f;
    TimeUs now = 0;
    // Paused at 0: frame 0 arrives.
    Stopwatch sw;
    sw.Start();
    while (!player.Tick(f, now) && sw.ElapsedMs() < 2000) Sleep(2);
    CT_REQUIRE(f.nv12 != nullptr);
    player.Play();
    CT_CHECK_EQ(player.ClockUsesAudio(), player.HasAudio());
    player.ResetStats();
    const uint64_t misses0 = player.Cache().GetStats().misses;
    // ~1.5 s of 60 Hz ticks.
    int mismatches = 0, checked = 0, ticks = 0;
    const int64_t q0 = Clock::QpcNow();
    int64_t next = q0;
    while ((Clock::QpcNow() - q0) * 1000 / Clock::QpcFrequency() < 1500) {
        next += Clock::QpcFrequency() / 60;
        while (Clock::QpcNow() < next) Sleep(1);
        ++ticks;
        if (player.Tick(f, now) && checked < 12 && (now / 100'000) % 3 == 0) {
            const int64_t expect = FrameIndexAt(now, 60, 1);
            test::Nv12Image img;
            if (test::ReadbackNv12(*dev, f.nv12, img)) {
                const int64_t idx = test::ReadFrameIndexY(img.y.data(), img.width, img.height, img.width);
                if (idx < expect - 1 || idx > expect) ++mismatches;   // the shown frame may lag by one tick
                ++checked;
            }
        }
    }
    const Player::Stats st = player.GetStats();
    std::printf("    1.5 s: clock at %.3f s, %llu frames shown, %llu late, cache %zu MB, %d/%d index mismatches\n", now / 1e6,
                static_cast<unsigned long long>(st.frames_shown), static_cast<unsigned long long>(st.frames_late),
                st.cache_bytes >> 20, mismatches, checked);
    CT_CHECK(now > 1'300'000 && now < 1'700'000);
    // Ticks are not phase-locked to the 60 fps frame boundaries (Sleep jitter), so some ticks repeat a frame and the
    // next skips one; what matters is that every tick found its exact frame in the cache.
    const uint64_t misses = player.Cache().GetStats().misses - misses0;
    std::printf("    cache misses while playing: %llu\n",static_cast<unsigned long long>(misses));
#ifndef CT_DEBUG
    // Real-time expectations: enforced in Release only (Debug + debug layer is report-only).
    CT_CHECK(misses <= 1);
    CT_CHECK(st.frames_shown * 10 >= static_cast<uint64_t>(ticks) * 7);
    CT_CHECK(st.frames_late <= 1);
#endif
    std::printf("    %d ticks\n", ticks);
    CT_CHECK_EQ(mismatches, 0);
    // Step: pauses and moves exactly one frame.
    player.StepFrames(1);
    CT_CHECK(!player.Playing());
    TimeUs before = 0;
    player.Tick(f, before);
    const int64_t i0 = FrameIndexAt(before, 60, 1);
    player.StepFrames(5);
    TimeUs after = 0;
    sw.Start();
    bool exact_frame = false;
    while (sw.ElapsedMs() < 2000) {
        if (player.Tick(f, after) && f.pts_us <= after && after < f.pts_us + f.duration_us + 1000) {
            exact_frame = true;
            break;
        }
        Sleep(2);
    }
    CT_CHECK(exact_frame);
    CT_CHECK_EQ(FrameIndexAt(after, 60, 1), i0 + 5);
    // Seek far: measured by the player.
    player.ResetStats();
    player.Seek(4'000'000);
    sw.Start();
    while (sw.ElapsedMs() < 2000) {
        player.Tick(f, now);
        if (player.GetStats().seek_to_frame_ms > 0) break;
        Sleep(1);
    }
    std::printf("    seek to 4.0 s -> exact frame in %.1f ms\n", player.GetStats().seek_to_frame_ms);
    CT_CHECK(player.GetStats().seek_to_frame_ms > 0 && player.GetStats().seek_to_frame_ms < 400);
    // Play to the end: stops there.
    player.Seek(4'900'000);
    player.Play();
    sw.Start();
    while (player.Playing() && sw.ElapsedMs() < 2000) {
        player.Tick(f, now);
        Sleep(5);
    }
    CT_CHECK(!player.Playing());
    CT_CHECK(now > 4'950'000);
}
