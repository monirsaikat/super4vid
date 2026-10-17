#pragma once
// ct/platform/hooks_thread.h
// The "ct-hooks" thread: low-level mouse/keyboard hooks, a foreground-window WinEvent hook and a periodic
// high-resolution timer, all serviced by one message loop (LL hooks and out-of-context WinEvents are delivered as
// messages to the installing thread, so it must pump). A heartbeat lets another thread detect a stalled loop and
// have the hooks re-installed: Windows silently removes an LL hook whose callback exceeded LowLevelHooksTimeout.
// Thread model: Start/Stop/DebugStall from one controlling thread; CheckWatchdog from one other thread (the
// recorder's IO thread); counters from any thread. Every HooksHandler callback runs on ct-hooks.
// Lifetime: Stop() (or the destructor) unhooks everything and joins the thread. The handler must outlive Stop().

#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/platform/win.h>

#include <atomic>
#include <cstdint>

namespace ct::platform {

class HooksHandler {
public:
    // Inside the WH_MOUSE_LL / WH_KEYBOARD_LL procedure (nCode == HC_ACTION): enqueue and return. No allocation,
    // no logging, no locks, no GDI: the system removes hooks that take longer than ~300 ms.
    virtual void OnMouseHook(UINT msg, const MSLLHOOKSTRUCT& info) = 0;
    virtual void OnKeyboardHook(UINT msg, const KBDLLHOOKSTRUCT& info) = 0;
    // EVENT_SYSTEM_FOREGROUND (out of context, own process skipped). Same rules as the hook callbacks.
    virtual void OnForeground(HWND hwnd) = 0;
    // Periodic timer (Config::timer_hz), e.g. the 120 Hz cursor poll.
    virtual void OnTimer() = 0;

protected:
    ~HooksHandler() = default;
};

class HooksThread {
public:
    struct Config {
        bool mouse = true;
        bool keyboard = true;          // false: no keyboard hook is installed at all
        bool focus = true;
        int timer_hz = 120;            // 0 = no timer
        // Test hook: input injected with SendInput whose dwExtraInfo equals this (non-zero) value is reported to the
        // handler and then swallowed (the hook returns 1), so tests can click and type without touching other apps.
        ULONG_PTR swallow_tag = 0;
    };

    HooksThread() = default;
    ~HooksThread();
    HooksThread(const HooksThread&) = delete;
    HooksThread& operator=(const HooksThread&) = delete;

    // Starts ct-hooks (THREAD_PRIORITY_HIGHEST) and waits until the hooks are installed. False if a hook could not
    // be installed (the thread is stopped again).
    bool Start(const Config& config, HooksHandler* handler);
    void Stop();
    bool IsRunning() const { return m_thread.IsRunning(); }

    // Call every ~100 ms from another thread. When the loop has not processed its heartbeat (a WM_NULL it posts to
    // itself every 250 ms) for 1 s, asks it to unhook and re-install as soon as it pumps again. Returns true when a
    // stall was detected by this call.
    bool CheckWatchdog();

    uint32_t Reinstalls() const { return m_reinstalls.load(std::memory_order_relaxed); }
    uint64_t TimerTicks() const { return m_ticks.load(std::memory_order_relaxed); }
    uint64_t HookCalls() const { return m_hook_calls.load(std::memory_order_relaxed); }
    // Blocks the hooks thread for `ms` (tests of the watchdog). Returns immediately.
    void DebugStall(uint32_t ms);

private:
    static LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wparam, LPARAM lparam);
    static void CALLBACK FocusProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD, DWORD);

    void Run();
    bool Install();
    void Uninstall();

    Config m_config;
    HooksHandler* m_handler = nullptr;
    Thread m_thread;
    Event m_stop{true, false};
    Event m_started{true, false};
    std::atomic<bool> m_start_ok{false};
    HHOOK m_mouse_hook = nullptr;
    HHOOK m_kbd_hook = nullptr;
    HWINEVENTHOOK m_focus_hook = nullptr;
    std::atomic<int64_t> m_heartbeat_qpc{0};
    std::atomic<bool> m_reinstall{false};
    std::atomic<uint32_t> m_stall_ms{0};
    std::atomic<uint32_t> m_reinstalls{0};
    std::atomic<uint64_t> m_ticks{0};
    std::atomic<uint64_t> m_hook_calls{0};
};

} // namespace ct::platform
