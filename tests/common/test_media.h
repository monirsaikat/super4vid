#pragma once
// tests/common/test_media.h
// Locates test media. Tests run with the working directory tests/ (ct_add_test), so the committed files are in
// data/ (written by tools/gen_test_media, see tests/data/README.md). Tests CT_SKIP when the file is not found.

#include <ct/platform/win.h>

#include <string>

namespace ct::test {

inline bool FileExistsUtf8(const std::string& path) {
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Returns the path of `name` (e.g. "pattern_1080p60_5s.mp4"), or an empty string when it does not exist.
inline std::string FindTestMedia(const char* name) {
    const std::string candidates[] = {std::string("data/") + name, std::string("tests/data/") + name};
    for (const std::string& c : candidates)
        if (FileExistsUtf8(c)) return c;
    return std::string();
}

} // namespace ct::test

#define CT_REQUIRE_MEDIA(var, name)                                                \
    const std::string var = ::ct::test::FindTestMedia(name);                       \
    if (var.empty()) {                                                             \
        CT_SKIP(name " missing (tests/data; run tools/gen_test_media --all tests/data)"); \
    }
