#include <ct/core/time.h>

#include "int128_internal.h"

#include <windows.h>
#include <timeapi.h>

#include <cstdio>

namespace ct {

namespace {

int64_t ReadQpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

// Reading QPC has no side effects, so this one dynamic initializer is acceptable (docs/02 section 1).
const int64_t g_process_start_qpc = ReadQpc();

constexpr int64_t kSpinUs = 200;

std::string Stamp(const SYSTEMTIME& st) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u-%02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return std::string(buf);
}

} // namespace

int64_t Clock::QpcNow() { return ReadQpc(); }

int64_t Clock::QpcFrequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    return freq;
}

TimeUs Clock::QpcToUs(int64_t qpc, int64_t epoch_qpc) {
    return detail::MulDivFloor(qpc - epoch_qpc, 1'000'000, QpcFrequency());
}

int64_t Clock::UsToQpc(TimeUs us, int64_t epoch_qpc) {
    return epoch_qpc + detail::MulDivFloor(us, QpcFrequency(), 1'000'000);
}

TimeUs Clock::NowUs() { return QpcToUs(ReadQpc(), g_process_start_qpc); }

int64_t Clock::ProcessStartQpc() { return g_process_start_qpc; }

TimeUs FrameTimeUs(int64_t frame_index, int fps_num, int fps_den) {
    return detail::FrameTimeUsImpl(frame_index, fps_num, fps_den);
}

int64_t FrameIndexAt(TimeUs t, int fps_num, int fps_den) { return detail::FrameIndexAtImpl(t, fps_num, fps_den); }

HighResTimer::~HighResTimer() {
    if (m_timer) {
        CloseHandle(m_timer);
        timeEndPeriod(1);
    }
}

bool HighResTimer::Init() {
    if (m_timer) return true;
    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    m_high_res = m_timer != nullptr;
    if (!m_timer) m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (!m_timer) return false;
    // Measured on Win11 26100: even high-resolution timers wake up to ~0.8 ms late at the default 15.6 ms system
    // timer resolution (p99 ~500 us); with a 1 ms period p99 drops to ~300 us. Held only while a timer exists, i.e.
    // while recording or playing, so the idle app keeps the default resolution.
    timeBeginPeriod(1);
    return true;
}

HANDLE HighResTimer::Handle() const { return m_timer; }

bool HighResTimer::ArmAbsoluteQpc(int64_t qpc) {
    if (!m_timer) return false;
    // FILETIME-based absolute due times are not on the QPC timebase, so convert to a relative (negative) due time.
    const int64_t delta_qpc = qpc - ReadQpc();
    int64_t due_100ns = detail::MulDivFloor(delta_qpc, 10'000'000, Clock::QpcFrequency());
    if (due_100ns < 1) due_100ns = 1;
    LARGE_INTEGER due;
    due.QuadPart = -due_100ns;
    return SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE;
}

bool HighResTimer::WaitUntilQpc(int64_t qpc, HANDLE abort_event) {
    const int64_t spin_qpc = detail::MulDivFloor(kSpinUs, Clock::QpcFrequency(), 1'000'000);
    if (m_timer && qpc - ReadQpc() > spin_qpc) {
        if (!ArmAbsoluteQpc(qpc - spin_qpc)) return false;
        HANDLE handles[2] = {m_timer, abort_event};
        const DWORD count = abort_event ? 2 : 1;
        const DWORD rc = WaitForMultipleObjects(count, handles, FALSE, INFINITE);
        if (rc == WAIT_OBJECT_0 + 1) {
            CancelWaitableTimer(m_timer);
            return false;
        }
        if (rc != WAIT_OBJECT_0) return false;
    }
    while (ReadQpc() < qpc) YieldProcessor();
    return !(abort_event && WaitForSingleObject(abort_event, 0) == WAIT_OBJECT_0);
}

void Stopwatch::Start() { m_start_qpc = ReadQpc(); }

double Stopwatch::ElapsedMs() const {
    return static_cast<double>(ReadQpc() - m_start_qpc) * 1000.0 / static_cast<double>(Clock::QpcFrequency());
}

TimeUs Stopwatch::ElapsedUs() const { return Clock::QpcToUs(ReadQpc(), m_start_qpc); }

std::string UtcTime::NowIso8601() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

std::string UtcTime::NowFileStamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    return Stamp(st);
}

std::string UtcTime::LocalFileStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Stamp(st);
}

} // namespace ct
