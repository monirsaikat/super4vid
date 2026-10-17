// ct/core/strings.h: UTF conversion, formatting, split/trim, path helpers, UUIDs, timecodes, hashes.
#include <ct/core/strings.h>
#include <ct/test/ct_test.h>

#include <string>
#include <unordered_set>

CT_TEST(strings_utf_roundtrip_emoji) {
    const std::string utf8 = "caf\xC3\xA9 \xF0\x9F\x8E\xA5 \xE6\x97\xA5\xE6\x9C\xAC";   // "café 🎥 日本"
    const std::wstring wide = ct::Utf8ToWide(utf8);
    CT_CHECK_EQ(wide.size(), size_t(10));   // c a f é space (2 units for the emoji) space 日 本
    CT_CHECK(wide[3] == 0x00E9);
    CT_CHECK(wide[5] == 0xD83C && wide[6] == 0xDFA5);
    CT_CHECK_EQ(ct::WideToUtf8(wide), utf8);
}

CT_TEST(strings_utf_invalid_and_empty) {
    CT_CHECK(ct::Utf8ToWide("").empty());
    CT_CHECK(ct::WideToUtf8(L"").empty());
    // Lone continuation byte, truncated 3-byte sequence, overlong encoding.
    const std::wstring w = ct::Utf8ToWide(std::string("a\x80" "b\xE6\x97" "c\xC0\xAF", 8));
    CT_CHECK(w.find(L'a') == 0);
    CT_CHECK(w.find(wchar_t(0xFFFD)) != std::wstring::npos);
    CT_CHECK(w.find(L'b') != std::wstring::npos && w.find(L'c') != std::wstring::npos);
    // Unpaired surrogate -> U+FFFD (EF BF BD).
    const wchar_t lone[] = {L'x', wchar_t(0xD800), L'y', 0};
    CT_CHECK_EQ(ct::WideToUtf8(lone), std::string("x\xEF\xBF\xBDy"));
    // Embedded NUL is preserved (lengths, not terminators).
    CT_CHECK_EQ(ct::Utf8ToWide(std::string("a\0b", 3)).size(), size_t(3));
}

CT_TEST(strings_format_large) {
    const std::string big(5000, 'x');
    const std::string s = ct::Format("[%s] %d %.2f", big.c_str(), 42, 1.5);
    CT_CHECK_EQ(s.size(), big.size() + 2 + 3 + 5);
    CT_CHECK(ct::StartsWith(s, "[xxx"));
    CT_CHECK(ct::EndsWith(s, "] 42 1.50"));
    std::string out = "a";
    ct::AppendFormat(out, "%s-%ls", big.c_str(), L"w");
    CT_CHECK_EQ(out.size(), 1 + big.size() + 2);
    CT_CHECK(ct::Format("%s", "").empty());
    CT_CHECK_EQ(ct::Format("%5.1f|%-3d|", 3.14159, 7), std::string("  3.1|7  |"));
}

CT_TEST(strings_split_edges) {
    using V = std::vector<std::string_view>;
    auto eq = [](const V& a, const V& b) { return a == b; };
    CT_CHECK(eq(ct::Split("a,b,c", ','), V{"a", "b", "c"}));
    CT_CHECK(eq(ct::Split("a,,b,", ','), V{"a", "b"}));
    CT_CHECK(eq(ct::Split("a,,b,", ',', true), V{"a", "", "b", ""}));
    CT_CHECK(eq(ct::Split(",", ',', true), V{"", ""}));
    CT_CHECK(ct::Split("", ',').empty());
    CT_CHECK(eq(ct::Split("", ',', true), V{""}));
    CT_CHECK(eq(ct::Split("abc", ','), V{"abc"}));
    CT_CHECK(ct::Split(",,,", ',').empty());
    CT_CHECK_EQ(ct::Trim("  \t a b \r\n"), std::string_view("a b"));
    CT_CHECK(ct::Trim(" \t\n").empty());
    CT_CHECK(ct::IEquals("Ctrl", "cTRL"));
    CT_CHECK(!ct::IEquals("Ctrl", "Ctr"));
    CT_CHECK_EQ(ct::ToLowerAscii("AbC-\xC3\x89"), std::string("abc-\xC3\x89"));   // non-ASCII untouched
    CT_CHECK(ct::StartsWith("abc", ""));
    CT_CHECK(!ct::EndsWith("c", "abc"));
}

CT_TEST(strings_path_helpers) {
    CT_CHECK_EQ(ct::PathJoin("C:\\a\\", "/b/c.txt"), std::string("C:\\a\\b/c.txt"));
    CT_CHECK_EQ(ct::PathJoin("C:/a", "b"), std::string("C:/a\\b"));
    CT_CHECK_EQ(ct::PathJoin("C:\\", "b"), std::string("C:\\b"));
    CT_CHECK_EQ(ct::PathJoin("", "b"), std::string("b"));
    CT_CHECK_EQ(ct::PathJoin("a", ""), std::string("a"));
    CT_CHECK_EQ(ct::PathJoin("\\", "b"), std::string("\\b"));

    CT_CHECK_EQ(ct::PathName("C:\\dir/sub\\file.tar.gz"), std::string_view("file.tar.gz"));
    CT_CHECK_EQ(ct::PathExt("C:\\dir/sub\\file.tar.gz"), std::string_view(".gz"));
    CT_CHECK_EQ(ct::PathStem("C:\\dir/sub\\file.tar.gz"), std::string_view("file.tar"));
    CT_CHECK_EQ(ct::PathParent("C:\\dir/sub\\file.tar.gz"), std::string_view("C:\\dir/sub"));
    CT_CHECK_EQ(ct::PathParent("C:\\file"), std::string_view("C:\\"));
    CT_CHECK_EQ(ct::PathParent("C:file"), std::string_view("C:"));
    CT_CHECK_EQ(ct::PathParent("/file"), std::string_view("/"));
    CT_CHECK_EQ(ct::PathParent("a\\\\b"), std::string_view("a"));
    CT_CHECK(ct::PathParent("file").empty());
    CT_CHECK(ct::PathName("C:\\dir\\").empty());
    CT_CHECK_EQ(ct::PathName("C:x.txt"), std::string_view("x.txt"));
    CT_CHECK(ct::PathExt("C:\\dir.d\\.gitignore").empty());
    CT_CHECK_EQ(ct::PathStem(".gitignore"), std::string_view(".gitignore"));
    CT_CHECK(ct::PathExt("..").empty());
    CT_CHECK(ct::PathExt("noext").empty());
    CT_CHECK_EQ(ct::PathExt("a.MP4"), std::string_view(".MP4"));
}

CT_TEST(strings_uuid4_format_unique) {
    std::unordered_set<std::string> seen;
    for (int i = 0; i < 10000; ++i) {
        const std::string u = ct::Uuid4();
        if (u.size() != 36 || u[8] != '-' || u[13] != '-' || u[18] != '-' || u[23] != '-' || u[14] != '4' ||
            !(u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b')) {
            CT_FAIL(("bad uuid " + u).c_str());
            return;
        }
        for (char c : u)
            if (!(c == '-' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                CT_FAIL(("bad uuid char " + u).c_str());
                return;
            }
        seen.insert(u);
    }
    CT_CHECK_EQ(seen.size(), size_t(10000));
}

CT_TEST(strings_timecode_2997) {
    // 29.97 fps: frame n starts at floor(n * 1001000000 / 30000) us.
    CT_CHECK_EQ(ct::FormatTimecode(0, 30000, 1001, true), std::string("00:00:00.00"));
    CT_CHECK_EQ(ct::FormatTimecode(967633, 30000, 1001, true), std::string("00:00:00.29"));    // frame 29 start
    CT_CHECK_EQ(ct::FormatTimecode(999999, 30000, 1001, true), std::string("00:00:00.29"));    // still frame 29
    CT_CHECK_EQ(ct::FormatTimecode(1001000, 30000, 1001, true), std::string("00:00:01.00"));   // frame 30
    // Frame field stays within 0..29 and is monotonic within each second over 10 minutes.
    int64_t prev_s = -1, prev_f = -1;
    bool ok = true;
    for (int64_t n = 0; n < 30000 * 10 * 60 / 1001; ++n) {
        const int64_t t = n * 1001000000LL / 30000;
        const std::string tc = ct::FormatTimecode(t, 30000, 1001, true);
        const int64_t s = std::stoll(tc.substr(0, 2)) * 3600 + std::stoll(tc.substr(3, 2)) * 60 + std::stoll(tc.substr(6, 2));
        const int64_t f = std::stoll(tc.substr(9));
        if (f > 29 || (s == prev_s && f != prev_f + 1) || (s != prev_s && (s != prev_s + 1 || f != 0))) {
            if (ok) std::printf("n=%lld %s\n", static_cast<long long>(n), tc.c_str());
            ok = false;
        }
        prev_s = s;
        prev_f = f;
    }
    CT_CHECK(ok);
    CT_CHECK_EQ(ct::FormatTimecode(3723123456, 30, 1, false), std::string("01:02:03.123"));
    CT_CHECK_EQ(ct::FormatTimecode(3723400000, 30, 1, true), std::string("01:02:03.12"));
    CT_CHECK_EQ(ct::FormatTimecode(-1500000, 30, 1, false), std::string("-00:00:01.500"));
    CT_CHECK_EQ(ct::FormatTimecode(100LL * 3600 * 1000000, 60, 1, false), std::string("100:00:00.000"));
}

CT_TEST(strings_bytes_and_hashes) {
    CT_CHECK_EQ(ct::FormatBytes(0), std::string("0 B"));
    CT_CHECK_EQ(ct::FormatBytes(1023), std::string("1023 B"));
    CT_CHECK_EQ(ct::FormatBytes(1536), std::string("1.5 KB"));
    CT_CHECK_EQ(ct::FormatBytes(1572864), std::string("1.5 MB"));
    CT_CHECK_EQ(ct::FormatBytes(5ull << 30), std::string("5.0 GB"));
    // Reference values from the FNV spec.
    CT_CHECK_EQ(ct::Fnv1a32("", 0), 0x811C9DC5u);
    CT_CHECK_EQ(ct::Fnv1a32("a", 1), 0xE40C292Cu);
    CT_CHECK_EQ(ct::Fnv1a64("", 0), 0xCBF29CE484222325ull);
    CT_CHECK_EQ(ct::Fnv1a64("a", 1), 0xAF63DC4C8601EC8Cull);
    CT_CHECK_EQ(ct::Fnv1a32("foobar", 6), 0xBF9CF968u);
}
