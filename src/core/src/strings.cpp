#include <ct/core/strings.h>

#include "int128_internal.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>

namespace ct {

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return std::wstring();
    // Without MB_ERR_INVALID_CHARS, invalid sequences become U+FFFD instead of failing the whole call.
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

namespace {

void AppendFormatV(std::string& out, const char* fmt, va_list args) {
    if (!fmt) return;
    char stack[512];
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(stack, sizeof(stack), fmt, copy);
    va_end(copy);
    if (n <= 0) return;
    if (static_cast<size_t>(n) < sizeof(stack)) {
        out.append(stack, static_cast<size_t>(n));
        return;
    }
    // Second pass: the exact size is known now.
    const size_t old_size = out.size();
    out.resize(old_size + static_cast<size_t>(n) + 1);
    std::vsnprintf(out.data() + old_size, static_cast<size_t>(n) + 1, fmt, args);
    out.resize(old_size + static_cast<size_t>(n));
}

bool IsSep(char c) { return c == '\\' || c == '/'; }

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Length of a "C:" drive prefix (2) or 0.
size_t DrivePrefix(std::string_view p) {
    if (p.size() >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'))) return 2;
    return 0;
}

} // namespace

std::string FormatV(const char* fmt, va_list args) {
    std::string out;
    AppendFormatV(out, fmt, args);
    return out;
}

std::string Format(const char* fmt, ...) {
    std::string out;
    va_list args;
    va_start(args, fmt);
    AppendFormatV(out, fmt, args);
    va_end(args);
    return out;
}

void AppendFormat(std::string& out, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    AppendFormatV(out, fmt, args);
    va_end(args);
}

std::vector<std::string_view> Split(std::string_view s, char sep, bool keep_empty) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        const size_t pos = s.find(sep, start);
        const size_t len = pos == std::string_view::npos ? std::string_view::npos : pos - start;
        const std::string_view piece = s.substr(start, len);
        if (keep_empty || !piece.empty()) parts.push_back(piece);
        if (pos == std::string_view::npos) break;
        start = pos + 1;
    }
    return parts;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
    return s;
}

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

bool EndsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}

std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = LowerAscii(c);
    return out;
}

std::string PathJoin(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    std::string_view head = a;
    while (!head.empty() && IsSep(head.back())) head.remove_suffix(1);
    while (!b.empty() && IsSep(b.front())) b.remove_prefix(1);
    std::string out;
    out.reserve(head.size() + 1 + b.size());
    out.append(head);
    out.push_back('\\');   // also keeps the root: "\\" + "x" -> "\\x"
    out.append(b);
    return out;
}

std::string_view PathName(std::string_view p) {
    size_t start = DrivePrefix(p);
    for (size_t i = start; i < p.size(); ++i)
        if (IsSep(p[i])) start = i + 1;
    return p.substr(start);
}

std::string_view PathParent(std::string_view p) {
    const size_t drive = DrivePrefix(p);
    size_t last = std::string_view::npos;
    for (size_t i = drive; i < p.size(); ++i)
        if (IsSep(p[i])) last = i;
    if (last == std::string_view::npos) return p.substr(0, drive);   // "C:a.txt" -> "C:", "a.txt" -> ""
    // Collapse a run of separators ("a\\\\b") but keep the root separator ("C:\\", "\\").
    size_t end = last;
    while (end > drive && IsSep(p[end - 1])) --end;
    if (end == drive) return p.substr(0, drive + 1);
    return p.substr(0, end);
}

std::string_view PathExt(std::string_view p) {
    const std::string_view name = PathName(p);
    if (name == "." || name == "..") return std::string_view();
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return std::string_view();
    return name.substr(dot);
}

std::string_view PathStem(std::string_view p) {
    const std::string_view name = PathName(p);
    const std::string_view ext = PathExt(p);
    return name.substr(0, name.size() - ext.size());
}

std::string Uuid4() {
    uint8_t b[16] = {};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        // Practically unreachable; mix QPC and the pid so the result is still unlikely to collide.
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        uint64_t x = static_cast<uint64_t>(q.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
        for (int i = 0; i < 16; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            b[i] = static_cast<uint8_t>(x);
        }
    }
    b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);   // version 4
    b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);   // RFC 4122 variant
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHex[b[i] >> 4]);
        out.push_back(kHex[b[i] & 15]);
    }
    return out;
}

std::string FormatTimecode(TimeUs t, int fps_num, int fps_den, bool with_frames) {
    const bool negative = t < 0;
    if (negative) t = -t;
    int64_t frac = 0;
    int64_t total_s = 0;
    if (with_frames && fps_num > 0 && fps_den > 0) {
        // Snap to the start of the displayed frame, then count frames from the first frame starting in that second.
        const int64_t index = detail::FrameIndexAtImpl(t, fps_num, fps_den);
        const int64_t start = detail::FrameTimeUsImpl(index, fps_num, fps_den);
        total_s = start / kUsPerSecond;
        const int64_t first = detail::FrameIndexAtImpl(total_s * kUsPerSecond - 1, fps_num, fps_den) + 1;
        frac = index - first;
    } else {
        total_s = t / kUsPerSecond;
        frac = (t % kUsPerSecond) / 1000;
    }
    const long long h = static_cast<long long>(total_s / 3600);
    const int m = static_cast<int>((total_s / 60) % 60);
    const int s = static_cast<int>(total_s % 60);
    char buf[48];
    std::snprintf(buf, sizeof(buf), with_frames ? "%s%02lld:%02d:%02d.%02lld" : "%s%02lld:%02d:%02d.%03lld",
                  negative ? "-" : "", h, m, s, static_cast<long long>(frac));
    return std::string(buf);
}

std::string FormatBytes(uint64_t bytes) {
    static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    if (bytes < 1024) return Format("%llu B", static_cast<unsigned long long>(bytes));
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 6) {
        v /= 1024.0;
        ++unit;
    }
    return Format("%.1f %s", v, kUnits[unit]);
}

uint32_t Fnv1a32(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint64_t Fnv1a64(const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace ct
