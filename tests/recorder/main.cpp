// Entry point for ct_recorder_tests: per-monitor DPI awareness first (hook coordinates, monitor rects and overlay
// sizes are physical pixels; the app gets it from its manifest), then the shared runner.
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <objbase.h>

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // The UI thread is an STA (docs/02 section 6); LayeredWindow's WIC surface needs COM on its thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int rc = ct::test::RunAll(argc, argv);
    CoUninitialize();
    return rc;
}
