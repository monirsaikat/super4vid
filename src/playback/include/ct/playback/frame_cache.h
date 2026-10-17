#pragma once
// ct/playback/frame_cache.h
// Bookkeeping of one media's decoded frames: entries sorted by pts (binary search), the frame shown at a time, LRU
// ticks and the eviction order (outside the prefetch window first, then least recently used, never a pinned slot).
// It holds no GPU objects: `slot` indexes the texture store owned by MediaCache, so the logic is unit-testable.
// Thread model: not synchronized; MediaCache guards it with its SRW lock.

#include <ct/core/types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ct::playback {

class FrameCache {
public:
    struct Entry {
        TimeUs pts_us = 0;
        TimeUs duration_us = 0;
        size_t bytes = 0;
        uint32_t slot = 0;        // owner's texture slot
        uint64_t last_used = 0;   // LRU tick (insert or Touch)
    };

    static constexpr TimeUs kContiguousUs = 1000;
    // Index of the entry shown at t (pts <= t < pts + duration, or before the next entry when that starts within
    // kContiguousUs of the nominal end) with exact = true; else the nearest earlier entry (exact = false); else -1.
    int Find(TimeUs t, bool& exact) const;
    // Index of the entry with exactly this pts, or -1.
    int FindPts(TimeUs pts) const;
    // Inserts in pts order. An entry with the same pts is replaced (its slot is returned in *replaced_slot).
    void Insert(const Entry& e, bool* replaced = nullptr, uint32_t* replaced_slot = nullptr);
    // The entry to evict, or -1. Order: entries outside [win_start, win_end) first; then entries inside the window
    // that are already behind the playhead in the playback direction (pts + duration <= playhead when forward,
    // pts > playhead when backward); within each group the least recently used goes first. Entries ahead of the
    // playhead inside the window are never evicted (the caller stops prefetching instead), nor are pinned slots.
    int PickVictim(TimeUs win_start, TimeUs win_end, TimeUs playhead, bool forward, const uint32_t* pinned,
                   int pinned_count) const;
    Entry Erase(int index);
    void Touch(int index, uint64_t tick) { m_entries[static_cast<size_t>(index)].last_used = tick; }
    // First time >= from not covered by contiguous entries, i.e. where decoding has to continue to fill [from, ...).
    // Two entries are contiguous when the next starts within `tolerance_us` after the previous one ends.
    TimeUs FirstGap(TimeUs from, TimeUs tolerance_us) const;

    size_t Count() const { return m_entries.size(); }
    size_t Bytes() const { return m_bytes; }
    const Entry& At(int index) const { return m_entries[static_cast<size_t>(index)]; }
    void Clear() { m_entries.clear(); m_bytes = 0; }

private:
    // First index with pts > t.
    size_t UpperBound(TimeUs t) const;

    std::vector<Entry> m_entries;
    size_t m_bytes = 0;
};

} // namespace ct::playback
