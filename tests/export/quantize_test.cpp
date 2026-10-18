// BuildPalette (median cut) and Quantize (nearest cache, Floyd-Steinberg).
#include <ct/export/gif_encoder.h>
#include <ct/test/ct_test.h>

#include "../../src/export/src/gif_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ct::exportng;

namespace {

struct Image {
    int w, h;
    std::vector<uint8_t> px;   // BGRA
    Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 4, 255) {}
    void Set(int x, int y, uint32_t rgb) {
        uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        p[0] = static_cast<uint8_t>(rgb);
        p[1] = static_cast<uint8_t>(rgb >> 8);
        p[2] = static_cast<uint8_t>(rgb >> 16);
        p[3] = 255;
    }
    uint32_t Get(int x, int y) const {
        const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        return (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
    }
    int Stride() const { return w * 4; }
};

int Channel(uint32_t c, int ch) { return static_cast<int>((c >> (16 - 8 * ch)) & 255); }

// Mean absolute error of 8x8 block averages (what the eye sees from a distance), averaged over channels.
double BlockError(const Image& img, const std::vector<uint8_t>& idx, const std::vector<uint32_t>& pal) {
    double total = 0;
    int blocks = 0;
    for (int by = 0; by + 8 <= img.h; by += 8)
        for (int bx = 0; bx + 8 <= img.w; bx += 8) {
            for (int ch = 0; ch < 3; ++ch) {
                double src = 0, out = 0;
                for (int y = by; y < by + 8; ++y)
                    for (int x = bx; x < bx + 8; ++x) {
                        src += Channel(img.Get(x, y), ch);
                        out += Channel(pal[idx[static_cast<size_t>(y) * img.w + x]], ch);
                    }
                total += std::abs(src - out) / 64.0;
            }
            ++blocks;
        }
    return total / (blocks * 3);
}

Image Gradient(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t r = static_cast<uint32_t>(x * 255 / (w - 1));
            const uint32_t g = static_cast<uint32_t>(y * 255 / (h - 1));
            const uint32_t b = static_cast<uint32_t>((x + y) * 255 / (w + h - 2));
            img.Set(x, y, (r << 16) | (g << 8) | b);
        }
    return img;
}

} // namespace

CT_TEST(palette_four_colors_exact) {
    const uint32_t colors[4] = {0x102030, 0xF0E0D0, 0x7F007F, 0x00C864};
    Image img(37, 23);   // odd sizes, uneven counts
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) img.Set(x, y, colors[(x * x + y * 3) % 4]);
    for (int max_colors : {4, 16, 256}) {
        std::vector<uint32_t> pal;
        GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), max_colors, pal);
        CT_REQUIRE(pal.size() == 4);
        std::vector<uint32_t> got = pal, want(colors, colors + 4);
        std::sort(got.begin(), got.end());
        std::sort(want.begin(), want.end());
        CT_CHECK(got == want);
        // Quantizing maps every pixel back to its own color, with and without dither.
        for (bool dither : {false, true}) {
            std::vector<uint8_t> idx;
            GifEncoder::Quantize(img.px.data(), img.w, img.h, img.Stride(), pal, dither, idx);
            CT_REQUIRE(idx.size() == static_cast<size_t>(img.w * img.h));
            bool exact = true;
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; ++x)
                    if (pal[idx[static_cast<size_t>(y) * img.w + x]] != img.Get(x, y)) exact = false;
            CT_CHECK(exact);
        }
    }
}

CT_TEST(palette_respects_max_colors_and_mean) {
    const Image img = Gradient(256, 128);
    for (int max_colors : {1, 2, 3, 16, 255, 256}) {
        std::vector<uint32_t> pal;
        GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), max_colors, pal);
        CT_CHECK_EQ(static_cast<int>(pal.size()), max_colors);
    }
    // One color: the mean of the image (exact, not a 5-bit bucket centre).
    Image flat(10, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x) flat.Set(x, y, x < 5 ? 0x0A0B0C : 0x141516);
    std::vector<uint32_t> pal;
    GifEncoder::BuildPalette(flat.px.data(), flat.w, flat.h, flat.Stride(), 1, pal);
    CT_REQUIRE(pal.size() == 1);
    CT_CHECK_EQ(pal[0], 0x0F1011u);
    // Empty input gives one black entry.
    GifEncoder::BuildPalette(flat.px.data(), 0, 0, flat.Stride(), 16, pal);
    CT_CHECK_EQ(pal.size(), static_cast<size_t>(1));
}

CT_TEST(palette_weights_by_count) {
    // 90 % of the pixels are shades of blue, 10 % a red: most entries go to the blues, the red keeps one.
    Image img(100, 100);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x)
            img.Set(x, y, y < 10 ? 0xFF0000 : static_cast<uint32_t>((y * 2) << 8 | (x * 2 + 50)));
    std::vector<uint32_t> pal;
    GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), 16, pal);
    CT_CHECK_EQ(static_cast<int>(pal.size()), 16);
    int reds = 0;
    for (uint32_t c : pal)
        if (Channel(c, 0) > 200) ++reds;
    CT_CHECK_EQ(reds, 1);
}

CT_TEST(quantize_without_and_with_dither) {
    const Image img = Gradient(256, 128);
    std::vector<uint32_t> pal;
    GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), 16, pal);
    std::vector<uint8_t> plain, dith;
    GifEncoder::Quantize(img.px.data(), img.w, img.h, img.Stride(), pal, false, plain);
    GifEncoder::Quantize(img.px.data(), img.w, img.h, img.Stride(), pal, true, dith);
    // Without dither every pixel is the nearest entry of its 5-bit bucket: identical colors give identical indices.
    GifEncoder::NearestCache cache;
    cache.Reset(pal.data(), static_cast<int>(pal.size()));
    bool nearest = true;
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const uint32_t c = img.Get(x, y);
            if (plain[static_cast<size_t>(y) * img.w + x] != cache.Lookup(Channel(c, 0), Channel(c, 1), Channel(c, 2))) nearest = false;
        }
    CT_CHECK(nearest);
    CT_CHECK(plain != dith);
    // Dithering keeps local averages much closer to the source.
    const double e_plain = BlockError(img, plain, pal), e_dith = BlockError(img, dith, pal);
    if (ct::test::Verbose()) std::printf("  block error plain %.2f dither %.2f\n", e_plain, e_dith);
    CT_CHECK(e_dith < e_plain * 0.6);
    CT_CHECK(e_dith < 6.0);
    // Deterministic.
    std::vector<uint8_t> again;
    GifEncoder::Quantize(img.px.data(), img.w, img.h, img.Stride(), pal, true, again);
    CT_CHECK(again == dith);
}

CT_TEST(quantize_nearest_is_exact_for_bucket_centres) {
    std::vector<uint32_t> pal = {0x000000, 0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF, 0x808080};
    GifEncoder::NearestCache cache;
    cache.Reset(pal.data(), static_cast<int>(pal.size()));
    bool ok = true;
    for (int r = 0; r < 32; ++r)
        for (int g = 0; g < 32; ++g)
            for (int b = 0; b < 32; ++b) {
                const int cr = r << 3 | 4, cg = g << 3 | 4, cb = b << 3 | 4;
                int best = 0, best_d = 1 << 30;
                for (int i = 0; i < static_cast<int>(pal.size()); ++i) {
                    const int dr = cr - Channel(pal[i], 0), dg = cg - Channel(pal[i], 1), db = cb - Channel(pal[i], 2);
                    const int d = dr * dr + dg * dg + db * db;
                    if (d < best_d) {
                        best_d = d;
                        best = i;
                    }
                }
                if (cache.Lookup(r << 3 | (r & 7), g << 3, b << 3 | 7) != best) ok = false;   // any value in the bucket
            }
    CT_CHECK(ok);
}

CT_TEST(quantize_region_skip_mask) {
    const Image img = Gradient(64, 32);
    std::vector<uint32_t> pal;
    GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), 8, pal);
    GifEncoder::NearestCache cache;
    cache.Reset(pal.data(), static_cast<int>(pal.size()));
    std::vector<uint8_t> skip(64 * 32, 0), out(64 * 32, 0);
    for (int i = 0; i < 64 * 32; i += 3) skip[static_cast<size_t>(i)] = 1;
    // Region with a stride wider than the region (left 64 columns of the image).
    GifEncoder::QuantizeRegion(img.px.data(), 64, 32, img.Stride(), cache, true, skip.data(), 8, out.data());
    bool ok = true;
    for (size_t i = 0; i < out.size(); ++i) {
        if (skip[i] && out[i] != 8) ok = false;
        if (!skip[i] && out[i] >= 8) ok = false;
    }
    CT_CHECK(ok);
}

// Perf follow-up (GIF): the encoder quantizes the bands of a region on several threads; the result equals the
// sequential QuantizeRegion exactly (bands are laid out from the height alone), with and without a skip mask.
CT_TEST(quantize_bands_in_parallel_match_sequential) {
    const Image img = Gradient(1280, 720);
    std::vector<uint32_t> pal;
    GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), 255, pal);
    std::vector<uint8_t> skip(static_cast<size_t>(img.w) * img.h, 0);
    for (size_t i = 0; i < skip.size(); i += 7) skip[i] = 1;
    ct::exportng::gif_detail::Workers w;
    w.Start(4);
    for (const uint8_t* mask : {static_cast<const uint8_t*>(nullptr), static_cast<const uint8_t*>(skip.data())}) {
        GifEncoder::NearestCache seq_cache, par_cache;
        seq_cache.Reset(pal.data(), static_cast<int>(pal.size()));
        par_cache.Reset(pal.data(), static_cast<int>(pal.size()));
        par_cache.FillAll();
        std::vector<uint8_t> seq(skip.size()), par(skip.size());
        GifEncoder::QuantizeRegion(img.px.data(), img.w, img.h, img.Stride(), seq_cache, true, mask, 255, seq.data());
        const int bands = ct::exportng::gif_detail::QuantBands(img.h);
        CT_CHECK(bands > 1);
        w.For(bands, [&](int k) {
            ct::exportng::gif_detail::QuantizeBand(img.px.data(), img.w, img.h, img.Stride(), par_cache, true, mask, 255, par.data(),
                                                   ct::exportng::gif_detail::BandStart(img.h, k),
                                                   ct::exportng::gif_detail::BandStart(img.h, k + 1));
        });
        CT_CHECK(seq == par);
    }
}

CT_BENCH(quantize_dither_720p, 10) {
    const Image img = Gradient(1280, 720);
    std::vector<uint32_t> pal;
    GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), 255, pal);
    std::vector<uint8_t> idx;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) GifEncoder::Quantize(img.px.data(), img.w, img.h, img.Stride(), pal, true, idx);
}

CT_BENCH(quantize_dither_720p_warm_cache, 10) {
    const Image img = Gradient(1280, 720);
    std::vector<uint32_t> pal;
    GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), 255, pal);
    GifEncoder::NearestCache cache;
    cache.Reset(pal.data(), static_cast<int>(pal.size()));
    std::vector<uint8_t> idx(static_cast<size_t>(img.w) * img.h);
    GifEncoder::QuantizeRegion(img.px.data(), img.w, img.h, img.Stride(), cache, true, nullptr, 0, idx.data());
    for (int64_t i = 0; i < CT_BENCH_N; ++i)
        GifEncoder::QuantizeRegion(img.px.data(), img.w, img.h, img.Stride(), cache, true, nullptr, 0, idx.data());
}

CT_BENCH(palette_build_720p, 10) {
    const Image img = Gradient(1280, 720);
    std::vector<uint32_t> pal;
    for (int64_t i = 0; i < CT_BENCH_N; ++i) GifEncoder::BuildPalette(img.px.data(), img.w, img.h, img.Stride(), 255, pal);
}
