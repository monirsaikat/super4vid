#pragma once
// ct/core/stats.h
// Named counters, gauges and timings for --stats (dumped once per second) and the F11 overlay.
// Thread model: updates are lock-free atomics (relaxed) and safe from any thread, including capture/audio hot paths.
// C/G/T intern by name under a lock on first use: call them once and keep the reference
// (`static auto& frames = ct::Stats::C("capture.frames");`). Lifetime: entries live until process exit.

#include <atomic>
#include <cstdint>
#include <string>

namespace ct {

struct Stats {
    struct Counter {
        std::atomic<uint64_t> v{0};
        void Add(uint64_t n = 1) { v.fetch_add(n, std::memory_order_relaxed); }
        uint64_t Get() const { return v.load(std::memory_order_relaxed); }
    };
    struct Gauge {
        std::atomic<double> v{0};
        void Set(double x) { v.store(x, std::memory_order_relaxed); }
        double Get() const { return v.load(std::memory_order_relaxed); }
    };
    struct Timing {
        void AddUs(int64_t us);
        double AvgMs() const;
        double MaxMs() const;
        uint64_t Count() const { return m_count.load(std::memory_order_relaxed); }
        void Reset();

    private:
        std::atomic<uint64_t> m_count{0};
        std::atomic<int64_t> m_sum_us{0};
        std::atomic<int64_t> m_max_us{0};
    };

    static Counter& C(const char* name);   // interned by name, created once
    static Gauge& G(const char* name);
    static Timing& T(const char* name);
    // One line, entries sorted by name: "capture.dropped 0  capture.frames 36000  convert.ms avg 0.42 max 1.30"
    static void Dump(std::string& out);
    static void ResetTimings();
    // CT_TRACE_SCOPE adds its duration to Stats::T(name) only while enabled (--stats, F11 overlay). Off by default.
    static void SetScopeTimings(bool enabled);
    static bool ScopeTimings();
};

} // namespace ct
