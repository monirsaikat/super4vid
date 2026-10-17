// ct/core/json.h: RFC 8259 parsing, deterministic dumping, DOM helpers, robustness.
#include <ct/core/json.h>
#include <ct/test/ct_test.h>

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string ReadText(const char* path) {
    std::string out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

// The ```json blocks of docs/04 (tests run with tests/ as the working directory).
std::vector<std::string> DocSamples() {
    std::vector<std::string> samples;
    std::string md = ReadText("../docs/04-file-formats.md");
    if (md.empty()) md = ReadText("docs/04-file-formats.md");   // run from the repository root
    for (char& c : md)
        if (c == '\r') c = ' ';
    size_t pos = 0;
    while ((pos = md.find("```json\n", pos)) != std::string::npos) {
        pos += 8;
        const size_t end = md.find("\n```", pos);
        if (end == std::string::npos) break;
        samples.push_back(md.substr(pos, end - pos));
        pos = end + 4;
    }
    return samples;
}

std::string Nested(int depth) { return std::string(static_cast<size_t>(depth), '[') + std::string(static_cast<size_t>(depth), ']'); }

} // namespace

CT_TEST(json_docs_samples_roundtrip) {
    const std::vector<std::string> samples = DocSamples();
    CT_REQUIRE(samples.size() >= 4);
    for (const std::string& text : samples) {
        ct::Json a;
        std::string err;
        if (!ct::Json::Parse(text, a, &err)) {
            CT_FAIL(err.c_str());
            continue;
        }
        for (int indent : {2, 0, 4}) {
            const std::string d1 = a.Dump(indent);
            ct::Json b;
            CT_REQUIRE(ct::Json::Parse(d1, b));
            const std::string d2 = b.Dump(indent);
            CT_CHECK_EQ(d1, d2);
            CT_CHECK(a == b);
        }
    }
    // Settings sample: spot-check values and insertion order.
    ct::Json s;
    CT_REQUIRE(ct::Json::Parse(samples[2], s));
    CT_CHECK_EQ(s["version"].AsInt(), 1);
    CT_CHECK_EQ(s["hotkeys"]["cancel"].AsString(), std::string("Ctrl+Shift+F9"));
    CT_CHECK(s["recording"]["mic_device"].IsNull());
    CT_CHECK_EQ(s.Items()[0].first, std::string("version"));
    CT_CHECK_EQ(s.Items()[1].first, std::string("recording"));
}

CT_TEST(json_unicode_escapes) {
    ct::Json j;
    CT_REQUIRE(ct::Json::Parse(R"({"a":"\u00e9","e":"\ud83c\udfa5","lone":"x\ud800y","lo":"\udc00","esc":"\"\\\/\b\f\n\r\t"})", j));
    CT_CHECK_EQ(j["a"].AsString(), std::string("\xC3\xA9"));
    CT_CHECK_EQ(j["e"].AsString(), std::string("\xF0\x9F\x8E\xA5"));
    CT_CHECK_EQ(j["lone"].AsString(), std::string("x\xEF\xBF\xBDy"));
    CT_CHECK_EQ(j["lo"].AsString(), std::string("\xEF\xBF\xBD"));
    CT_CHECK_EQ(j["esc"].AsString(), std::string("\"\\/\b\f\n\r\t"));
    // Dump: control characters escaped, non-ASCII raw UTF-8, '/' unescaped.
    ct::Json o = ct::Json::Object();
    o.Set("s", std::string("\x01" "\xC3\xA9/\"\\\n", 7));
    CT_CHECK_EQ(o.Dump(0), std::string("{\"s\":\"\\u0001\xC3\xA9/\\\"\\\\\\n\"}"));
    // Raw UTF-8 in the input passes through.
    CT_REQUIRE(ct::Json::Parse("[\"\xE6\x97\xA5\"]", j));
    CT_CHECK_EQ(j[size_t(0)].AsString(), std::string("\xE6\x97\xA5"));
    CT_CHECK(!ct::Json::Parse("[\"a\x01\"]", j));   // raw control char
    CT_CHECK(!ct::Json::Parse(R"(["\x"])", j));
    CT_CHECK(!ct::Json::Parse(R"(["\u12"])", j));
}

CT_TEST(json_numbers_exact) {
    ct::Json j;
    CT_REQUIRE(ct::Json::Parse("[1e308, -0, 9007199254740993, -9223372036854775808, 9223372036854775808, 0.1, 1.5e-7, 42, -17, 3.0, 1E2, -0.0]", j));
    CT_CHECK(!j[size_t(0)].IsInteger());
    CT_CHECK_EQ(j[size_t(0)].AsNumber(), 1e308);
    CT_CHECK(std::signbit(j[size_t(1)].AsNumber()));
    CT_CHECK(j[size_t(2)].IsInteger());
    CT_CHECK_EQ(j[size_t(2)].AsInt64(), int64_t(9007199254740993));
    CT_CHECK_EQ(j[size_t(3)].AsInt64(), INT64_MIN);
    CT_CHECK(!j[size_t(4)].IsInteger());   // > INT64_MAX: double
    CT_CHECK_EQ(j[size_t(7)].AsInt(), 42);
    CT_CHECK_EQ(j[size_t(8)].AsInt64(), int64_t(-17));
    CT_CHECK_EQ(j.Dump(0), std::string("[1e+308,-0,9007199254740993,-9223372036854775808,9.223372036854776e+18,0.1,1.5e-07,42,-17,3,100,-0]"));
    ct::Json back;
    CT_REQUIRE(ct::Json::Parse(j.Dump(0), back));
    CT_CHECK(std::signbit(back[size_t(1)].AsNumber()));
    CT_CHECK_EQ(back[size_t(2)].AsInt64(), int64_t(9007199254740993));
    CT_CHECK_EQ(back[size_t(6)].AsNumber(), 1.5e-7);
    // Every double survives Dump -> Parse bit-exactly.
    const double tricky[] = {0.1 + 0.2, 1.0 / 3.0, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 123456.789e-30};
    for (double d : tricky) {
        ct::Json v(d), r;
        CT_REQUIRE(ct::Json::Parse(v.Dump(), r));
        CT_CHECK_EQ(r.AsNumber(), d);
    }
    CT_CHECK_EQ(ct::Json(std::nan("")).Dump(), std::string("null"));
    CT_CHECK_EQ(ct::Json(uint64_t(18446744073709551615ull)).Dump(), std::string("1.8446744073709552e+19"));
    CT_CHECK_EQ(ct::Json(uint32_t(4000000000u)).Dump(), std::string("4000000000"));
    for (const char* bad : {"01", "1.", ".5", "-", "1e", "1e+", "+1", "0x10", "--1", "1.e5", "Infinity", "NaN"})
        CT_CHECK(!ct::Json::Parse(bad, j));
}

CT_TEST(json_locale_independent) {
    const char* old = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = old ? old : "C";
    const bool switched = std::setlocale(LC_NUMERIC, "de-DE") != nullptr;   // decimal comma
    ct::Json j;
    const bool ok = ct::Json::Parse("[1.5, 2.25e3]", j);
    const std::string dumped = j.Dump(0);
    std::setlocale(LC_NUMERIC, saved.c_str());
    if (!switched) CT_SKIP("de-DE locale not available");
    CT_CHECK(ok);
    CT_CHECK_EQ(j[size_t(0)].AsNumber(), 1.5);
    CT_CHECK_EQ(dumped, std::string("[1.5,2250]"));
}

CT_TEST(json_depth_limit) {
    ct::Json j;
    std::string err;
    CT_CHECK(ct::Json::Parse(Nested(255), j));
    CT_CHECK(ct::Json::Parse(Nested(256), j));
    CT_CHECK(!ct::Json::Parse(Nested(257), j, &err));
    CT_CHECK(err.find("nesting") != std::string::npos);
    std::string objs;
    for (int i = 0; i < 300; ++i) objs += "{\"a\":";
    objs += "1";
    for (int i = 0; i < 300; ++i) objs += "}";
    CT_CHECK(!ct::Json::Parse(objs, j));
}

CT_TEST(json_errors_and_syntax) {
    ct::Json j;
    std::string err;
    CT_CHECK(!ct::Json::Parse("{\n  \"a\": 1,\n  \"b\": 2 \"c\": 3\n}", j, &err));
    CT_CHECK_EQ(err, std::string("line 3 col 10: expected ',' or '}'"));
    CT_CHECK(j.IsNull());
    CT_CHECK(!ct::Json::Parse("[1,2,]", j, &err));
    CT_CHECK(err.find("trailing comma") != std::string::npos);
    CT_CHECK(!ct::Json::Parse("{\"a\":1,}", j));
    CT_CHECK(!ct::Json::Parse("", j, &err));
    CT_CHECK(!ct::Json::Parse("   ", j));
    CT_CHECK(!ct::Json::Parse("[1] x", j));
    CT_CHECK(!ct::Json::Parse("nul", j));
    CT_CHECK(!ct::Json::Parse("{a:1}", j));
    CT_CHECK(!ct::Json::Parse("[1 2]", j));
    CT_CHECK(!ct::Json::Parse("\"abc", j));
    CT_CHECK(!ct::Json::Parse("{\"a\" 1}", j));
    CT_CHECK(ct::Json::Parse("\xEF\xBB\xBF {\"bom\": true} \r\n", j));   // BOM + whitespace
    CT_CHECK(j["bom"].AsBool());
    CT_CHECK(ct::Json::Parse("\"just a string\"", j) && j.IsString());
    CT_CHECK(ct::Json::Parse("null", j) && j.IsNull());
    // A view into a larger buffer: the number must not read past the view.
    const char buf[] = "12345";
    CT_CHECK(ct::Json::Parse(std::string_view(buf, 2), j) && j.AsInt() == 12);
    const char buf2[] = "1.5999";
    CT_CHECK(ct::Json::Parse(std::string_view(buf2, 3), j) && j.AsNumber() == 1.5);
}

CT_TEST(json_dom_api) {
    ct::Json root;
    CT_CHECK(root.IsNull());
    root["name"] = "clip";
    root["count"] = 3;
    root["ratio"] = 0.5;
    root["on"] = true;
    root["big"] = int64_t(1) << 53;
    root["size"] = size_t(7);
    root["nested"]["deep"]["x"] = 1;   // creates intermediate objects
    root.Set("count", 4);              // replace keeps position
    ct::Json& arr = root.Set("list", ct::Json::Array());
    arr.Push(1);
    arr.Push("two");
    arr.Push(ct::Json());
    CT_CHECK_EQ(root.Size(), size_t(8));
    CT_CHECK_EQ(root.Items()[1].first, std::string("count"));
    CT_CHECK_EQ(root["count"].AsInt(), 4);
    CT_CHECK(root.Has("nested") && !root.Has("missing"));
    const ct::Json& croot = root;
    CT_CHECK(croot["missing"]["deeper"].IsNull());   // static Null, no insertion
    CT_CHECK(!root.Has("missing"));
    CT_CHECK(croot["list"][size_t(5)].IsNull());
    CT_CHECK_EQ(croot["list"].Elements().size(), size_t(3));
    CT_CHECK_EQ(root.Get<int>("count", 0), 4);
    CT_CHECK_EQ(root.Get<double>("ratio", 0.0), 0.5);
    CT_CHECK_EQ(root.Get<bool>("on", false), true);
    CT_CHECK_EQ(root.Get<std::string>("name", "x"), std::string("clip"));
    CT_CHECK_EQ(root.Get<std::string>("count", "fallback"), std::string("fallback"));   // wrong type
    CT_CHECK_EQ(root.Get<int>("missing", -1), -1);
    CT_CHECK_EQ(root.Get<uint8_t>("big", 0), uint8_t(255));   // clamped
    CT_CHECK_EQ(root.Get<int64_t>("big", 0), int64_t(1) << 53);
    CT_CHECK_EQ(croot["name"].AsString("d"), std::string("clip"));
    CT_CHECK_EQ(croot["ratio"].AsString("d"), std::string("d"));
    CT_CHECK(croot["ratio"].AsString().empty());
    CT_CHECK_EQ(croot["ratio"].AsInt64(), int64_t(0));
    CT_CHECK(root.Erase("ratio") && !root.Erase("ratio"));
    CT_CHECK_EQ(ct::Json::Object().Dump(), std::string("{}"));
    CT_CHECK_EQ(ct::Json::Array().Dump(), std::string("[]"));
    ct::Json small = ct::Json::Object();
    small["a"] = ct::Json::Array();
    small["a"].Push(1);
    small["b"] = ct::Json::Object();
    CT_CHECK_EQ(small.Dump(2), std::string("{\n  \"a\": [\n    1\n  ],\n  \"b\": {}\n}"));
    CT_CHECK_EQ(small.Dump(0), std::string("{\"a\":[1],\"b\":{}}"));
    ct::Json copy = small;
    CT_CHECK(copy == small);
    copy["a"][size_t(0)] = 2;
    CT_CHECK(copy != small);
    CT_CHECK(ct::Json(1) == ct::Json(1.0));
}

CT_TEST(json_fuzz_mutations_never_crash) {
    const std::vector<std::string> samples = DocSamples();
    CT_REQUIRE(!samples.empty());
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next = [&rng] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    static const char kTokens[] = "{}[]\",:\\0123456789eE+-.tfnu \n\x01\xC3\xFF";
    int parsed = 0;
    bool roundtrip_ok = true;
    for (int i = 0; i < 1000; ++i) {
        std::string doc = samples[next() % samples.size()];
        const int edits = 1 + static_cast<int>(next() % 8);
        for (int e = 0; e < edits && !doc.empty(); ++e) {
            const size_t at = next() % doc.size();
            switch (next() % 4) {
            case 0: doc[at] = kTokens[next() % (sizeof(kTokens) - 1)]; break;
            case 1: doc.erase(at, 1 + next() % 16); break;
            case 2: doc.insert(at, 1, kTokens[next() % (sizeof(kTokens) - 1)]); break;
            default: doc.resize(at); break;
            }
        }
        ct::Json j;
        std::string err;
        if (ct::Json::Parse(doc, j, &err)) {
            ++parsed;
            ct::Json again;
            if (!ct::Json::Parse(j.Dump(), again) || again.Dump() != j.Dump()) roundtrip_ok = false;
        } else if (err.rfind("line ", 0) != 0) {
            roundtrip_ok = false;
        }
    }
    CT_CHECK(roundtrip_ok);
    if (ct::test::Verbose()) std::printf("    %d of 1000 mutated documents still parsed\n", parsed);
}
