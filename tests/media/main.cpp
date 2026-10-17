// Entry point for ct_media_tests: MTA COM (Media Foundation, WIC) and per-monitor DPI awareness, then the runner.
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <objbase.h>

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int failures = ct::test::RunAll(argc, argv);
    if (SUCCEEDED(hr)) CoUninitialize();
    return failures;
}
