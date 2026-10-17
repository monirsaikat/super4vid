#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/recorder/session_clock.h>
#include <ct/test/ct_test.h>

#include <atomic>
#include <cstdio>

using namespace ct;
using namespace ct::recorder;

namespace {
int64_t Sec(double s) { return static_cast<int64_t>(s * static_cast<double>(Clock::QpcFrequency())); }
} // namespace

CT_TEST(session_clock_pause_math) {
    SessionClock c;
    const int64_t e = 1'000'000'000;   // arbitrary epoch
    c.Start(e);
    CT_CHECK_NEAR(c.QpcToMediaUs(e), 0, 1);
    CT_CHECK_NEAR(c.QpcToMediaUs(e + Sec(1)), 1'000'000, 1);
    CT_CHECK_NEAR(c.QpcToMediaUs(e - Sec(1)), -1'000'000, 1);

    c.PauseAt(e + Sec(2));
    CT_CHECK(c.IsPaused());
    // The clock stands still at the pause start, for any qpc inside the pause.
    CT_CHECK_NEAR(c.QpcToMediaUs(e + Sec(3)), 2'000'000, 1);
    CT_CHECK_NEAR(c.QpcToMediaUs(e + Sec(4.5)), 2'000'000, 1);
    // Earlier timestamps (an event taken just before the pause) still map normally.
    CT_CHECK_NEAR(c.QpcToMediaUs(e + Sec(1.5)), 1'500'000, 1);
    c.PauseAt(e + Sec(2.5));   // ignored while paused
    c.ResumeAt(e + Sec(5));
    CT_CHECK(!c.IsPaused());
    CT_CHECK_NEAR(c.TotalPausedUs(), 3'000'000, 1);
    CT_CHECK_NEAR(c.QpcToMediaUs(e + Sec(6)), 3'000'000, 1);
    c.ResumeAt(e + Sec(7));   // ignored while running
    CT_CHECK_NEAR(c.TotalPausedUs(), 3'000'000, 1);

    c.PauseAt(e + Sec(8));    // media 5 s
    c.ResumeAt(e + Sec(8.25));
    CT_CHECK_NEAR(c.QpcToMediaUs(e + Sec(10)), 6'750'000, 1);
    CT_REQUIRE(c.Pauses().size() == 2);
    CT_CHECK_NEAR(c.Pauses()[0].media_t_us, 2'000'000, 1);
    CT_CHECK_NEAR(c.Pauses()[0].duration_us, 3'000'000, 1);
    CT_CHECK_NEAR(c.Pauses()[1].media_t_us, 5'000'000, 1);
    CT_CHECK_NEAR(c.Pauses()[1].duration_us, 250'000, 1);

    // WASAPI positions: QPC in 100 ns units.
    const int64_t q = e + Sec(10);
    const uint64_t hns = static_cast<uint64_t>(static_cast<double>(q) * 1e7 / static_cast<double>(Clock::QpcFrequency()));
    CT_CHECK_NEAR(c.HnsToMediaUs(hns), 6'750'000, 2);

    c.Start(e);   // a new session clears everything
    CT_CHECK_NEAR(c.TotalPausedUs(), 0, 1);
    CT_CHECK(c.Pauses().empty());
    CT_CHECK_NEAR(c.QpcToMediaUs(e + Sec(1)), 1'000'000, 1);
}

CT_TEST(session_clock_monotonic_across_transitions) {
    // Readers on other threads must never see time go backwards while pause/resume flips (seqlock).
    SessionClock c;
    c.Start(Clock::QpcNow());
    std::atomic<bool> stop{false};
    std::atomic<int64_t> regressions{0}, reads{0};
    Thread reader;
    reader.Start({}, [&] {
        TimeUs last = c.NowMediaUs();
        while (!stop.load(std::memory_order_relaxed)) {
            const TimeUs t = c.NowMediaUs();
            if (t < last) regressions.fetch_add(1);
            last = t;
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });
    // At least 2000 cycles and until the reader has had real overlap with them (it may start late on a busy box).
    int cycles = 0;
    const int64_t give_up = Clock::QpcNow() + 5 * Clock::QpcFrequency();
    while (cycles < 2000 || (reads.load() < 100000 && Clock::QpcNow() < give_up)) {
        c.Pause();
        c.Resume();
        ++cycles;
    }
    stop.store(true);
    reader.Join();
    std::printf("  %lld reads during %d pause/resume cycles, paused total %lld us\n",
                static_cast<long long>(reads.load()), cycles,
                static_cast<long long>(c.TotalPausedUs()));
    CT_CHECK_EQ(regressions.load(), 0);
    CT_CHECK_EQ(c.Pauses().size(), static_cast<size_t>(cycles));
    CT_CHECK(reads.load() > 0);
}
