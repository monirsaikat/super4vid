#define CT_LOG_CATEGORY "hooks"
#include <ct/platform/hooks_thread.h>

#include <ct/core/log.h>
#include <ct/core/time.h>

namespace ct::platform {

namespace {

// The LL hook and WinEvent procedures carry no context pointer; they run on the thread that installed them, so a
// thread_local finds the owning HooksThread (and several instances can coexist, e.g. in tests).
thread_local HooksThread* t_self = nullptr;

constexpr UINT kMsgWake = WM_APP + 0x51;   // posted by CheckWatchdog / DebugStall to wake the loop
constexpr int64_t kHeartbeatMs = 250;
constexpr int64_t kStallMs = 1000;

int64_t MsToQpc(int64_t ms) { return ms * Clock::QpcFrequency() / 1000; }

} // namespace

HooksThread::~HooksThread() { Stop(); }

bool HooksThread::Start(const Config& config, HooksHandler* handler) {
    if (m_thread.IsRunning() || !handler) return false;
    m_config = config;
    m_handler = handler;
    m_stop.Reset();
    m_started.Reset();
    m_start_ok.store(false);
    m_reinstall.store(false);
    m_stall_ms.store(0);
    m_heartbeat_qpc.store(Clock::QpcNow(), std::memory_order_relaxed);
    Thread::Options opt;
    opt.name = "ct-hooks";
    opt.priority = THREAD_PRIORITY_HIGHEST;
    if (!m_thread.Start(opt, [this] { Run(); })) return false;
    m_started.Wait(5000);
    if (!m_start_ok.load()) {
        Stop();
        return false;
    }
    return true;
}

void HooksThread::Stop() {
    if (!m_thread.IsRunning() && !m_thread.Handle()) return;
    m_stop.Set();
    m_thread.Join(5000);
}

bool HooksThread::CheckWatchdog() {
    if (!m_thread.IsRunning()) return false;
    const int64_t now = Clock::QpcNow();
    const int64_t last = m_heartbeat_qpc.load(std::memory_order_acquire);
    if (now - last < MsToQpc(kStallMs)) return false;
    if (m_reinstall.exchange(true)) return false;   // already requested; the loop has not pumped since
    // Push the heartbeat forward so one stall is reported once, not every 100 ms while it lasts.
    m_heartbeat_qpc.store(now, std::memory_order_release);
    PostThreadMessageW(m_thread.Id(), kMsgWake, 0, 0);
    return true;
}

void HooksThread::DebugStall(uint32_t ms) {
    m_stall_ms.store(ms);
    PostThreadMessageW(m_thread.Id(), kMsgWake, 0, 0);
}

bool HooksThread::Install() {
    const HINSTANCE module = GetModuleHandleW(nullptr);
    if (m_config.mouse) {
        m_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, module, 0);
        if (!m_mouse_hook) {
            CT_LOG_ERROR("SetWindowsHookExW(WH_MOUSE_LL) failed: %lu", GetLastError());
            return false;
        }
    }
    if (m_config.keyboard) {
        m_kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, module, 0);
        if (!m_kbd_hook) {
            CT_LOG_ERROR("SetWindowsHookExW(WH_KEYBOARD_LL) failed: %lu", GetLastError());
            return false;
        }
    }
    if (m_config.focus && !m_focus_hook) {
        m_focus_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FocusProc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!m_focus_hook) CT_LOG_WARN("SetWinEventHook(EVENT_SYSTEM_FOREGROUND) failed; no focus events");
    }
    return true;
}

void HooksThread::Uninstall() {
    if (m_mouse_hook) UnhookWindowsHookEx(m_mouse_hook);
    if (m_kbd_hook) UnhookWindowsHookEx(m_kbd_hook);
    if (m_focus_hook) UnhookWinEvent(m_focus_hook);
    m_mouse_hook = nullptr;
    m_kbd_hook = nullptr;
    m_focus_hook = nullptr;
}

void HooksThread::Run() {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);   // creates the message queue before anyone posts
    t_self = this;
    const bool ok = Install();
    m_start_ok.store(ok);
    m_started.Set();
    if (!ok) {
        Uninstall();
        t_self = nullptr;
        return;
    }

    HighResTimer timer;
    const bool use_timer = m_config.timer_hz > 0 && timer.Init();
    const int64_t period = use_timer ? Clock::QpcFrequency() / m_config.timer_hz : 0;
    int64_t next_tick = Clock::QpcNow() + period;
    if (use_timer) timer.ArmAbsoluteQpc(next_tick);
    int64_t next_heartbeat = Clock::QpcNow();
    const int64_t heartbeat_period = MsToQpc(kHeartbeatMs);

    HANDLE handles[2] = {m_stop.Handle(), use_timer ? timer.Handle() : nullptr};
    const DWORD count = use_timer ? 2 : 1;
    for (;;) {
        int64_t now = Clock::QpcNow();
        DWORD timeout_ms = 0;
        if (next_heartbeat > now) timeout_ms = static_cast<DWORD>((next_heartbeat - now) * 1000 / Clock::QpcFrequency()) + 1;
        const DWORD r = MsgWaitForMultipleObjectsEx(count, handles, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) break;

        // Pumping is what delivers the LL hook callbacks and WinEvents (they arrive inside PeekMessage).
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.hwnd == nullptr && msg.message == WM_NULL) {
                m_heartbeat_qpc.store(Clock::QpcNow(), std::memory_order_release);
                continue;
            }
            if (msg.hwnd == nullptr && msg.message == kMsgWake) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (const uint32_t stall = m_stall_ms.exchange(0)) Sleep(stall);   // test only

        if (m_reinstall.exchange(false)) {
            // A stalled loop may have lost its LL hooks without notice; installing fresh ones is always safe.
            Uninstall();
            if (Install()) {
                m_reinstalls.fetch_add(1, std::memory_order_relaxed);
                CT_LOG_WARN("hooks re-installed after a stalled message loop");
            } else {
                CT_LOG_ERROR("hooks could not be re-installed");
            }
        }

        now = Clock::QpcNow();
        if (use_timer && (r == WAIT_OBJECT_0 + 1 || now >= next_tick)) {
            if (now >= next_tick) {
                m_handler->OnTimer();
                m_ticks.fetch_add(1, std::memory_order_relaxed);
                next_tick += period;
                if (next_tick <= now) next_tick = now + period;   // fell behind (stall): resynchronize, no burst
            }
            timer.ArmAbsoluteQpc(next_tick);
        }
        if (now >= next_heartbeat) {
            PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            next_heartbeat = now + heartbeat_period;
        }
    }
    Uninstall();
    t_self = nullptr;
}

LRESULT CALLBACK HooksThread::MouseProc(int code, WPARAM wparam, LPARAM lparam) {
    HooksThread* self = t_self;
    if (code == HC_ACTION && self) {
        const auto& info = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
        self->m_hook_calls.fetch_add(1, std::memory_order_relaxed);
        const bool tagged = (info.flags & LLMHF_INJECTED) && info.dwExtraInfo == self->m_config.swallow_tag;
        // With a test tag set, only the test's own input is reported: a person using the machine is not recorded.
        if (!self->m_config.swallow_tag || tagged) self->m_handler->OnMouseHook(static_cast<UINT>(wparam), info);
        if (self->m_config.swallow_tag && tagged) return 1;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK HooksThread::KeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    HooksThread* self = t_self;
    if (code == HC_ACTION && self) {
        const auto& info = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        self->m_hook_calls.fetch_add(1, std::memory_order_relaxed);
        const bool tagged = (info.flags & LLKHF_INJECTED) && info.dwExtraInfo == self->m_config.swallow_tag;
        if (!self->m_config.swallow_tag || tagged) self->m_handler->OnKeyboardHook(static_cast<UINT>(wparam), info);
        if (self->m_config.swallow_tag && tagged) return 1;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void CALLBACK HooksThread::FocusProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG, DWORD, DWORD) {
    HooksThread* self = t_self;
    if (!self || event != EVENT_SYSTEM_FOREGROUND || id_object != OBJID_WINDOW || !hwnd) return;
    self->m_handler->OnForeground(hwnd);
}

} // namespace ct::platform
