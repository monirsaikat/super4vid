#pragma once
// ct/core/thread.h
// The only way first-party code creates threads. Each thread gets a debugger-visible name, a priority, an optional
// MMCSS task ("Capture", "Pro Audio", "Playback") and, by default, an MTA COM apartment, all set up inside the thread.
// Thread model: Start/Join/the destructor from the owning thread; IsRunning/Handle/Id from any thread.
// Lifetime: the destructor joins a thread that is still running (waits without timeout), then closes the handle.
// Not copyable or movable; hold it by value or in a unique_ptr.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

typedef void* HANDLE;   // same definition as <winnt.h>; keeps <windows.h> out of this header
#ifndef INFINITE
#define INFINITE 0xFFFFFFFF   // same token sequence as <winbase.h>, so a later <windows.h> does not warn
#endif

namespace ct {

class Thread {
public:
    struct Options {
        const char* name = "ct-worker";        // copied by Start()
        int priority = 0;                      // THREAD_PRIORITY_NORMAL; e.g. THREAD_PRIORITY_TIME_CRITICAL = 15
        const wchar_t* mmcss_task = nullptr;   // L"Capture", L"Pro Audio", L"Playback"; copied by Start()
        bool com_mta = true;                   // CoInitializeEx(COINIT_MULTITHREADED) around the body
    };

    Thread() = default;
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Sets SetThreadDescription, priority, AvSetMmThreadCharacteristicsW and COM inside the new thread, then runs body.
    // Returns false if already started (and not joined) or CreateThread failed.
    bool Start(const Options& options, std::function<void()> body);
    bool Join(uint32_t timeout_ms = INFINITE);   // false on timeout (the caller CT_FATALs); true if never started
    bool IsRunning() const;
    HANDLE Handle() const;
    uint32_t Id() const;
    // True once the thread registered with MMCSS (AvSetMmThreadCharacteristicsW returned a handle). MMCSS can be
    // unavailable (service stopped, some CI VMs); the thread then runs at its plain priority and logs a warning.
    bool MmcssApplied() const;

private:
    static unsigned long __stdcall Entry(void* self);

    std::function<void()> m_body;
    std::string m_name;
    std::wstring m_mmcss_task;
    int m_priority = 0;
    bool m_com_mta = true;
    HANDLE m_handle = nullptr;
    uint32_t m_id = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_mmcss_applied{false};
};

} // namespace ct
