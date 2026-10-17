#pragma once
// ct/core/trace.h
// ETW TraceLogging provider "Camtesia" {9b3e281e-aa6b-4781-a0b4-817998fa4f27} for WPR/WPA, plus scoped spans.
// CT_TRACE_SCOPE("FrameConvert") writes start/stop events (only while a session listens) and, when
// Stats::SetScopeTimings(true), adds the duration to Stats::T("FrameConvert"). Costs two QPC reads otherwise.
// Thread model: everything is thread-safe. Register() once at startup, Unregister() at exit; events written while
// unregistered are dropped. Names must be string literals (or otherwise live for the whole process).
// Capture with any ETW tool that takes a provider GUID (a WPR custom profile, PerfView, tracelog).

#include <cstdint>

namespace ct {

struct Trace {
    static void Register();
    static void Unregister();
    static void Event(const char* name, int64_t value = 0);
    static bool Enabled();   // an ETW session is listening
};

class TraceScope {
public:
    explicit TraceScope(const char* name);
    ~TraceScope();
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

private:
    const char* m_name;
    int64_t m_start_qpc;
};

} // namespace ct

#ifndef CT_CONCAT
#define CT_CONCAT_INNER(a, b) a##b
#define CT_CONCAT(a, b) CT_CONCAT_INNER(a, b)
#endif
#define CT_TRACE_SCOPE(name) ::ct::TraceScope CT_CONCAT(_ts_, __LINE__)(name)   // TraceLoggingWrite start/stop + optional Stats timing
