#pragma once
// ct/platform/paths.h
// Well-known directories (UTF-8, no trailing separator), created on Init():
//   logs, crashes, autosave: %LOCALAPPDATA%\Camtesia\{logs,crashes,autosave}
//   settings:                %APPDATA%\Camtesia
//   recordings:              %USERPROFILE%\Videos\Camtesia
// Portable mode (a file named portable.txt next to the exe): everything lives under <exe dir>\data\.
// Thread model: Init() once on the main thread before any other use; afterwards read-only from any thread.

#include <string>

namespace ct::platform {

struct Paths {
    static void Init();
    static const std::string& LogsDir();
    static const std::string& CrashDir();
    static const std::string& SettingsDir();
    static const std::string& AutosaveDir();
    static const std::string& DefaultRecordingsDir();
    static const std::string& ExeDir();
    static bool Portable();
};

} // namespace ct::platform
