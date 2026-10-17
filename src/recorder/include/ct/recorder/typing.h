#pragma once
// ct/recorder/typing.h
// Derives TYPING records at record time: non-modifier key-downs closer than the gap (1 s) form one burst. A burst
// starts at its first key-down and ends at its last key-down (the same rule EventLog uses for files without TYPING
// records, so both paths agree). The "ended" edge is known only once the gap has passed, so it is emitted late
// with the time of the last key; readers sort records by time.
// Thread model: one thread (ct-events-io). Pure logic, no allocation.

#include <ct/core/types.h>

namespace ct::recorder {

class TypingDetector {
public:
    static constexpr TimeUs kDefaultGapUs = 1'000'000;
    struct Edge { TimeUs t_us = 0; bool started = false; };

    explicit TypingDetector(TimeUs gap_us = kDefaultGapUs) : m_gap(gap_us) {}

    // A non-modifier key-down at t. Emits "ended" for a burst whose last key is >= gap before t, then "started" for
    // a new burst. Returns the number of edges written to out (0..2), in time order.
    int OnKeyDown(TimeUs t_us, Edge out[2]);
    // Periodic check: closes the open burst when now - last key >= gap. Returns 0 or 1.
    int Tick(TimeUs now_us, Edge out[1]);
    // Closes the open burst unconditionally (pause, stop). Returns 0 or 1.
    int Finish(Edge out[1]);

    bool InBurst() const { return m_open; }
    TimeUs LastKeyUs() const { return m_last; }

private:
    TimeUs m_gap;
    TimeUs m_last = 0;
    bool m_open = false;
};

} // namespace ct::recorder
