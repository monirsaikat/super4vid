#pragma once
// ct/core/sync.h
// Thin wrappers over Win32 events and slim reader/writer locks, plus RAII guards.
// Thread model: all members are thread-safe (that is their purpose). Lifetime: Event owns its kernel handle.
// SrwLock is not recursive; never hold one on the capture/encode/audio hot path (docs/02 section 6).

#include <cstdint>

typedef void* HANDLE;   // same definition as <winnt.h>; keeps <windows.h> out of this header
#ifndef INFINITE
#define INFINITE 0xFFFFFFFF   // same token sequence as <winbase.h>
#endif

namespace ct {

class Event {
public:
    explicit Event(bool manual_reset = false, bool initial = false);
    ~Event();
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    void Set();
    void Reset();
    bool Wait(uint32_t ms = INFINITE) const;   // true when signaled, false on timeout
    HANDLE Handle() const;

private:
    HANDLE m_handle = nullptr;
};

class SrwLock {
public:
    SrwLock() = default;
    SrwLock(const SrwLock&) = delete;
    SrwLock& operator=(const SrwLock&) = delete;

    void LockExclusive();
    void UnlockExclusive();
    void LockShared();
    void UnlockShared();
    bool TryLockExclusive();

private:
    void* m_lock = nullptr;   // SRWLOCK storage (SRWLOCK_INIT is all zero)
};

class ExclusiveGuard {
public:
    explicit ExclusiveGuard(SrwLock& lock) : m_lock(lock) { m_lock.LockExclusive(); }
    ~ExclusiveGuard() { m_lock.UnlockExclusive(); }
    ExclusiveGuard(const ExclusiveGuard&) = delete;
    ExclusiveGuard& operator=(const ExclusiveGuard&) = delete;

private:
    SrwLock& m_lock;
};

class SharedGuard {
public:
    explicit SharedGuard(SrwLock& lock) : m_lock(lock) { m_lock.LockShared(); }
    ~SharedGuard() { m_lock.UnlockShared(); }
    SharedGuard(const SharedGuard&) = delete;
    SharedGuard& operator=(const SharedGuard&) = delete;

private:
    SrwLock& m_lock;
};

} // namespace ct
