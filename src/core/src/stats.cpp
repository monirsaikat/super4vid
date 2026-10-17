#include <ct/core/stats.h>

#include <ct/core/strings.h>

#include <windows.h>

#include <algorithm>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ct {

namespace {

enum class Kind : uint8_t { Counter, Gauge, Timing };

struct Entry {
    std::string name;
    Kind kind;
    Stats::Counter counter;
    Stats::Gauge gauge;
    Stats::Timing timing;
};

struct Registry {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<std::unique_ptr<Entry>> entries;                  // heap entries: references stay valid forever
    std::unordered_map<std::string_view, Entry*> by_kind[3];      // keys view Entry::name
};

Registry& R() {
    static Registry registry;
    return registry;
}

std::atomic<bool> g_scope_timings{false};

Entry& Intern(const char* name, Kind kind) {
    Registry& r = R();
    const std::string_view key(name ? name : "");
    auto& map = r.by_kind[static_cast<int>(kind)];
    AcquireSRWLockShared(&r.lock);
    auto it = map.find(key);
    Entry* found = it == map.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&r.lock);
    if (found) return *found;
    AcquireSRWLockExclusive(&r.lock);
    it = map.find(key);
    if (it != map.end()) {
        found = it->second;
    } else {
        auto entry = std::make_unique<Entry>();
        entry->name.assign(key);
        entry->kind = kind;
        found = entry.get();
        r.entries.push_back(std::move(entry));
        map.emplace(std::string_view(found->name), found);
    }
    ReleaseSRWLockExclusive(&r.lock);
    return *found;
}

} // namespace

void Stats::Timing::AddUs(int64_t us) {
    m_count.fetch_add(1, std::memory_order_relaxed);
    m_sum_us.fetch_add(us, std::memory_order_relaxed);
    int64_t prev = m_max_us.load(std::memory_order_relaxed);
    while (us > prev && !m_max_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {
    }
}

double Stats::Timing::AvgMs() const {
    const uint64_t n = m_count.load(std::memory_order_relaxed);
    if (n == 0) return 0.0;
    return static_cast<double>(m_sum_us.load(std::memory_order_relaxed)) / static_cast<double>(n) / 1000.0;
}

double Stats::Timing::MaxMs() const { return static_cast<double>(m_max_us.load(std::memory_order_relaxed)) / 1000.0; }

void Stats::Timing::Reset() {
    m_count.store(0, std::memory_order_relaxed);
    m_sum_us.store(0, std::memory_order_relaxed);
    m_max_us.store(0, std::memory_order_relaxed);
}

Stats::Counter& Stats::C(const char* name) { return Intern(name, Kind::Counter).counter; }
Stats::Gauge& Stats::G(const char* name) { return Intern(name, Kind::Gauge).gauge; }
Stats::Timing& Stats::T(const char* name) { return Intern(name, Kind::Timing).timing; }

void Stats::Dump(std::string& out) {
    Registry& r = R();
    std::vector<const Entry*> sorted;
    AcquireSRWLockShared(&r.lock);
    sorted.reserve(r.entries.size());
    for (const auto& e : r.entries) sorted.push_back(e.get());
    ReleaseSRWLockShared(&r.lock);
    std::sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) {
        return a->name != b->name ? a->name < b->name : a->kind < b->kind;
    });
    for (const Entry* e : sorted) {
        if (!out.empty() && out.back() != '\n') out.append("  ");
        switch (e->kind) {
        case Kind::Counter:
            AppendFormat(out, "%s %llu", e->name.c_str(), static_cast<unsigned long long>(e->counter.Get()));
            break;
        case Kind::Gauge: AppendFormat(out, "%s %.6g", e->name.c_str(), e->gauge.Get()); break;
        case Kind::Timing:
            AppendFormat(out, "%s avg %.2f max %.2f", e->name.c_str(), e->timing.AvgMs(), e->timing.MaxMs());
            break;
        }
    }
}

void Stats::ResetTimings() {
    Registry& r = R();
    AcquireSRWLockShared(&r.lock);
    for (const auto& e : r.entries)
        if (e->kind == Kind::Timing) e->timing.Reset();
    ReleaseSRWLockShared(&r.lock);
}

void Stats::SetScopeTimings(bool enabled) { g_scope_timings.store(enabled, std::memory_order_relaxed); }
bool Stats::ScopeTimings() { return g_scope_timings.load(std::memory_order_relaxed); }

} // namespace ct
