// First-run detection, folders and the one-time toast (P11 11.6).
#define CT_LOG_CATEGORY "ui"
#include <ct/ui/first_run.h>

#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/platform/paths.h>
#include <ct/ui/strings.h>
#include <ct/ui/widgets.h>

namespace ct::ui {

std::string SettingsFilePath() {
    const std::string& dir = platform::Paths::SettingsDir();
    if (dir.empty()) return {};
    return PathJoin(dir, "settings.json");
}

bool IsFirstRun() {
    const std::string p = SettingsFilePath();
    return !p.empty() && FirstRun::IsFirstRunAt(p);
}

bool FirstRun::IsFirstRunAt(std::string_view settings_path) { return !fs::Exists(settings_path); }

bool FirstRun::CreateFolders(const Settings& settings, std::string_view settings_path) {
    bool ok = true;
    const std::string_view settings_dir = PathParent(settings_path);
    if (!settings_dir.empty()) ok &= fs::CreateDirectories(settings_dir);
    const std::string& rec = settings.recording.output_dir.empty() ? platform::Paths::DefaultRecordingsDir()
                                                                   : settings.recording.output_dir;
    if (!rec.empty()) ok &= fs::CreateDirectories(rec);
    for (const std::string* d : {&platform::Paths::LogsDir(), &platform::Paths::CrashDir(), &platform::Paths::AutosaveDir()})
        if (!d->empty()) ok &= fs::CreateDirectories(*d);
    return ok;
}

bool FirstRun::Run(ToastStack* toasts, const Settings& settings, std::string_view settings_path) {
    if (!IsFirstRunAt(settings_path)) return false;
    CT_LOG_INFO("first run: no %.*s", static_cast<int>(settings_path.size()), settings_path.data());
    if (toasts) toasts->Push(str::kFirstRunToast, ToastKind::Info, {}, 12.0f);
    Async::Run([s = settings, path = std::string(settings_path)] {
        if (!CreateFolders(s, path)) CT_LOG_WARN("first run: some folders could not be created");
        Err err;
        if (!s.Save(path, &err)) CT_LOG_WARN("first run: %s", err.message.c_str());
    });
    return true;
}

} // namespace ct::ui
