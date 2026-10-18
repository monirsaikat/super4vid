// Recorder / export failures -> banner text and recovery actions (P11 11.5).
#include <ct/ui/recorder_errors.h>

#include <ct/core/strings.h>
#include <ct/ui/strings.h>

#include <ct/platform/win.h>

namespace ct::ui {

namespace {

void AppendLine(std::string& s, std::string_view line) {
    if (line.empty()) return;
    if (!s.empty()) s += '\n';
    s.append(line);
}

std::string ParentOf(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool IsFolderError(long hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == E_ACCESSDENIED;
}

} // namespace

const char* ErrorActionLabel(int action) {
    switch (action) {
    case kActionChooseFolder: return str::kErrActionChooseFolder;
    case kActionOpenEditor: return str::kErrActionOpenEditor;
    case kActionShowFolder: return str::kErrActionShowFolder;
    case kActionRetry: return str::kErrActionRetry;
    default: return "";
    }
}

ErrorView DescribeFailure(const recorder::SessionFailure& f) {
    using recorder::FailureCause;
    ErrorView v;
    const std::string where = f.output_dir.empty() ? f.bundle_path : f.output_dir;
    int n = 0;
    auto add = [&v, &n](int a) {
        if (n < 3) v.actions[n++] = a;
    };
    switch (f.cause) {
    case FailureCause::DiskFull:
        v.message = ErrorMessage(f.saved ? ErrorKind::DiskFull : ErrorKind::DiskFullNotStarted, where);
        add(kActionChooseFolder);
        break;
    case FailureCause::OutputFolder:
        v.message = ErrorMessage(ErrorKind::OutputFolder, where);
        add(kActionChooseFolder);
        break;
    case FailureCause::DeviceLost:
        v.message = ErrorMessage(f.saved ? ErrorKind::DeviceLost : ErrorKind::DeviceLostNotSaved);
        v.recreate_device = true;
        AppendLine(v.extra, str::kErrDeviceRecreated);
        break;
    case FailureCause::NoEncoder: v.message = ErrorMessage(ErrorKind::NoEncoder); break;
    case FailureCause::AccessLost: v.message = ErrorMessage(ErrorKind::AccessLost); break;
    case FailureCause::CaptureFailed:
    case FailureCause::Other:
    default: v.message = Format(str::kToastRecordingFailed, f.message.c_str()); break;
    }
    if (f.saved) {
        add(kActionOpenEditor);
        add(kActionShowFolder);
        if (!f.finalized) AppendLine(v.extra, str::kErrNotFinalized);
    }
    if (f.cause != FailureCause::CaptureFailed && f.cause != FailureCause::Other) AppendLine(v.extra, f.message);
    AppendLine(v.extra, f.saved ? f.bundle_path : where);
    return v;
}

ErrorView DescribeWarning(const recorder::SessionWarning& w) {
    using recorder::WarningCause;
    ErrorView v;
    switch (w.cause) {
    case WarningCause::MicBusy: v.message = ErrorMessage(ErrorKind::MicBusy); break;
    case WarningCause::AudioDeviceLost: v.message = ErrorMessage(ErrorKind::MicLost); break;
    default:
        v.message = w.message;
        v.banner = false;
        return v;
    }
    AppendLine(v.extra, w.message);
    return v;
}

ErrorView DescribeExportFailure(long hr, const std::string& message, const std::string& path) {
    ErrorView v;
    ErrorKind k;
    const bool known = ErrorKindForHr(hr, &k);
    if (known && k == ErrorKind::DiskFull) {
        v.message = ErrorMessage(ErrorKind::ExportDiskFull, path);
        v.actions[0] = kActionChooseFolder;
    } else if (IsFolderError(hr)) {
        v.message = ErrorMessage(ErrorKind::ExportFolder, ParentOf(path).empty() ? path : ParentOf(path));
        v.actions[0] = kActionChooseFolder;
    } else if (known && k == ErrorKind::DeviceLost) {
        v.message = ErrorMessage(ErrorKind::ExportDeviceLost);
        v.actions[0] = kActionRetry;
    } else if (known && k == ErrorKind::NoEncoder) {
        v.message = ErrorMessage(ErrorKind::NoEncoder);
    } else {
        v.message = Format(str::kExportFailed, message.c_str());
        v.actions[0] = kActionRetry;
    }
    AppendLine(v.extra, message);
    AppendLine(v.extra, path);
    return v;
}

void ShowErrorView(ErrorBanner& banner, const ErrorView& v, long hr) {
    banner.Show(v.message, hr, v.extra);
    for (int i = 0; i < 3 && v.actions[i] != kActionNone; ++i) banner.AddAction(v.actions[i], ErrorActionLabel(v.actions[i]), i == 0);
}

} // namespace ct::ui
