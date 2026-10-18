#pragma once
// ct/ui/recorder_errors.h
// Error UX of the recorder (P11 11.5): turns a recorder::SessionFailure / SessionWarning into the banner the recorder
// bar shows (plain-language strings.h message, the recovery actions that apply, extra Details lines), and an export
// failure (ExportJob::Progress) into the export dialog's banner.
// Thread model: pure functions, any thread.

#include <ct/recorder/recording_session.h>
#include <ct/ui/error_ux.h>

#include <string>

namespace ct::ui {

enum ErrorAction : int {
    kActionNone = 0,
    kActionChooseFolder = 1,   // pick another output folder (recordings: settings.recording.output_dir)
    kActionOpenEditor = 2,     // open the kept bundle in the editor
    kActionShowFolder = 3,     // reveal the kept bundle in Explorer
    kActionRetry = 4,          // export: start the same export again
};

struct ErrorView {
    std::string message;       // shown in the banner
    std::string extra;         // Details lines (besides the error code and the log folder)
    int actions[3] = {};       // kAction*, 0-terminated, in button order; the first one is primary
    bool banner = true;        // false: a plain warning toast is enough (warnings only)
    bool recreate_device = false;   // the app must recreate its D3D device before the next recording
};

ErrorView DescribeFailure(const recorder::SessionFailure& f);
ErrorView DescribeWarning(const recorder::SessionWarning& w);
// Export failures: hr/message from ExportJob::Progress, `path` the output file.
ErrorView DescribeExportFailure(long hr, const std::string& message, const std::string& path);

const char* ErrorActionLabel(int action);
// Shows `v` on `banner` with its actions (label from ErrorActionLabel; the first is primary).
void ShowErrorView(ErrorBanner& banner, const ErrorView& v, long hr);

} // namespace ct::ui
