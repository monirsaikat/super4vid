#define CT_LOG_CATEGORY "platform"
#include <ct/platform/single_instance.h>

#include <ct/core/log.h>
#include <ct/platform/win.h>

#include <cwchar>

namespace ct::platform {

namespace {
HANDLE g_mutex = nullptr;
constexpr const wchar_t* kMainWindowClass = L"CamtesiaMain";
}

bool SingleInstance::Acquire(const wchar_t* mutex_name, const wchar_t* cmdline_to_forward) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name);
    const DWORD err = GetLastError();
    if (!mutex) {
        // Should not happen; running a second copy is better than not running at all.
        CT_LOG_WARN("CreateMutexW failed (%lu); single-instance check skipped", err);
        return true;
    }
    if (err != ERROR_ALREADY_EXISTS) {
        g_mutex = mutex;
        return true;
    }
    CloseHandle(mutex);

    // The first instance may still be starting up: give its main window a moment to appear.
    HWND target = nullptr;
    for (int attempt = 0; attempt < 40 && !target; ++attempt) {
        target = FindWindowW(kMainWindowClass, nullptr);
        if (!target) Sleep(50);
    }
    if (!target) {
        CT_LOG_WARN("another instance holds the mutex but has no main window; exiting");
        return false;
    }

    DWORD owner_pid = 0;
    GetWindowThreadProcessId(target, &owner_pid);
    AllowSetForegroundWindow(owner_pid);   // so the running instance may bring itself to the front

    const wchar_t* text = cmdline_to_forward ? cmdline_to_forward : L"";
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataCommandLine;
    cds.cbData = static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(text);
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 2000, &result))
        CT_LOG_WARN("forwarding the command line to the running instance failed (%lu)", GetLastError());
    else
        CT_LOG_INFO("another instance is running (pid %lu); command line forwarded", owner_pid);
    return false;
}

void SingleInstance::Release() {
    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}

} // namespace ct::platform
