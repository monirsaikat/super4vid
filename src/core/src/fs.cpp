#define CT_LOG_CATEGORY "fs"
#include <ct/core/fs.h>

#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

namespace ct::fs {

namespace {

constexpr size_t kLongPath = 240;

bool HasPrefix(const std::wstring& w) { return w.size() >= 4 && w.compare(0, 4, L"\\\\?\\") == 0; }

// "\\?\UNC\server\share" -> "\\server\share", "\\?\C:\x" -> "C:\x".
std::wstring StripPrefix(const std::wstring& w) {
    if (!HasPrefix(w)) return w;
    if (w.size() >= 8 && _wcsnicmp(w.c_str() + 4, L"UNC\\", 4) == 0) return L"\\" + w.substr(7);
    return w.substr(4);
}

std::wstring Normalized(std::string_view utf8) {
    std::wstring w = Utf8ToWide(utf8);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

std::wstring FullPathW(const std::wstring& w) {
    if (w.empty()) return std::wstring();
    DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (n == 0) return w;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(w.c_str(), n, full.data(), nullptr);
    if (n == 0 || n >= full.size()) return w;
    full.resize(n);
    return full;
}

std::wstring AddPrefix(const std::wstring& full) {
    if (HasPrefix(full)) return full;
    if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

int64_t FileTimeToUnix(const FILETIME& ft) {
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (static_cast<int64_t>(t) - 116444736000000000ll) / 10000000ll;
}

struct RawEntry {
    std::wstring name;
    DWORD attributes;
    uint64_t size;
    FILETIME mtime;
};

bool ListRaw(std::string_view dir, std::vector<RawEntry>& out) {
    out.clear();
    const std::wstring pattern = ToWinPath(PathJoin(dir, "*"));
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;   // empty drive root
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        out.push_back(RawEntry{fd.cFileName, fd.dwFileAttributes,
                               (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, fd.ftLastWriteTime});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return true;
}

bool SamePathComponent(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// Splits an absolute, prefix-free path into its root ("C:" or "\\server\share") and the components after it.
void SplitAbsolute(const std::wstring& path, std::wstring& root, std::vector<std::wstring>& parts) {
    root.clear();
    parts.clear();
    size_t pos = 0;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        size_t server_end = path.find(L'\\', 2);
        size_t share_end = server_end == std::wstring::npos ? std::wstring::npos : path.find(L'\\', server_end + 1);
        root = path.substr(0, share_end);
        pos = share_end == std::wstring::npos ? path.size() : share_end + 1;
    } else if (path.size() >= 2 && path[1] == L':') {
        root = path.substr(0, 2);
        pos = 2;
    }
    while (pos < path.size()) {
        size_t next = path.find(L'\\', pos);
        if (next == std::wstring::npos) next = path.size();
        if (next > pos) parts.push_back(path.substr(pos, next - pos));
        pos = next + 1;
    }
}

// COM for the shell calls: initialize STA if this thread has no apartment yet; leave an existing one alone.
struct ComScope {
    bool owned = false;
    ComScope() { owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)); }
    ~ComScope() {
        if (owned) CoUninitialize();
    }
};

} // namespace

std::wstring ToWinPath(std::string_view utf8) {
    std::wstring w = Normalized(utf8);
    if (HasPrefix(w) || w.size() <= kLongPath) return w;
    return AddPrefix(FullPathW(w));   // \\?\ disables normalization, so it needs an absolute, clean path
}

std::string Absolute(std::string_view path) {
    if (path.empty()) return std::string();
    const std::wstring w = Normalized(path);
    return WideToUtf8(StripPrefix(FullPathW(w)));
}

bool ReadAll(std::string_view path, std::string& out) {
    out.clear();
    HANDLE file = CreateFileW(ToWinPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 64u << 20));
            DWORD got = 0;
            ok = ReadFile(file, out.data() + done, chunk, &got, nullptr) != FALSE;
            if (ok && got == 0) break;   // file shrank while reading
            done += got;
        }
        out.resize(done);
    }
    CloseHandle(file);
    if (!ok) out.clear();
    return ok;
}

bool WriteAtomic(std::string_view path, std::string_view data) {
    const std::wstring target = ToWinPath(path);
    const std::wstring temp = ToWinPath(std::string(path) + ".tmp");   // same directory: the rename never crosses volumes
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        CT_LOG_WARN("WriteAtomic: cannot create %ls (error %lu)", temp.c_str(), GetLastError());
        return false;
    }
    bool ok = true;
    size_t done = 0;
    while (ok && done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 64u << 20));
        DWORD wrote = 0;
        ok = WriteFile(file, data.data() + done, chunk, &wrote, nullptr) != FALSE && wrote == chunk;
        done += wrote;
    }
    ok = ok && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) {
        CT_LOG_WARN("WriteAtomic: writing %ls failed (error %lu)", target.c_str(), GetLastError());
        DeleteFileW(temp.c_str());
    }
    return ok;
}

bool Exists(std::string_view path) { return GetFileAttributesW(ToWinPath(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

bool IsDirectory(std::string_view path) {
    const DWORD a = GetFileAttributesW(ToWinPath(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool CreateDirectories(std::string_view path) {
    if (path.empty()) return false;
    const std::string abs = Absolute(path);
    if (IsDirectory(abs)) return true;
    std::wstring root;
    std::vector<std::wstring> parts;
    SplitAbsolute(Utf8ToWide(abs), root, parts);
    std::wstring cur = root;
    for (const std::wstring& part : parts) {
        cur += L'\\';
        cur += part;
        const std::wstring win = ToWinPath(WideToUtf8(cur));
        if (!CreateDirectoryW(win.c_str(), nullptr)) {
            const DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS && err != ERROR_ACCESS_DENIED) {   // access denied: e.g. a drive root
                CT_LOG_WARN("CreateDirectories: %ls failed (error %lu)", win.c_str(), err);
                return false;
            }
        }
    }
    return IsDirectory(abs);
}

bool Delete(std::string_view path) {
    const std::wstring w = ToWinPath(path);
    const DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(w.c_str()) != FALSE;
    return DeleteFileW(w.c_str()) != FALSE;
}

bool DeleteTree(std::string_view dir) {
    const std::wstring w = ToWinPath(dir);
    const DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return Delete(dir);
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {   // a junction/symlink is removed itself, never followed
        std::vector<RawEntry> entries;
        ListRaw(dir, entries);
        for (const RawEntry& e : entries) {
            const std::string child = PathJoin(dir, WideToUtf8(e.name));
            const std::wstring cw = ToWinPath(child);
            if (e.attributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (e.attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    RemoveDirectoryW(cw.c_str());
                else
                    DeleteTree(child);
            } else {
                if (e.attributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(cw.c_str(), e.attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
                DeleteFileW(cw.c_str());
            }
        }
    }
    RemoveDirectoryW(w.c_str());
    return GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES;
}

bool List(std::string_view dir, std::vector<Entry>& out) {
    out.clear();
    std::vector<RawEntry> raw;
    if (!ListRaw(dir, raw)) return false;
    out.reserve(raw.size());
    for (const RawEntry& e : raw)
        out.push_back(Entry{WideToUtf8(e.name), (e.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, e.size, FileTimeToUnix(e.mtime)});
    return true;
}

uint64_t FileSize(std::string_view path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(ToWinPath(path).c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

uint64_t FreeSpaceBytes(std::string_view path_on_volume) {
    std::string dir = Absolute(path_on_volume);
    while (!dir.empty() && !IsDirectory(dir)) {
        const std::string_view parent = PathParent(dir);
        if (parent.size() == dir.size()) break;
        dir = std::string(parent);
    }
    std::wstring w = ToWinPath(dir);
    if (!w.empty() && w.back() != L'\\') w += L'\\';
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExW(w.c_str(), &avail, nullptr, nullptr)) return 0;
    return avail.QuadPart;
}

std::string TempPath(std::string_view ext) {
    wchar_t buf[MAX_PATH + 2] = {};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(buf)), buf);
    std::string dir = n > 0 && n < std::size(buf) ? WideToUtf8(std::wstring_view(buf, n)) : std::string(".");
    std::string name = "camtesia-" + Uuid4();
    if (!ext.empty() && ext.front() != '.') name.push_back('.');
    name.append(ext);
    return PathJoin(dir, name);
}

std::string Relative(std::string_view base_dir, std::string_view path) {
    const std::string abs_path = Absolute(path);
    std::wstring base_root, path_root;
    std::vector<std::wstring> base_parts, path_parts;
    SplitAbsolute(Utf8ToWide(Absolute(base_dir)), base_root, base_parts);
    SplitAbsolute(Utf8ToWide(abs_path), path_root, path_parts);
    if (base_root.empty() || !SamePathComponent(base_root, path_root)) return abs_path;
    size_t common = 0;
    while (common < base_parts.size() && common < path_parts.size() &&
           SamePathComponent(base_parts[common], path_parts[common]))
        ++common;
    std::wstring rel;
    for (size_t i = common; i < base_parts.size(); ++i) rel += rel.empty() ? L".." : L"\\..";
    for (size_t i = common; i < path_parts.size(); ++i) {
        if (!rel.empty()) rel += L'\\';
        rel += path_parts[i];
    }
    return rel.empty() ? std::string(".") : WideToUtf8(rel);
}

bool SameVolume(std::string_view a, std::string_view b) {
    auto volume_of = [](std::string_view p, std::wstring& out) {
        const std::wstring w = ToWinPath(Absolute(p));
        std::wstring mount(w.size() + 2, L'\0');
        if (!GetVolumePathNameW(w.c_str(), mount.data(), static_cast<DWORD>(mount.size()))) return false;
        mount.resize(wcslen(mount.c_str()));
        wchar_t guid[64] = {};
        // The volume GUID also matches two mount points (or a subst drive) of the same volume.
        if (GetVolumeNameForVolumeMountPointW(mount.c_str(), guid, static_cast<DWORD>(std::size(guid))))
            out = guid;
        else
            out = StripPrefix(mount);
        return true;
    };
    std::wstring va, vb;
    if (!volume_of(a, va) || !volume_of(b, vb)) return false;
    return SamePathComponent(va, vb);
}

bool ShowInExplorer(std::string_view path) {
    ComScope com;
    const std::wstring w = StripPrefix(FullPathW(Normalized(path)));
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(w.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;
    const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    CoTaskMemFree(pidl);
    return SUCCEEDED(hr);
}

bool OpenWithShell(std::string_view path) {
    ComScope com;
    const std::wstring w = StripPrefix(FullPathW(Normalized(path)));
    const HINSTANCE rc = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(rc) > 32;
}

} // namespace ct::fs
