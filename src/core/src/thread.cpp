#define CT_LOG_CATEGORY "thread"
#include <ct/core/thread.h>

#include <ct/core/assert.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>

#include <windows.h>
#include <avrt.h>
#include <objbase.h>

namespace ct {

// ---- Thread --------------------------------------------------------------------------------------------------

Thread::~Thread() {
    if (m_handle) {
        Join(INFINITE);
        if (m_handle) CloseHandle(m_handle);
        m_handle = nullptr;
    }
}

bool Thread::Start(const Options& options, std::function<void()> body) {
    if (m_handle) return false;
    m_body = std::move(body);
    m_name = options.name ? options.name : "ct-worker";
    m_mmcss_task = options.mmcss_task ? options.mmcss_task : L"";
    m_priority = options.priority;
    m_com_mta = options.com_mta;
    m_mmcss_applied.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_release);
    DWORD id = 0;
    m_handle = CreateThread(nullptr, 0, &Thread::Entry, this, 0, &id);
    if (!m_handle) {
        m_running.store(false, std::memory_order_release);
        CT_LOG_ERROR("CreateThread(%s) failed: %lu", m_name.c_str(), GetLastError());
        return false;
    }
    m_id = id;
    return true;
}

unsigned long __stdcall Thread::Entry(void* p) {
    Thread* self = static_cast<Thread*>(p);
    SetThreadDescription(GetCurrentThread(), Utf8ToWide(self->m_name).c_str());
    if (self->m_priority != THREAD_PRIORITY_NORMAL && !SetThreadPriority(GetCurrentThread(), self->m_priority))
        CT_LOG_WARN("SetThreadPriority(%s, %d) failed: %lu", self->m_name.c_str(), self->m_priority, GetLastError());

    HANDLE mmcss = nullptr;
    if (!self->m_mmcss_task.empty()) {
        DWORD task_index = 0;
        mmcss = AvSetMmThreadCharacteristicsW(self->m_mmcss_task.c_str(), &task_index);
        if (mmcss)
            self->m_mmcss_applied.store(true, std::memory_order_release);
        else
            CT_LOG_WARN("AvSetMmThreadCharacteristicsW(%ls) failed for %s: %lu", self->m_mmcss_task.c_str(),
                        self->m_name.c_str(), GetLastError());
    }
    const bool com = self->m_com_mta && SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    if (self->m_body) self->m_body();

    if (com) CoUninitialize();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    self->m_running.store(false, std::memory_order_release);
    return 0;
}

bool Thread::Join(uint32_t timeout_ms) {
    if (!m_handle) return true;
    const DWORD rc = WaitForSingleObject(m_handle, timeout_ms);
    if (rc != WAIT_OBJECT_0) return false;
    CloseHandle(m_handle);
    m_handle = nullptr;
    m_id = 0;
    m_body = nullptr;
    return true;
}

bool Thread::IsRunning() const { return m_running.load(std::memory_order_acquire); }
HANDLE Thread::Handle() const { return m_handle; }
uint32_t Thread::Id() const { return m_id; }
bool Thread::MmcssApplied() const { return m_mmcss_applied.load(std::memory_order_acquire); }

// ---- Event ---------------------------------------------------------------------------------------------------

Event::Event(bool manual_reset, bool initial) {
    m_handle = CreateEventW(nullptr, manual_reset ? TRUE : FALSE, initial ? TRUE : FALSE, nullptr);
    if (!m_handle) CT_FATAL("CreateEventW failed");
}

Event::~Event() {
    if (m_handle) CloseHandle(m_handle);
}

void Event::Set() { SetEvent(m_handle); }
void Event::Reset() { ResetEvent(m_handle); }
bool Event::Wait(uint32_t ms) const { return WaitForSingleObject(m_handle, ms) == WAIT_OBJECT_0; }
HANDLE Event::Handle() const { return m_handle; }

// ---- SrwLock -------------------------------------------------------------------------------------------------

static_assert(sizeof(void*) == sizeof(SRWLOCK), "SrwLock stores an SRWLOCK in a void*");

void SrwLock::LockExclusive() { AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock)); }
void SrwLock::UnlockExclusive() { ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock)); }
void SrwLock::LockShared() { AcquireSRWLockShared(reinterpret_cast<PSRWLOCK>(&m_lock)); }
void SrwLock::UnlockShared() { ReleaseSRWLockShared(reinterpret_cast<PSRWLOCK>(&m_lock)); }
bool SrwLock::TryLockExclusive() { return TryAcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m_lock)) != FALSE; }

} // namespace ct
