#include <ct/recorder/session_clock.h>

#include <ct/core/time.h>

namespace ct::recorder {

void SessionClock::Start(int64_t epoch) {
    m_seq.fetch_add(1, std::memory_order_seq_cst);
    epoch_qpc = epoch;
    pause_offset_us.store(0, std::memory_order_release);
    pause_started_qpc.store(0, std::memory_order_release);
    paused.store(false, std::memory_order_release);
    m_seq.fetch_add(1, std::memory_order_seq_cst);
    m_pauses.clear();
}

TimeUs SessionClock::NowMediaUs() const { return MediaUs(true, 0); }

TimeUs SessionClock::QpcToMediaUs(int64_t qpc) const { return MediaUs(false, qpc); }

TimeUs SessionClock::MediaUs(bool now, int64_t qpc) const {
    for (;;) {
        const uint32_t s0 = m_seq.load(std::memory_order_seq_cst);
        if (s0 & 1u) continue;   // a transition is being written (a handful of stores); retry
        // "Now" is read inside the sequence window, after the state it is combined with: a value taken before a
        // concurrent resume would otherwise be combined with the new offset and go backwards.
        if (now) qpc = Clock::QpcNow();
        const bool p = paused.load(std::memory_order_acquire);
        const int64_t offset = pause_offset_us.load(std::memory_order_acquire);
        const int64_t started = pause_started_qpc.load(std::memory_order_acquire);
        const int64_t epoch = epoch_qpc;
        if (m_seq.load(std::memory_order_seq_cst) != s0) continue;
        const int64_t at = (p && qpc > started) ? started : qpc;
        return Clock::QpcToUs(at, epoch) - offset;
    }
}

TimeUs SessionClock::HnsToMediaUs(uint64_t qpc_hns) const {
    // 100 ns -> us (drops < 1 us), then back to QPC ticks so pauses are applied in one place.
    const TimeUs us = static_cast<TimeUs>(qpc_hns / 10);
    return QpcToMediaUs(Clock::UsToQpc(us, 0));
}

void SessionClock::Pause() { PauseImpl(true, 0); }
void SessionClock::Resume() { ResumeImpl(true, 0); }
void SessionClock::PauseAt(int64_t qpc) { PauseImpl(false, qpc); }
void SessionClock::ResumeAt(int64_t qpc) { ResumeImpl(false, qpc); }

// The live variants read QPC after the sequence counter went odd, so no reader can have combined a later "now"
// with the old state (see MediaUs).
void SessionClock::PauseImpl(bool now, int64_t qpc) {
    if (paused.load(std::memory_order_acquire)) return;
    m_seq.fetch_add(1, std::memory_order_seq_cst);
    if (now) qpc = Clock::QpcNow();
    const TimeUs media = Clock::QpcToUs(qpc, epoch_qpc) - pause_offset_us.load(std::memory_order_relaxed);
    pause_started_qpc.store(qpc, std::memory_order_release);
    paused.store(true, std::memory_order_release);
    m_seq.fetch_add(1, std::memory_order_seq_cst);
    m_pauses.push_back({media, 0});
}

void SessionClock::ResumeImpl(bool now, int64_t qpc) {
    if (!paused.load(std::memory_order_acquire)) return;
    m_seq.fetch_add(1, std::memory_order_seq_cst);
    if (now) qpc = Clock::QpcNow();
    const int64_t started = pause_started_qpc.load(std::memory_order_acquire);
    const TimeUs dur = qpc > started ? Clock::QpcToUs(qpc, started) : 0;
    pause_offset_us.store(pause_offset_us.load(std::memory_order_relaxed) + dur, std::memory_order_release);
    paused.store(false, std::memory_order_release);
    m_seq.fetch_add(1, std::memory_order_seq_cst);
    if (!m_pauses.empty()) m_pauses.back().duration_us = dur;
}

} // namespace ct::recorder
