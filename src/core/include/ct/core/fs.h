#pragma once
// ct/core/fs.h
// File system helpers on UTF-8 paths. Inside ct::fs paths become UTF-16 with '\\' separators, and paths longer than
// 240 characters get the \\?\ prefix, so callers never deal with MAX_PATH.
// Thread model: every function is thread-safe but blocking (disk IO): never call these on the UI thread for user
// data; use ct::Async::Run. ShowInExplorer/OpenWithShell initialize COM (STA) on the calling thread if needed.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ct::fs {

bool ReadAll(std::string_view path, std::string& out);
// Writes <path>.tmp in the destination directory, FlushFileBuffers, then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH).
// On failure the original file is untouched and the temp file is removed.
bool WriteAtomic(std::string_view path, std::string_view data);
bool Exists(std::string_view path);
bool IsDirectory(std::string_view path);
bool CreateDirectories(std::string_view path);   // true if the directory exists afterwards
bool Delete(std::string_view path);              // file or empty directory; true if nothing is left at path
bool DeleteTree(std::string_view dir);           // recursive; does not follow junctions/symlinks; true if dir is gone
struct Entry {
    std::string name;
    bool is_dir;
    uint64_t size;
    int64_t mtime_unix;   // seconds since 1970-01-01 UTC
};
bool List(std::string_view dir, std::vector<Entry>& out);   // without "." and ".."; order as the file system returns it
uint64_t FileSize(std::string_view path);                   // 0 when missing
uint64_t FreeSpaceBytes(std::string_view path_on_volume);   // bytes available to the caller; path may not exist yet
std::string TempPath(std::string_view ext);                  // %TEMP%\camtesia-<uuid><ext>; not created
std::wstring ToWinPath(std::string_view utf8);              // adds \\?\ when > 240 chars, normalizes '/' to '\\'
std::string Absolute(std::string_view path);                // GetFullPathNameW: resolves ".", "..", the current directory
// Path of `path` relative to `base_dir` ("..\\x\\y.txt", "." when equal), case-insensitive. Paths on different
// drives/shares cannot be relative: returns Absolute(path).
std::string Relative(std::string_view base_dir, std::string_view path);
bool SameVolume(std::string_view a, std::string_view b);    // e.g. to know whether MoveFileExW can rename instead of copy
bool ShowInExplorer(std::string_view path);                 // SHOpenFolderAndSelectItems
bool OpenWithShell(std::string_view path);                  // ShellExecuteW "open"

} // namespace ct::fs
