// Benchmarks (run with --bench, Release): SPSC ping, JSON parse of a 1 MB document, Format.
#include <ct/core/json.h>
#include <ct/core/spsc_queue.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <cstdio>
#include <string>

namespace {

// ~1 MB of project-like JSON: clips with nested objects, arrays, strings with escapes, ints and doubles.
const std::string& OneMegabyteDocument() {
    static std::string doc;
    if (!doc.empty()) return doc;
    ct::Json root = ct::Json::Object();
    root["format"] = "ctproj";
    ct::Json& clips = root.Set("clips", ct::Json::Array());
    for (int i = 0; doc.size() < (1u << 20); ++i) {
        ct::Json clip = ct::Json::Object();
        clip["id"] = ct::Format("c%d", i);
        clip["track"] = "t1";
        clip["start_us"] = int64_t(i) * 16667;
        clip["speed"] = 1.0 + i * 0.001;
        clip["name"] = "Clip \"quoted\" \xC3\xA9\\path";
        ct::Json& tr = clip["transform"];
        tr["x"] = 0.25 * i;
        tr["y"] = -0.5;
        tr["scale"] = 1.5;
        tr["opacity"] = 1.0;
        ct::Json& keys = clip.Set("keyframes", ct::Json::Array());
        for (int k = 0; k < 4; ++k) {
            ct::Json kf = ct::Json::Object();
            kf["t_us"] = int64_t(k) * 100000;
            kf["v"] = k * 0.333;
            kf["ease"] = "inout";
            keys.Push(std::move(kf));
        }
        clips.Push(std::move(clip));
        if (i % 256 == 255) doc = root.Dump(2);
    }
    return doc;
}

} // namespace

CT_BENCH(bench_spsc_push_pop_ping, 50'000'000) {
    static ct::SpscQueue<int64_t, 1024> q;
    int64_t sum = 0, v = 0;
    ct::Stopwatch sw;
    sw.Start();
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        q.Push(i);
        q.Pop(v);
        sum += v;
    }
    const double ms = sw.ElapsedMs();
    const double mops = static_cast<double>(CT_BENCH_N) / ms / 1000.0;
    std::printf("    SPSC push+pop pairs: %.1f M/s (budget >= 20 M/s)\n", mops);
    CT_CHECK_EQ(sum, CT_BENCH_N * (CT_BENCH_N - 1) / 2);
    CT_CHECK(mops >= 20.0);
}

CT_BENCH(bench_json_parse_1mb, 20) {
    const std::string& doc = OneMegabyteDocument();
    double best_ms = 1e9, total_ms = 0;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        ct::Json j;
        ct::Stopwatch sw;
        sw.Start();
        const bool ok = ct::Json::Parse(doc, j);
        const double ms = sw.ElapsedMs();
        CT_CHECK(ok);
        best_ms = ms < best_ms ? ms : best_ms;
        total_ms += ms;
    }
    const double avg_ms = total_ms / static_cast<double>(CT_BENCH_N);
    std::printf("    JSON parse %.2f MB: avg %.2f ms, best %.2f ms (budget < 20 ms), %.0f MB/s\n",
                static_cast<double>(doc.size()) / (1 << 20), avg_ms, best_ms,
                static_cast<double>(doc.size()) / (1 << 20) / (avg_ms / 1000.0));
    CT_CHECK(avg_ms < 20.0);
    ct::Json j;
    CT_REQUIRE(ct::Json::Parse(doc, j));
    ct::Stopwatch sw;
    sw.Start();
    const std::string dumped = j.Dump(2);
    std::printf("    JSON dump: %.2f ms, byte-identical: %s\n", sw.ElapsedMs(), dumped == doc ? "yes" : "no");
    CT_CHECK(dumped == doc);
}

CT_BENCH(bench_format_1m_calls, 1'000'000) {
    size_t total = 0;
    ct::Stopwatch sw;
    sw.Start();
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        const std::string s = ct::Format("frame %lld at %lld us: %s %.3f", static_cast<long long>(i),
                                         static_cast<long long>(i * 16667), "capture", 0.42);
        total += s.size();
    }
    std::printf("    Format: %.0f ns/call\n", sw.ElapsedMs() * 1e6 / static_cast<double>(CT_BENCH_N));
    CT_CHECK(total > 0);
}
