// Entry point for ct_playback_tests: MTA COM (Media Foundation, WASAPI, WIC) and per-monitor DPI awareness.
#include <ct/core/async.h>
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <objbase.h>

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int failures = ct::test::RunAll(argc, argv);
    ct::Async::Shutdown();
    if (SUCCEEDED(hr)) CoUninitialize();
    return failures;
}
