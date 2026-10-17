// Entry point for ct_media_tests: per-monitor DPI awareness (capture-based tests) and an MTA for COM/Media
// Foundation on the test thread, then the shared runner.
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <objbase.h>

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const int rc = ct::test::RunAll(argc, argv);
    CoUninitialize();
    return rc;
}
