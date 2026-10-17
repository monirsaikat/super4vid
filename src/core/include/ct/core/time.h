#pragma once
// ct/core/time.h
// QPC clock with exact 128-bit conversions, tick-aligned frame times, a high-resolution waitable timer,
// a stopwatch and wall-clock stamps. Internal media time is int64_t microseconds (TimeUs).
// Thread model: Clock, FrameTimeUs/FrameIndexAt and UtcTime are thread-safe. A HighResTimer or Stopwatch belongs
// to one thread. Lifetime: HighResTimer owns its kernel timer and closes it in the destructor.

#include <ct/core/types.h>

#include <cstdint>
#include <string>

typedef void* HANDLE;   // same definition as <winnt.h>; keeps <windows.h> out of this header

namespace ct {

struct Clock {
    static int64_t QpcNow();                                   // QueryPerformanceCounter
    static int64_t QpcFrequency();                             // cached; never assume 10 MHz
    // floor((qpc - epoch) * 1e6 / freq) with a 128-bit intermediate: exact and overflow-free for any int64 range.
    static TimeUs  QpcToUs(int64_t qpc, int64_t epoch_qpc);
    static int64_t UsToQpc(TimeUs us, int64_t epoch_qpc);      // epoch + floor(us * freq / 1e6)
    static TimeUs  NowUs();                                    // since ProcessStartQpc()
    static int64_t ProcessStartQpc();                          // captured when ct_core's time module initializes
};

// Exact start time of frame `frame_index`: floor(index * 1e6 * den / num). 60 fps gives 0, 16666, 33333, 50000, ...
TimeUs FrameTimeUs(int64_t frame_index, int fps_num, int fps_den);
// Index of the frame shown at t: the largest n with FrameTimeUs(n) <= t, so FrameIndexAt(FrameTimeUs(n)) == n.
int64_t FrameIndexAt(TimeUs t, int fps_num, int fps_den);

// CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION); falls back to a normal waitable timer (Win10 < 1803).
// While initialized it holds timeBeginPeriod(1) (wake accuracy; see time.cpp), released in the destructor.
class HighResTimer {
public:
    HighResTimer() = default;
    ~HighResTimer();
    HighResTimer(const HighResTimer&) = delete;
    HighResTimer& operator=(const HighResTimer&) = delete;

    bool Init();
    HANDLE Handle() const;
    bool IsHighResolution() const { return m_high_res; }
    // Arms the timer for an absolute QPC value (converted to a negative relative due time at arm time).
    bool ArmAbsoluteQpc(int64_t qpc);
    // Waits until QpcNow() >= qpc: kernel wait to SpinUs() (default 200 us) before the target, then spins. Returns
    // false if abort_event was signaled first (or the wait failed).
    bool WaitUntilQpc(int64_t qpc, HANDLE abort_event = nullptr);
    // Longer spins trade CPU for accuracy where the kernel timer fires late (see DECISIONS.md, P01 task 1.5).
    void SetSpinUs(int64_t us) { m_spin_us = us < 0 ? 0 : us; }
    int64_t SpinUs() const { return m_spin_us; }

private:
    HANDLE m_timer = nullptr;
    bool m_high_res = false;
    int64_t m_spin_us = 200;
};

class Stopwatch {
public:
    void Start();
    double ElapsedMs() const;
    TimeUs ElapsedUs() const;

private:
    int64_t m_start_qpc = 0;
};

struct UtcTime {
    static std::string NowIso8601();       // "2026-10-17T12:03:44.123Z" (UTC)
    static std::string NowFileStamp();     // "2026-10-17 12-03-44" (UTC)
    static std::string LocalFileStamp();   // "2026-10-17 12-03-44" (local time)
};

} // namespace ct
