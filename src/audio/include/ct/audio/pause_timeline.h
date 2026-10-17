#pragma once
// ct/audio/pause_timeline.h
// Pause intervals of a capture stream, decided by each frame's own capture time (absolute microseconds on the QPC
// clock), not by when the capture thread happens to read the packet. Frames captured inside [begin, end) of a pause
// are dropped; every other frame gets media_abs = capture time - (sum of pauses that ended before it), so the stream
// is continuous across pauses and a packet read just after a Pause()/Resume() call keeps exactly the right frames.
// Thread model: Pause/Resume/Reset/IsPaused on the owner thread; Map/PeekMedia on one consumer (capture) thread.
// Pause() publishes the interval start with release semantics, Resume() publishes its end; a consumer that has not
// seen the end yet treats the pause as still open (drops). The intervals are a ring of kCapacity entries: the owner
// must not get more than kCapacity pauses ahead of the consumer (it runs every few ms).

#include <atomic>
#include <cstdint>
#include <limits>

namespace ct::audio {

class PauseTimeline {
public:
    static constexpr int kCapacity = 64;
    static constexpr int64_t kOpen = (std::numeric_limits<int64_t>::max)();

    // Owner thread, while the consumer is not running.
    void Reset() {
        m_count.store(0, std::memory_order_relaxed);
        m_owner_paused = false;
        m_k = 0;
        m_offset_us = 0;
    }
    // Owner thread. Calls that do not change the state are ignored. Times must not go backwards.
    void Pause(int64_t abs_us) {
        if (m_owner_paused) return;
        const uint32_t n = m_count.load(std::memory_order_relaxed);
        Slot& s = m_slots[n % kCapacity];
        s.end.store(kOpen, std::memory_order_relaxed);
        s.begin.store(abs_us, std::memory_order_relaxed);
        m_count.store(n + 1, std::memory_order_release);
        m_owner_paused = true;
    }
    void Resume(int64_t abs_us) {
        if (!m_owner_paused) return;
        const uint32_t n = m_count.load(std::memory_order_relaxed);
        Slot& s = m_slots[(n - 1) % kCapacity];
        const int64_t b = s.begin.load(std::memory_order_relaxed);
        s.end.store(abs_us > b ? abs_us : b, std::memory_order_release);
        m_owner_paused = false;
    }
    bool IsPaused() const { return m_owner_paused; }   // owner thread

    // Consumer: frames [0, frames) captured at r0_abs_us + floor(i * 1e6 / rate) (r0 must not go backwards between
    // calls). emit(uint32_t first_frame, uint32_t count, int64_t media_abs_us_of_first) for each run that is kept.
    // Returns the number of frames dropped because they fall inside a pause.
    template <class Emit>
    uint32_t Map(int64_t r0_abs_us, uint32_t frames, int rate, Emit emit) {
        uint32_t dropped = 0;
        uint32_t i = 0;
        while (i < frames) {
            const int64_t r = r0_abs_us + static_cast<int64_t>(i) * 1'000'000 / rate;
            Advance(r);
            const uint32_t count = m_count.load(std::memory_order_acquire);
            if (m_k < count) {
                const Slot& s = m_slots[m_k % kCapacity];
                const int64_t b = s.begin.load(std::memory_order_relaxed);
                const int64_t e = s.end.load(std::memory_order_acquire);
                if (r >= b) {   // inside the pause: skip to its end
                    const uint32_t j = e == kOpen ? frames : FirstFrameAtOrAfter(r0_abs_us, e, rate, frames);
                    const uint32_t stop = j > i ? j : i + 1;
                    dropped += stop - i;
                    i = stop;
                    continue;
                }
                // Kept until the next pause begins.
                uint32_t j = FirstFrameAtOrAfter(r0_abs_us, b, rate, frames);
                if (j <= i) j = i + 1;
                emit(i, j - i, r - m_offset_us);
                i = j;
                continue;
            }
            emit(i, frames - i, r - m_offset_us);
            break;
        }
        return dropped;
    }

    // Consumer: media time of capture time x without consuming intervals (for starvation fill). Returns false when x
    // lies inside a pause (or a pause is open).
    bool PeekMedia(int64_t x_abs_us, int64_t* media_abs_us) const {
        int64_t offset = m_offset_us;
        const uint32_t count = m_count.load(std::memory_order_acquire);
        for (uint32_t k = m_k; k < count; ++k) {
            const Slot& s = m_slots[k % kCapacity];
            const int64_t b = s.begin.load(std::memory_order_relaxed);
            const int64_t e = s.end.load(std::memory_order_acquire);
            if (x_abs_us < b) break;
            if (e == kOpen || x_abs_us < e) return false;
            offset += e - b;
        }
        *media_abs_us = x_abs_us - offset;
        return true;
    }

private:
    struct Slot {
        std::atomic<int64_t> begin{0};
        std::atomic<int64_t> end{kOpen};
    };

    // Consumes the pauses that ended at or before r.
    void Advance(int64_t r) {
        const uint32_t count = m_count.load(std::memory_order_acquire);
        while (m_k < count) {
            const Slot& s = m_slots[m_k % kCapacity];
            const int64_t e = s.end.load(std::memory_order_acquire);
            if (e == kOpen || e > r) break;
            m_offset_us += e - s.begin.load(std::memory_order_relaxed);
            ++m_k;
        }
    }
    // Smallest j with r0 + floor(j * 1e6 / rate) >= t, clamped to `frames`.
    static uint32_t FirstFrameAtOrAfter(int64_t r0, int64_t t, int rate, uint32_t frames) {
        if (t <= r0) return 0;
        int64_t j = ((t - r0) * rate + 999'999) / 1'000'000;
        while (j > 0 && r0 + (j - 1) * 1'000'000 / rate >= t) --j;
        while (r0 + j * 1'000'000 / rate < t) ++j;
        return j > static_cast<int64_t>(frames) ? frames : static_cast<uint32_t>(j);
    }

    Slot m_slots[kCapacity];
    std::atomic<uint32_t> m_count{0};
    bool m_owner_paused = false;   // owner thread
    uint32_t m_k = 0;              // consumer: first interval not yet fully behind it
    int64_t m_offset_us = 0;       // consumer: total length of the consumed pauses
};

} // namespace ct::audio
