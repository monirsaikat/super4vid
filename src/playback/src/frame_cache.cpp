#include <ct/playback/frame_cache.h>
#include <ct/playback/cache_budget.h>

#include <algorithm>
#include <atomic>

namespace ct::playback {

namespace {
std::atomic<size_t> g_budget{CacheBudget::kDefaultBytes};
std::atomic<int64_t> g_used{0};
std::atomic<int64_t> g_extra{0};
} // namespace

void CacheBudget::SetBytes(size_t b) { g_budget.store(b, std::memory_order_relaxed); }
size_t CacheBudget::ConfiguredBytes() { return g_budget.load(std::memory_order_relaxed); }
size_t CacheBudget::Bytes() {
    const int64_t total = static_cast<int64_t>(g_budget.load(std::memory_order_relaxed)) + g_extra.load(std::memory_order_relaxed);
    return total > 0 ? static_cast<size_t>(total) : 0;
}
void CacheBudget::AddExtra(int64_t delta) { g_extra.fetch_add(delta, std::memory_order_relaxed); }
bool CacheBudget::TryReserve(size_t bytes) {
    int64_t used = g_used.load(std::memory_order_relaxed);
    for (;;) {
        if (static_cast<size_t>(std::max<int64_t>(used, 0)) + bytes > Bytes()) return false;
        if (g_used.compare_exchange_weak(used, used + static_cast<int64_t>(bytes), std::memory_order_relaxed)) return true;
    }
}
size_t CacheBudget::Used() {
    const int64_t u = g_used.load(std::memory_order_relaxed);
    return u > 0 ? static_cast<size_t>(u) : 0;
}
void CacheBudget::Add(int64_t delta) { g_used.fetch_add(delta, std::memory_order_relaxed); }

size_t FrameCache::UpperBound(TimeUs t) const {
    return static_cast<size_t>(std::upper_bound(m_entries.begin(), m_entries.end(), t,
                                                [](TimeUs v, const Entry& e) { return v < e.pts_us; }) -
                               m_entries.begin());
}

int FrameCache::Find(TimeUs t, bool& exact) const {
    exact = false;
    const size_t ub = UpperBound(t);
    if (ub == 0) return -1;
    const Entry& e = m_entries[ub - 1];
    const TimeUs end = e.pts_us + e.duration_us;
    // Rounded sample durations leave 1 us holes between frames (16666 + 16666 < 33333): a frame lasts until the next
    // cached one when that starts within kContiguousUs of its nominal end.
    exact = t < end || (ub < m_entries.size() && m_entries[ub].pts_us - end <= kContiguousUs);
    return static_cast<int>(ub - 1);
}

int FrameCache::FindPts(TimeUs pts) const {
    const size_t ub = UpperBound(pts);
    if (ub == 0 || m_entries[ub - 1].pts_us != pts) return -1;
    return static_cast<int>(ub - 1);
}

void FrameCache::Insert(const Entry& e, bool* replaced, uint32_t* replaced_slot) {
    if (replaced) *replaced = false;
    const int same = FindPts(e.pts_us);
    if (same >= 0) {
        Entry& old = m_entries[static_cast<size_t>(same)];
        if (replaced) *replaced = true;
        if (replaced_slot) *replaced_slot = old.slot;
        m_bytes -= old.bytes;
        old = e;
        m_bytes += e.bytes;
        return;
    }
    m_entries.insert(m_entries.begin() + static_cast<ptrdiff_t>(UpperBound(e.pts_us)), e);
    m_bytes += e.bytes;
}

int FrameCache::PickVictim(TimeUs win_start, TimeUs win_end, TimeUs playhead, bool forward, const uint32_t* pinned,
                           int pinned_count) const {
    int best_out = -1, best_behind = -1;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        bool is_pinned = false;
        for (int p = 0; p < pinned_count; ++p) is_pinned |= pinned[p] == e.slot;
        if (is_pinned) continue;
        const bool inside = e.pts_us + e.duration_us > win_start && e.pts_us < win_end;
        const bool behind = forward ? e.pts_us + e.duration_us <= playhead : e.pts_us > playhead;
        if (inside && !behind) continue;
        int& best = inside ? best_behind : best_out;
        if (best < 0 || e.last_used < m_entries[static_cast<size_t>(best)].last_used) best = static_cast<int>(i);
    }
    return best_out >= 0 ? best_out : best_behind;
}

FrameCache::Entry FrameCache::Erase(int index) {
    const Entry e = m_entries[static_cast<size_t>(index)];
    m_entries.erase(m_entries.begin() + index);
    m_bytes -= e.bytes;
    return e;
}

TimeUs FrameCache::FirstGap(TimeUs from, TimeUs tolerance_us) const {
    bool exact = false;
    int i = Find(from, exact);
    if (i < 0 || !exact) return from;
    TimeUs end = m_entries[static_cast<size_t>(i)].pts_us + m_entries[static_cast<size_t>(i)].duration_us;
    for (size_t k = static_cast<size_t>(i) + 1; k < m_entries.size(); ++k) {
        if (m_entries[k].pts_us > end + tolerance_us) break;
        end = std::max(end, m_entries[k].pts_us + m_entries[k].duration_us);
    }
    return end;
}

} // namespace ct::playback
