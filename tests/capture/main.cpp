// Entry point for ct_capture_tests: per-monitor DPI awareness first (DuplicateOutput1 and physical-pixel monitor
// rects need it; the app gets it from its manifest), then the shared runner.
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return ct::test::RunAll(argc, argv);
}
