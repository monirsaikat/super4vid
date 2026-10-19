#include <ct/core/trace.h>

#include <ct/core/stats.h>
#include <ct/core/time.h>

#include <windows.h>
#include <TraceLoggingProvider.h>
#include <winmeta.h>

#include <atomic>
#include <cstdint>

// Generated once for this project; never change it (saved WPR profiles and scripts refer to it).
TRACELOGGING_DEFINE_PROVIDER(g_ct_provider, "super4vid",
                             (0x9b3e281e, 0xaa6b, 0x4781, 0xa0, 0xb4, 0x81, 0x79, 0x98, 0xfa, 0x4f, 0x27));

namespace ct {

namespace {

std::atomic<bool> g_registered{false};

// Per-thread direct-mapped cache name pointer -> Timing, so a scope does not hit the Stats registry lock.
struct TimingCacheSlot {
    const char* name;
    Stats::Timing* timing;
};
thread_local TimingCacheSlot t_timing_cache[64];

Stats::Timing& CachedTiming(const char* name) {
    const uintptr_t h = reinterpret_cast<uintptr_t>(name);
    TimingCacheSlot& slot = t_timing_cache[(h >> 3) & 63];
    if (slot.name != name) {
        slot.timing = &Stats::T(name);
        slot.name = name;
    }
    return *slot.timing;
}

} // namespace

void Trace::Register() {
    if (g_registered.exchange(true)) return;
    TraceLoggingRegister(g_ct_provider);
}

void Trace::Unregister() {
    if (!g_registered.exchange(false)) return;
    TraceLoggingUnregister(g_ct_provider);
}

bool Trace::Enabled() { return TraceLoggingProviderEnabled(g_ct_provider, 0, 0); }

void Trace::Event(const char* name, int64_t value) {
    if (!TraceLoggingProviderEnabled(g_ct_provider, 0, 0)) return;
    TraceLoggingWrite(g_ct_provider, "Event", TraceLoggingUtf8String(name ? name : "", "Name"),
                      TraceLoggingInt64(value, "Value"));
}

TraceScope::TraceScope(const char* name) : m_name(name), m_start_qpc(Clock::QpcNow()) {
    if (TraceLoggingProviderEnabled(g_ct_provider, 0, 0))
        TraceLoggingWrite(g_ct_provider, "Scope", TraceLoggingOpcode(WINEVENT_OPCODE_START),
                          TraceLoggingUtf8String(m_name, "Name"));
}

TraceScope::~TraceScope() {
    const int64_t end = Clock::QpcNow();
    const bool etw = TraceLoggingProviderEnabled(g_ct_provider, 0, 0);
    const bool stats = Stats::ScopeTimings();
    if (!etw && !stats) return;
    const int64_t us = Clock::QpcToUs(end, m_start_qpc);
    if (etw)
        TraceLoggingWrite(g_ct_provider, "Scope", TraceLoggingOpcode(WINEVENT_OPCODE_STOP),
                          TraceLoggingUtf8String(m_name, "Name"), TraceLoggingInt64(us, "DurationUs"));
    if (stats) CachedTiming(m_name).AddUs(us);
}

} // namespace ct
