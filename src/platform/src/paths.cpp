#define CT_LOG_CATEGORY "platform"
#include <ct/platform/paths.h>

#include <ct/core/log.h>
#include <ct/platform/win.h>

#include <ct/core/strings.h>

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

namespace ct::platform {

namespace {

struct PathsData {
    std::string logs, crashes, settings, autosave, recordings, exe_dir;
    bool portable = false;
    bool initialized = false;
};

PathsData& Data() {
    static PathsData data;
    return data;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path)) && path) result = path;
    CoTaskMemFree(path);
    return result;
}

// Creates the directory and all missing parents; returns the UTF-8 path.
std::string Ensure(const std::wstring& dir) {
    const int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS)
        CT_LOG_WARN("could not create %ls (error %d)", dir.c_str(), rc);
    return WideToUtf8(dir);
}

} // namespace

void Paths::Init() {
    PathsData& d = Data();
    if (d.initialized) return;
    d.initialized = true;

    wchar_t exe[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::wstring exe_dir(exe, n);
    const size_t slash = exe_dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exe_dir.resize(slash);
    d.exe_dir = WideToUtf8(exe_dir);

    d.portable = GetFileAttributesW((exe_dir + L"\\portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring local, roaming, recordings;
    if (d.portable) {
        local = roaming = exe_dir + L"\\data";
        recordings = exe_dir + L"\\data\\recordings";
    } else {
        local = KnownFolder(FOLDERID_LocalAppData) + L"\\super4vid";
        roaming = KnownFolder(FOLDERID_RoamingAppData) + L"\\super4vid";
        recordings = KnownFolder(FOLDERID_Videos) + L"\\super4vid";
    }
    d.logs = Ensure(local + L"\\logs");
    d.crashes = Ensure(local + L"\\crashes");
    d.autosave = Ensure(local + L"\\autosave");
    d.settings = Ensure(roaming + (d.portable ? L"\\settings" : L""));
    d.recordings = Ensure(recordings);
}

const std::string& Paths::LogsDir() { return Data().logs; }
const std::string& Paths::CrashDir() { return Data().crashes; }
const std::string& Paths::SettingsDir() { return Data().settings; }
const std::string& Paths::AutosaveDir() { return Data().autosave; }
const std::string& Paths::DefaultRecordingsDir() { return Data().recordings; }
const std::string& Paths::ExeDir() { return Data().exe_dir; }
bool Paths::Portable() { return Data().portable; }

} // namespace ct::platform
