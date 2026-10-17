#pragma once
// ct/ui/first_run.h
// First run (P11 11.6): when settings.json does not exist yet, create the app folders and show the one-time toast
// "Press F9 to start recording. Recordings are saved to Videos\Camtesia" (no other dialogs). Writing the settings
// file ends the first run, so the toast appears exactly once.
// Thread model: IsFirstRun* do one GetFileAttributes (cheap; call once at startup, before the first frame).
// Run() is UI thread (it pushes the toast); the folder creation and the settings write go to the IO pool.
// Paths::Init() must have run.

#include <ct/core/config.h>

#include <string>
#include <string_view>

namespace ct::ui {

class ToastStack;

// %APPDATA%\Camtesia\settings.json (platform::Paths::SettingsDir() + "\\settings.json"; portable mode included).
std::string SettingsFilePath();

// True when SettingsFilePath() does not exist.
bool IsFirstRun();

struct FirstRun {
    // True when `settings_path` does not exist (tests pass a temp directory).
    static bool IsFirstRunAt(std::string_view settings_path);
    // Folders a first run creates: the settings directory, the recordings folder (settings.recording.output_dir or
    // Paths::DefaultRecordingsDir()), logs, crashes and autosave. Blocking; FirstRun::Run calls it on the IO pool.
    static bool CreateFolders(const Settings& settings, std::string_view settings_path);
    // If IsFirstRunAt(settings_path): pushes the toast (12 s, Info), then on the IO pool creates the folders and
    // saves `settings` to settings_path. Returns whether it was a first run. `toasts` may be null (no UI).
    static bool Run(ToastStack* toasts, const Settings& settings, std::string_view settings_path);
};

} // namespace ct::ui
