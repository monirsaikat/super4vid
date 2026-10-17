#pragma once
// tests/common/test_media.h
// Locates test media. Tests run with the working directory tests/ (ct_add_test), so the committed files are in
// data/. Until P04's generator has produced tests/data/pattern_*.mp4, the P06 helper ct_gen_test_media can write the
// same patterns to out/testmedia/ (git-ignored), which is searched second. Tests CT_SKIP when nothing is found.

#include <ct/platform/win.h>

#include <string>

namespace ct::test {

inline bool FileExistsUtf8(const std::string& path) {
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Returns the path of `name` (e.g. "pattern_1080p60_5s.mp4"), or an empty string when it does not exist.
inline std::string FindTestMedia(const char* name) {
    const std::string candidates[] = {std::string("data/") + name, std::string("../out/testmedia/") + name,
                                      std::string("tests/data/") + name, std::string("out/testmedia/") + name};
    for (const std::string& c : candidates)
        if (FileExistsUtf8(c)) return c;
    return std::string();
}

} // namespace ct::test

#define CT_REQUIRE_MEDIA(var, name)                                                \
    const std::string var = ::ct::test::FindTestMedia(name);                       \
    if (var.empty()) {                                                             \
        CT_SKIP(name " missing (tests/data or out/testmedia; run ct_gen_test_media)"); \
    }
