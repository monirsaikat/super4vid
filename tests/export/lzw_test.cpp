// GifEncoder::LzwEncode against the independent decoder in tests/common/gif_reader.h.
#include "gif_reader.h"

#include "../../src/export/src/gif_internal.h"

#include <ct/core/time.h>
#include <ct/export/gif_encoder.h>
#include <ct/test/ct_test.h>

#include <cstdio>
#include <vector>

using namespace ct;
using namespace ct::exportng;

namespace {

struct Rng {
    uint64_t s;
    uint64_t Next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int Int(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint64_t>(hi - lo + 1)); }
};

enum class Pattern { Uniform, Runs, Repeating, Constant };

std::vector<uint8_t> MakeStream(Rng& r, size_t n, int mcs, Pattern pat) {
    const int colors = 1 << mcs;
    std::vector<uint8_t> v(n);
    int run_left = 0, run_value = 0;
    for (size_t i = 0; i < n; ++i) {
        switch (pat) {
        case Pattern::Uniform: v[i] = static_cast<uint8_t>(r.Int(0, colors - 1)); break;
        case Pattern::Runs:
            if (run_left-- <= 0) {
                run_left = r.Int(0, 40);
                run_value = r.Int(0, colors - 1);
            }
            v[i] = static_cast<uint8_t>(run_value);
            break;
        case Pattern::Repeating: v[i] = static_cast<uint8_t>((i * 7 + i / 13) % static_cast<size_t>(colors)); break;
        case Pattern::Constant: v[i] = static_cast<uint8_t>(colors - 1); break;
        }
    }
    return v;
}

// Every sub-block is 1..255 bytes and the chain ends with exactly one 0 block at the end of `data`.
bool ValidSubBlocks(const std::vector<uint8_t>& data) {
    size_t p = 0;
    while (p < data.size()) {
        const size_t len = data[p++];
        if (len == 0) return p == data.size();
        p += len;
    }
    return false;
}

} // namespace

CT_TEST(lzw_round_trip_random_streams) {
    Rng r{7};
    const size_t sizes[] = {0, 1, 2, 3, 17, 255, 256, 4096, 70000};
    const Pattern patterns[] = {Pattern::Uniform, Pattern::Runs, Pattern::Repeating, Pattern::Constant};
    int cases = 0;
    for (int mcs = 2; mcs <= 8; ++mcs)
        for (size_t n : sizes)
            for (Pattern pat : patterns) {
                const std::vector<uint8_t> in = MakeStream(r, n, mcs, pat);
                std::vector<uint8_t> enc, dec;
                GifEncoder::LzwEncode(in.data(), in.size(), mcs, enc);
                CT_CHECK(ValidSubBlocks(enc));
                ct::test::gif::LzwStats st;
                CT_CHECK(ct::test::gif::LzwDecode(enc.data(), enc.size(), mcs, dec, &st));
                CT_CHECK(st.saw_eoi);
                CT_CHECK_EQ(st.consumed, enc.size());
                CT_CHECK(st.clear_codes >= 1);
                CT_CHECK(dec == in);
                if (dec != in) {
                    std::printf("  mismatch: mcs %d n %zu pattern %d\n", mcs, n, static_cast<int>(pat));
                    return;
                }
                ++cases;
            }
    CT_CHECK_EQ(cases, 7 * 9 * 4);
}

CT_TEST(lzw_code_size_growth_and_clear_codes) {
    Rng r{99};
    // Uniform random 8-bit data fills the 4096-entry table every ~3,800 codes: many clear codes, 12-bit codes.
    {
        const std::vector<uint8_t> in = MakeStream(r, 300000, 8, Pattern::Uniform);
        std::vector<uint8_t> enc, dec;
        GifEncoder::LzwEncode(in.data(), in.size(), 8, enc);
        ct::test::gif::LzwStats st;
        CT_REQUIRE(ct::test::gif::LzwDecode(enc.data(), enc.size(), 8, dec, &st));
        CT_CHECK(dec == in);
        CT_CHECK_EQ(st.max_code_size, 12);
        CT_CHECK(st.clear_codes > 20);
    }
    // 2-bit data (min code size 2): codes grow 3 -> 12 bits.
    {
        const std::vector<uint8_t> in = MakeStream(r, 200000, 2, Pattern::Uniform);
        std::vector<uint8_t> enc, dec;
        GifEncoder::LzwEncode(in.data(), in.size(), 2, enc);
        ct::test::gif::LzwStats st;
        CT_REQUIRE(ct::test::gif::LzwDecode(enc.data(), enc.size(), 2, dec, &st));
        CT_CHECK(dec == in);
        CT_CHECK_EQ(st.max_code_size, 12);
        CT_CHECK(st.clear_codes > 1);
    }
    // Every length around each code-size boundary (the last code before EOI is where off-by-one bugs live).
    for (int mcs : {2, 3, 8}) {
        for (size_t n = 1; n < 1200; ++n) {
            const std::vector<uint8_t> in = MakeStream(r, n, mcs, n % 2 ? Pattern::Uniform : Pattern::Repeating);
            std::vector<uint8_t> enc, dec;
            GifEncoder::LzwEncode(in.data(), in.size(), mcs, enc);
            if (!ct::test::gif::LzwDecode(enc.data(), enc.size(), mcs, dec) || dec != in) {
                CT_FAIL("round trip failed");
                std::printf("  mcs %d n %zu\n", mcs, n);
                return;
            }
        }
    }
    // A constant stream compresses to almost nothing.
    {
        const std::vector<uint8_t> in(1 << 20, 3);
        std::vector<uint8_t> enc;
        GifEncoder::LzwEncode(in.data(), in.size(), 2, enc);
        CT_CHECK(enc.size() < 4000);
    }
}

CT_TEST(lzw_masks_out_of_range_indices) {
    // Indices >= 1 << min_code_size are a caller error; they must not corrupt the stream.
    const uint8_t in[] = {0, 1, 2, 3, 200, 7};
    std::vector<uint8_t> enc, dec;
    GifEncoder::LzwEncode(in, sizeof(in), 2, enc);
    CT_REQUIRE(ct::test::gif::LzwDecode(enc.data(), enc.size(), 2, dec));
    CT_CHECK_EQ(dec.size(), sizeof(in));
}

// Perf follow-up (GIF 31 ms/frame): segmented LZW decodes with the same independent decoder, for any segment split,
// and its bytes do not depend on the number of worker threads.
CT_TEST(lzw_segmented_round_trip_and_thread_independent) {
    ct::exportng::gif_detail::Workers four;
    four.Start(4);
    Rng r{7};
    const size_t sizes[] = {1, 1023, 1024, 1025, 5000, 70000, 300001};
    for (size_t n : sizes) {
        for (int mcs : {2, 5, 8}) {
            for (Pattern pat : {Pattern::Uniform, Pattern::Runs, Pattern::Constant}) {
                const std::vector<uint8_t> in = MakeStream(r, n, mcs, pat);
                std::vector<uint8_t> a, b, dec;
                ct::exportng::gif_detail::LzwEncodeSegmented(in.data(), in.size(), mcs, a, 1024, nullptr);
                ct::exportng::gif_detail::LzwEncodeSegmented(in.data(), in.size(), mcs, b, 1024, &four);
                CT_CHECK(a == b);
                ct::test::gif::LzwStats st;
                CT_CHECK(ct::test::gif::LzwDecode(a.data(), a.size(), mcs, dec, &st));
                CT_CHECK(dec == in);
                CT_CHECK_EQ(st.consumed, a.size());
                CT_CHECK(ValidSubBlocks(a));
            }
        }
    }
    // A 720p frame with the encoder's segment size costs little compression.
    const std::vector<uint8_t> in = MakeStream(r, 1280 * 720, 8, Pattern::Runs);
    std::vector<uint8_t> whole, seg, dec;
    GifEncoder::LzwEncode(in.data(), in.size(), 8, whole);
    ct::exportng::gif_detail::LzwEncodeSegmented(in.data(), in.size(), 8, seg, ct::exportng::gif_detail::kLzwSegment, &four);
    CT_CHECK(ct::test::gif::LzwDecode(seg.data(), seg.size(), 8, dec));
    CT_CHECK(dec == in);
    std::printf("    720p runs: whole %zu bytes, %zu segments %zu bytes (%+.2f %%)\n", whole.size(),
                (in.size() + ct::exportng::gif_detail::kLzwSegment - 1) / ct::exportng::gif_detail::kLzwSegment, seg.size(),
                100.0 * (static_cast<double>(seg.size()) / static_cast<double>(whole.size()) - 1.0));
    CT_CHECK(seg.size() < whole.size() * 102 / 100);
}

CT_BENCH(lzw_encode_720p_frame_segmented_parallel, 20) {
    ct::exportng::gif_detail::Workers w;
    w.Start(ct::exportng::gif_detail::Workers::DefaultThreads());
    Rng r{1};
    std::vector<uint8_t> in = MakeStream(r, 1280 * 720, 8, Pattern::Runs);
    std::vector<uint8_t> enc;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        enc.clear();
        ct::exportng::gif_detail::LzwEncodeSegmented(in.data(), in.size(), 8, enc, ct::exportng::gif_detail::kLzwSegment, &w);
    }
}

CT_BENCH(lzw_encode_720p_frame, 20) {
    Rng r{1};
    std::vector<uint8_t> in = MakeStream(r, 1280 * 720, 8, Pattern::Runs);
    std::vector<uint8_t> enc;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        enc.clear();
        GifEncoder::LzwEncode(in.data(), in.size(), 8, enc);
    }
}
