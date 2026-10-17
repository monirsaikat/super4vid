#pragma once
// ct/core/strings.h
// UTF-8 <-> UTF-16 conversion, printf-style formatting into std::string, splitting/trimming, path-string helpers,
// UUIDs, timecodes, byte sizes and FNV-1a hashes. Strings are UTF-8 everywhere except at the Win32 boundary.
// Thread model: all functions are pure (or use only thread-safe OS calls) and may be called from any thread.

#include <ct/core/types.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ct {

// Invalid UTF-8 bytes / unpaired surrogates become U+FFFD.
std::wstring Utf8ToWide(std::string_view s);
std::string  WideToUtf8(std::wstring_view s);

// printf-style; never truncates. %s takes const char* (use .c_str()), %ls takes const wchar_t*.
std::string  Format(const char* fmt, ...);
std::string  FormatV(const char* fmt, va_list args);
void         AppendFormat(std::string& out, const char* fmt, ...);

// keep_empty = false drops empty pieces ("a,,b" -> a, b). With keep_empty = true, "" -> {""} and "a," -> {"a", ""}.
// The views point into `s`.
std::vector<std::string_view> Split(std::string_view s, char sep, bool keep_empty = false);
std::string_view Trim(std::string_view s);   // ASCII whitespace
bool StartsWith(std::string_view s, std::string_view p);
bool EndsWith(std::string_view s, std::string_view p);
bool IEquals(std::string_view a, std::string_view b);   // ASCII case-insensitive
std::string ToLowerAscii(std::string_view s);

// Path helpers treat both '\' and '/' as separators. They work on strings only (no file system access).
// PathJoin("a\\", "/b") == "a\b"; an empty side returns the other side.
std::string PathJoin(std::string_view a, std::string_view b);
std::string_view PathExt(std::string_view p);      // "C:\x\a.tar.gz" -> ".gz"; ".gitignore" -> ""
std::string_view PathStem(std::string_view p);     // "a.tar.gz" -> "a.tar"
std::string_view PathParent(std::string_view p);   // "C:\x\a.txt" -> "C:\x"; "C:\a.txt" -> "C:\\"; "a.txt" -> ""
std::string_view PathName(std::string_view p);     // "C:\x\a.txt" -> "a.txt"; "C:\x\\" -> ""

std::string Uuid4();   // random (BCryptGenRandom), "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx", lowercase

// with_frames: "HH:MM:SS.FF" of the frame shown at t (frames counted from the first frame that starts in that
// second, so 29.97 fps shows .00-.29). Otherwise "HH:MM:SS.mmm". Negative times get a leading '-'.
std::string FormatTimecode(TimeUs t, int fps_num, int fps_den, bool with_frames);
std::string FormatBytes(uint64_t bytes);   // 1024-based: "512 B", "1.5 KB", "1.5 MB", "2.0 GB"

uint32_t Fnv1a32(const void* data, size_t len);
uint64_t Fnv1a64(const void* data, size_t len);

} // namespace ct
