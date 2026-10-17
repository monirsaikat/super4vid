// ct/core/fs.h: atomic writes, long paths, listing, path math. Everything happens under %TEMP%.
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/test/ct_test.h>

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// A fresh directory under %TEMP%, removed when the test ends.
struct TempDir {
    std::string path = ct::fs::TempPath("");
    TempDir() { ct::fs::CreateDirectories(path); }
    ~TempDir() { ct::fs::DeleteTree(path); }
};

bool HasTempLeftovers(const std::string& dir) {
    std::vector<ct::fs::Entry> entries;
    ct::fs::List(dir, entries);
    for (const auto& e : entries)
        if (ct::EndsWith(e.name, ".tmp")) return true;
    return false;
}

} // namespace

CT_TEST(fs_write_read_roundtrip) {
    TempDir dir;
    CT_REQUIRE(ct::fs::IsDirectory(dir.path));
    const std::string file = ct::PathJoin(dir.path, "caf\xC3\xA9 \xF0\x9F\x8E\xA5.json");   // non-ASCII name
    std::string data(100000, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 31);
    CT_CHECK(!ct::fs::Exists(file));
    CT_REQUIRE(ct::fs::WriteAtomic(file, data));
    CT_CHECK(ct::fs::Exists(file));
    CT_CHECK(!ct::fs::IsDirectory(file));
    CT_CHECK_EQ(ct::fs::FileSize(file), uint64_t(data.size()));
    std::string back;
    CT_REQUIRE(ct::fs::ReadAll(file, back));
    CT_CHECK(back == data);
    CT_REQUIRE(ct::fs::WriteAtomic(file, "short"));   // replace
    CT_CHECK(ct::fs::ReadAll(file, back) && back == "short");
    CT_REQUIRE(ct::fs::WriteAtomic(file, ""));
    CT_CHECK(ct::fs::ReadAll(file, back) && back.empty());
    CT_CHECK(!HasTempLeftovers(dir.path));
    CT_CHECK(!ct::fs::ReadAll(ct::PathJoin(dir.path, "missing.txt"), back));
    CT_CHECK_EQ(ct::fs::FileSize(ct::PathJoin(dir.path, "missing.txt")), uint64_t(0));
    CT_CHECK(ct::fs::Delete(file));
    CT_CHECK(!ct::fs::Exists(file));
    CT_CHECK(ct::fs::Delete(file));   // already gone
}

CT_TEST(fs_atomic_write_failure_keeps_original) {
    TempDir dir;
    const std::string file = ct::PathJoin(dir.path, "settings.json");
    CT_REQUIRE(ct::fs::WriteAtomic(file, "original"));
    const std::wstring w = ct::fs::ToWinPath(file);
    CT_REQUIRE(SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_READONLY));
    CT_CHECK(!ct::fs::WriteAtomic(file, "replacement"));   // MoveFileExW cannot replace a read-only file
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::string back;
    CT_CHECK(ct::fs::ReadAll(file, back) && back == "original");
    CT_CHECK(!HasTempLeftovers(dir.path));
    // Destination directory does not exist: fails before touching anything.
    CT_CHECK(!ct::fs::WriteAtomic(ct::PathJoin(dir.path, "no\\such\\dir\\x.json"), "x"));
}

CT_TEST(fs_long_paths) {
    TempDir dir;
    std::string deep = dir.path;
    while (deep.size() < 300) deep = ct::PathJoin(deep, std::string(40, 'd') + std::to_string(deep.size()));
    CT_REQUIRE(deep.size() >= 300);
    CT_REQUIRE(ct::fs::CreateDirectories(deep));
    CT_CHECK(ct::fs::IsDirectory(deep));
    const std::string file = ct::PathJoin(deep, "long-file-name.bin");
    CT_CHECK(ct::fs::ToWinPath(file).rfind(L"\\\\?\\", 0) == 0);
    CT_REQUIRE(ct::fs::WriteAtomic(file, "deep data"));
    std::string back;
    CT_CHECK(ct::fs::ReadAll(file, back) && back == "deep data");
    CT_CHECK_EQ(ct::fs::FileSize(file), uint64_t(9));
    std::vector<ct::fs::Entry> entries;
    CT_CHECK(ct::fs::List(deep, entries));
    CT_REQUIRE(entries.size() == 1);
    CT_CHECK_EQ(entries[0].name, std::string("long-file-name.bin"));
    CT_CHECK(!entries[0].is_dir);
    CT_CHECK_EQ(entries[0].size, uint64_t(9));
    CT_CHECK(entries[0].mtime_unix > 1'700'000'000);   // after 2023
    CT_CHECK_EQ(ct::fs::Absolute(file), file);
    CT_CHECK(ct::fs::Delete(file));
    CT_CHECK(!ct::fs::Exists(file));
    CT_CHECK(ct::fs::DeleteTree(dir.path));
    CT_CHECK(!ct::fs::Exists(deep));
}

CT_TEST(fs_list_and_delete_tree) {
    TempDir dir;
    CT_REQUIRE(ct::fs::CreateDirectories(ct::PathJoin(dir.path, "a/b/c")));
    CT_REQUIRE(ct::fs::WriteAtomic(ct::PathJoin(dir.path, "a/one.txt"), "1"));
    CT_REQUIRE(ct::fs::WriteAtomic(ct::PathJoin(dir.path, "a/b/two.txt"), "22"));
    const std::wstring ro = ct::fs::ToWinPath(ct::PathJoin(dir.path, "a/b/c/ro.txt"));
    CT_REQUIRE(ct::fs::WriteAtomic(ct::PathJoin(dir.path, "a/b/c/ro.txt"), "333"));
    SetFileAttributesW(ro.c_str(), FILE_ATTRIBUTE_READONLY);
    std::vector<ct::fs::Entry> entries;
    CT_REQUIRE(ct::fs::List(ct::PathJoin(dir.path, "a"), entries));
    std::sort(entries.begin(), entries.end(), [](const auto& x, const auto& y) { return x.name < y.name; });
    CT_REQUIRE(entries.size() == 2);
    CT_CHECK(entries[0].name == "b" && entries[0].is_dir);
    CT_CHECK(entries[1].name == "one.txt" && !entries[1].is_dir && entries[1].size == 1);
    CT_CHECK(!ct::fs::List(ct::PathJoin(dir.path, "nope"), entries));
    CT_CHECK(!ct::fs::Delete(ct::PathJoin(dir.path, "a")));   // not empty
    CT_CHECK(ct::fs::DeleteTree(ct::PathJoin(dir.path, "a")));
    CT_CHECK(!ct::fs::Exists(ct::PathJoin(dir.path, "a")));
    CT_CHECK(ct::fs::DeleteTree(ct::PathJoin(dir.path, "a")));   // already gone
}

CT_TEST(fs_relative_absolute_winpath) {
    CT_CHECK_EQ(ct::fs::Absolute("C:/a/./b/../c"), std::string("C:\\a\\c"));
    CT_CHECK_EQ(ct::fs::Relative("C:\\a\\b", "C:\\a\\c\\d.txt"), std::string("..\\c\\d.txt"));
    CT_CHECK_EQ(ct::fs::Relative("C:\\a", "C:\\a"), std::string("."));
    CT_CHECK_EQ(ct::fs::Relative("C:\\a\\", "C:/a/x/y.mp4"), std::string("x\\y.mp4"));
    CT_CHECK_EQ(ct::fs::Relative("C:\\A\\b", "c:\\a\\B\\x"), std::string("x"));   // case-insensitive
    CT_CHECK_EQ(ct::fs::Relative("C:\\a\\b\\c", "C:\\"), std::string("..\\..\\.."));
    CT_CHECK_EQ(ct::fs::Relative("C:\\a", "D:\\x\\y"), std::string("D:\\x\\y"));      // other drive: absolute
    CT_CHECK_EQ(ct::fs::Relative("\\\\srv\\share\\p", "\\\\srv\\share\\q\\f"), std::string("..\\q\\f"));
    CT_CHECK_EQ(ct::fs::Relative("\\\\srv\\share\\p", "\\\\other\\share\\f"), std::string("\\\\other\\share\\f"));
    // Relative paths resolve against the current directory.
    wchar_t cwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    CT_CHECK_EQ(ct::fs::Absolute("x.txt"), ct::PathJoin(ct::WideToUtf8(cwd), "x.txt"));
    CT_CHECK(ct::fs::ToWinPath("C:/short/path") == L"C:\\short\\path");
    const std::string long_unc = "\\\\server\\share\\" + std::string(250, 'u');
    CT_CHECK(ct::fs::ToWinPath(long_unc).rfind(L"\\\\?\\UNC\\server\\share\\", 0) == 0);
    const std::string long_rel = std::string(250, 'r') + "/f.txt";
    const std::wstring lw = ct::fs::ToWinPath(long_rel);
    CT_CHECK(lw.rfind(L"\\\\?\\", 0) == 0 && lw.find(L'/') == std::wstring::npos && lw.find(cwd) == 4);
}

CT_TEST(fs_temp_volume_space) {
    const std::string a = ct::fs::TempPath(".ctproj");
    const std::string b = ct::fs::TempPath("mp4");
    CT_CHECK(a != b);
    CT_CHECK(ct::EndsWith(a, ".ctproj"));
    CT_CHECK(ct::EndsWith(b, ".mp4"));
    CT_CHECK(ct::fs::IsDirectory(std::string(ct::PathParent(a))));
    CT_CHECK(!ct::fs::Exists(a));
    CT_CHECK(ct::fs::FreeSpaceBytes(a) > 0);   // path does not exist yet: uses the nearest existing parent
    CT_CHECK(ct::fs::SameVolume(a, std::string(ct::PathParent(a))));
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::string windows = ct::WideToUtf8(win);
    CT_CHECK(ct::fs::SameVolume(windows, windows.substr(0, 3)));
}
