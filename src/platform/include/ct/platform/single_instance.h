#pragma once
// ct/platform/single_instance.h
// One Camtesia per session. A second launch forwards its command line to the running instance's main window
// (class "CamtesiaMain") with WM_COPYDATA (dwData = 1, UTF-16 with terminator) and exits.
// Thread model: main thread, before the UI is created.

namespace ct::platform {

struct SingleInstance {
    static constexpr unsigned long kCopyDataCommandLine = 1;   // COPYDATASTRUCT::dwData
    // True: this is the first instance (the mutex is held until Release()). False: forwarded; the caller exits.
    static bool Acquire(const wchar_t* mutex_name, const wchar_t* cmdline_to_forward);
    static void Release();
};

} // namespace ct::platform
