#pragma once
// ct/platform/single_instance.h
// One super4vid per session. A second launch forwards its command line to the running instance's main window
// (class "super4vidMain") with WM_COPYDATA (dwData = 1, UTF-16 with terminator) and exits.
// Dev/test override: when the environment variable CT_INSTANCE_SUFFIX is set (letters, digits, '-', '_', '.'; at most
// 32 characters, anything else is dropped), it is appended to the mutex name and the main window class, so parallel
// worktrees and benches each get their own instance. Unset (every normal user) = the release names unchanged.
// Thread model: main thread, before the UI is created.

#include <string>
#include <string_view>

namespace ct::platform {

struct SingleInstance {
    static constexpr unsigned long kCopyDataCommandLine = 1;   // COPYDATASTRUCT::dwData
    static constexpr const wchar_t* kBaseMutexName = L"Local\\super4vid.Instance";
    static constexpr const wchar_t* kBaseWindowClass = L"super4vidMain";

    // The sanitized CT_INSTANCE_SUFFIX (empty when unset or nothing valid is left).
    static std::wstring Suffix();
    static std::wstring SanitizeSuffix(std::wstring_view raw);   // pure (tests)
    // kBaseMutexName / kBaseWindowClass with "." + Suffix() appended when a suffix is set.
    static std::wstring MutexName();
    static std::wstring MainWindowClass();

    // True: this is the first instance (the mutex is held until Release()). False: forwarded; the caller exits.
    // The forward target is the window of class MainWindowClass().
    static bool Acquire(const wchar_t* mutex_name, const wchar_t* cmdline_to_forward);
    static void Release();
};

} // namespace ct::platform
