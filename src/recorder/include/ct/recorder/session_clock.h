#pragma once
// ct/recorder/session_clock.h
// The recording's media clock, shared by capture, audio and events so every stream agrees on time: microseconds since
// the session epoch (a QPC value) minus the accumulated pause time. While paused the clock stands still at the pause
// start, so media time has no gaps.
// Thread model: Start/Pause/Resume (and the *At variants) from one controlling thread (the UI thread in the app);
// NowMediaUs/QpcToMediaUs/HnsToMediaUs/IsPaused from any thread, lock-free (a sequence counter guards the
// paused/offset/pause-start triple so a reader never mixes the state before and after a transition).
// Pauses() is the log for meta.json; read it on the controlling thread (or after the session stopped).

#include <ct/core/types.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace ct::recorder {

struct PauseSpan {
    TimeUs media_t_us = 0;    // media time at which the pause began (the clock stood still there)
    TimeUs duration_us = 0;   // wall time spent paused (0 while the pause is still open)
};

struct SessionClock {
    int64_t epoch_qpc = 0;
    std::atomic<int64_t> pause_offset_us{0};     // accumulated wall time of finished pauses
    std::atomic<bool> paused{false};
    std::atomic<int64_t> pause_started_qpc{0};   // valid while paused

    // Sets the epoch and clears the pause state and log. RecordingSession::StartNow calls Start(Clock::QpcNow()).
    void Start(int64_t epoch);

    TimeUs NowMediaUs() const;                   // QpcToMediaUs(Clock::QpcNow())
    // Media time of a QPC value: QpcToUs(qpc, epoch) - pause offset. A qpc inside the current pause maps to the
    // pause start. Values before the epoch give negative times.
    TimeUs QpcToMediaUs(int64_t qpc) const;
    // WASAPI positions: u64QPCPosition is the QPC converted to 100 ns units.
    TimeUs HnsToMediaUs(uint64_t qpc_hns) const;

    void Pause();    // PauseAt(Clock::QpcNow())
    void Resume();   // ResumeAt(Clock::QpcNow())
    // Deterministic variants (tests, callers that already hold the timestamp). Pause while paused and Resume while
    // running are ignored.
    void PauseAt(int64_t qpc);
    void ResumeAt(int64_t qpc);

    bool IsPaused() const { return paused.load(std::memory_order_acquire); }
    TimeUs TotalPausedUs() const { return pause_offset_us.load(std::memory_order_acquire); }
    const std::vector<PauseSpan>& Pauses() const { return m_pauses; }

private:
    TimeUs MediaUs(bool now, int64_t qpc) const;
    void PauseImpl(bool now, int64_t qpc);
    void ResumeImpl(bool now, int64_t qpc);

    std::atomic<uint32_t> m_seq{0};   // odd while a transition is being written
    std::vector<PauseSpan> m_pauses;
};

} // namespace ct::recorder
