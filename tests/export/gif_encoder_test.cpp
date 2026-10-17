// Full GifEncoder: stream structure, frame differencing, delay merging, local palettes, dithered fidelity.
#include "gif_reader.h"

#include <ct/core/fs.h>
#include <ct/export/gif_encoder.h>
#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::exportng;
namespace gif = ct::test::gif;

namespace {

struct TempFile {
    std::string path = fs::TempPath(".gif");
    ~TempFile() { fs::Delete(path); }
};

struct Frame {
    int w, h;
    std::vector<uint8_t> px;   // BGRA
    Frame(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 4, 255) {}
    void Set(int x, int y, uint32_t rgb) {
        uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        p[0] = static_cast<uint8_t>(rgb);
        p[1] = static_cast<uint8_t>(rgb >> 8);
        p[2] = static_cast<uint8_t>(rgb >> 16);
    }
    uint32_t Get(int x, int y) const {
        const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
        return (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0];
    }
    void Fill(int x0, int y0, int fw, int fh, uint32_t rgb) {
        for (int y = y0; y < y0 + fh; ++y)
            for (int x = x0; x < x0 + fw; ++x)
                if (x >= 0 && y >= 0 && x < w && y < h) Set(x, y, rgb);
    }
};

// Ten frames: a 4-color striped background, a moving square, a counter block that changes every other frame.
std::vector<Frame> SyntheticFrames(int w, int h, int count) {
    static const uint32_t bg[4] = {0x203040, 0x304050, 0x405060, 0x506070};
    std::vector<Frame> frames;
    for (int i = 0; i < count; ++i) {
        Frame f(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) f.Set(x, y, bg[(y / 4) % 4]);
        f.Fill(3 + i * 4, 10 + i, 9, 9, 0xFF2000);
        f.Fill(w - 12, 2, 8, 6, (i / 2) % 2 ? 0xFF2000 : 0x00A0FF);
        frames.push_back(std::move(f));
    }
    return frames;
}

bool ReadGif(const std::string& path, gif::File& out) {
    std::string data, err;
    if (!fs::ReadAll(path, data)) return false;
    if (!gif::Parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), out, &err)) {
        std::printf("  parse failed: %s\n", err.c_str());
        return false;
    }
    return true;
}

double Psnr(const Frame& f, const std::vector<uint32_t>& canvas) {
    double se = 0;
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            const uint32_t a = f.Get(x, y), b = canvas[static_cast<size_t>(y) * f.w + x];
            for (int s = 0; s < 24; s += 8) {
                const double d = static_cast<double>((a >> s) & 255) - static_cast<double>((b >> s) & 255);
                se += d * d;
            }
        }
    const double mse = se / (static_cast<double>(f.w) * f.h * 3);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

CT_TEST(gif_frame_diff_decodes_to_identical_indices) {
    const int w = 64, h = 48;
    const std::vector<Frame> frames = SyntheticFrames(w, h, 10);
    TempFile file;
    GifEncoder enc;
    GifEncoder::Options opt;
    opt.dither = false;
    CT_REQUIRE(enc.Open(file.path, w, h, opt));
    for (const Frame& f : frames) CT_CHECK(enc.AddFrame(f.px.data(), f.w * 4, 5));
    CT_REQUIRE(enc.Close());
    CT_CHECK_EQ(enc.FramesWritten(), 10);
    CT_CHECK_EQ(enc.BytesWritten(), fs::FileSize(file.path));

    gif::File g;
    CT_REQUIRE(ReadGif(file.path, g));
    CT_CHECK_EQ(g.width, w);
    CT_CHECK_EQ(g.height, h);
    CT_CHECK(g.global_table);
    CT_CHECK_EQ(g.loop_count, 0);
    CT_CHECK(g.trailer);
    CT_REQUIRE(g.frames.size() == 10);
    // The global palette is the first frame's (6 colors) plus the reserved transparent slot -> 8 entries.
    CT_CHECK_EQ(g.global_palette.size(), static_cast<size_t>(8));
    const std::vector<uint32_t> palette(g.global_palette.begin(), g.global_palette.begin() + 6);

    std::vector<std::vector<uint32_t>> canvases;
    gif::Composite(g, canvases);
    for (size_t i = 0; i < frames.size(); ++i) {
        const gif::Frame& fr = g.frames[i];
        CT_CHECK_EQ(fr.delay_cs, 5);
        CT_CHECK_EQ(fr.disposal, 1);
        if (i == 0) {
            CT_CHECK(fr.x == 0 && fr.y == 0 && fr.w == w && fr.h == h);
            CT_CHECK_EQ(fr.transparent, -1);
        } else {
            CT_CHECK_EQ(fr.transparent, 6);
            CT_CHECK(fr.w * fr.h < w * h / 2);   // dirty rect only
        }
        // Expected: the frame quantized on its own with the same palette.
        std::vector<uint8_t> want;
        GifEncoder::Quantize(frames[i].px.data(), w, h, w * 4, palette, false, want);
        bool same = true;
        for (size_t p = 0; p < want.size(); ++p)
            if (canvases[i][p] != (0xFF000000u | palette[want[p]])) same = false;
        CT_CHECK(same);
        // With <= 6 exact colors the indices are lossless.
        CT_CHECK(Psnr(frames[i], canvases[i]) >= 99.0);
    }
}

CT_TEST(gif_identical_frames_merge_delays) {
    const int w = 32, h = 16;
    std::vector<Frame> frames = SyntheticFrames(w, h, 3);
    TempFile file;
    GifEncoder enc;
    CT_REQUIRE(enc.Open(file.path, w, h, GifEncoder::Options{}));
    CT_CHECK(enc.AddFrame(frames[0].px.data(), w * 4, 4));
    CT_CHECK(enc.AddFrame(frames[0].px.data(), w * 4, 6));   // identical: extends frame 0
    CT_CHECK(enc.AddFrame(frames[1].px.data(), w * 4, 1));   // clamped to 2
    CT_CHECK(enc.AddFrame(frames[1].px.data(), w * 4, 65535));   // overflow: a 1x1 transparent frame
    CT_CHECK(enc.AddFrame(frames[1].px.data(), w * 4, 100));
    CT_REQUIRE(enc.Close());
    gif::File g;
    CT_REQUIRE(ReadGif(file.path, g));
    CT_REQUIRE(g.frames.size() == 4);
    CT_CHECK_EQ(g.frames[0].delay_cs, 10);
    CT_CHECK_EQ(g.frames[1].delay_cs, 2);       // 2 + 65535 does not fit the 16-bit field
    CT_CHECK_EQ(g.frames[2].delay_cs, 65535);   // 1x1 transparent frame
    CT_CHECK(g.frames[2].w == 1 && g.frames[2].h == 1 && g.frames[2].indices[0] == g.frames[2].transparent);
    CT_CHECK_EQ(g.frames[3].delay_cs, 100);
}

CT_TEST(gif_no_frame_diff_writes_full_frames) {
    const int w = 40, h = 30;
    const std::vector<Frame> frames = SyntheticFrames(w, h, 4);
    TempFile file;
    GifEncoder enc;
    GifEncoder::Options opt;
    opt.frame_diff = false;
    opt.dither = false;
    opt.loop_count = 3;
    CT_REQUIRE(enc.Open(file.path, w, h, opt));
    for (const Frame& f : frames) CT_CHECK(enc.AddFrame(f.px.data(), w * 4, GifEncoder::DelayForFps(20)));
    CT_REQUIRE(enc.Close());
    gif::File g;
    CT_REQUIRE(ReadGif(file.path, g));
    CT_CHECK_EQ(g.loop_count, 3);
    CT_REQUIRE(g.frames.size() == 4);
    for (const gif::Frame& fr : g.frames) {
        CT_CHECK(fr.x == 0 && fr.y == 0 && fr.w == w && fr.h == h);
        CT_CHECK_EQ(fr.transparent, -1);
        CT_CHECK_EQ(fr.delay_cs, 5);
    }
    CT_CHECK_EQ(g.global_palette.size(), static_cast<size_t>(8));   // 6 colors, no reserved slot needed
}

CT_TEST(gif_per_frame_palette_is_lossless_for_few_colors) {
    const int w = 48, h = 40;
    std::vector<Frame> frames;
    for (int i = 0; i < 6; ++i) {
        Frame f(w, h);
        // Each frame has its own 4 colors; together more than any single palette of 4 entries could hold.
        const uint32_t base = 0x111111u * static_cast<uint32_t>(i + 1);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) f.Set(x, y, ((x / 8 + y / 8) % 2) ? base : (base ^ 0x804020u));
        f.Fill(i * 5, i * 3, 10, 10, 0x00FF00u + static_cast<uint32_t>(i));
        f.Fill(30, 30, 4, 4, 0x0000FFu + static_cast<uint32_t>(i) * 0x10000u);
        frames.push_back(std::move(f));
    }
    TempFile file;
    GifEncoder enc;
    GifEncoder::Options opt;
    opt.per_frame_palette = true;
    opt.max_colors = 8;
    CT_REQUIRE(enc.Open(file.path, w, h, opt));
    for (const Frame& f : frames) CT_CHECK(enc.AddFrame(f.px.data(), w * 4, 10));
    CT_REQUIRE(enc.Close());
    gif::File g;
    CT_REQUIRE(ReadGif(file.path, g));
    CT_CHECK(!g.global_table);
    CT_REQUIRE(g.frames.size() == frames.size());
    std::vector<std::vector<uint32_t>> canvases;
    gif::Composite(g, canvases);
    for (size_t i = 0; i < frames.size(); ++i) {
        CT_CHECK(g.frames[i].local_table);
        CT_CHECK(g.frames[i].palette.size() <= 8);
        CT_CHECK(Psnr(frames[i], canvases[i]) >= 99.0);
    }
}

CT_TEST(gif_dithered_gradient_fidelity) {
    const int w = 160, h = 90;
    std::vector<Frame> frames;
    for (int i = 0; i < 8; ++i) {
        Frame f(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint32_t r = static_cast<uint32_t>((x + i * 3) * 255 / (w + 24));
                const uint32_t gg = static_cast<uint32_t>(y * 255 / (h - 1));
                f.Set(x, y, (r << 16) | (gg << 8) | 0x60);
            }
        f.Fill(20 + i * 6, 30, 30, 12, 0xF0F0F0);
        frames.push_back(std::move(f));
    }
    TempFile file;
    GifEncoder enc;
    GifEncoder::Options opt;   // defaults: dither, frame diff, 256 colors
    CT_REQUIRE(enc.Open(file.path, w, h, opt));
    for (size_t i = 0; i < frames.size(); i += 4) enc.AddPaletteSample(frames[i].px.data(), w * 4);
    for (const Frame& f : frames) CT_CHECK(enc.AddFrame(f.px.data(), w * 4, 5));
    CT_REQUIRE(enc.Close());
    gif::File g;
    CT_REQUIRE(ReadGif(file.path, g));
    CT_REQUIRE(g.frames.size() == frames.size());
    CT_CHECK(g.global_palette.size() == 256);
    std::vector<std::vector<uint32_t>> canvases;
    gif::Composite(g, canvases);
    for (size_t i = 0; i < frames.size(); ++i) {
        const double psnr = Psnr(frames[i], canvases[i]);
        if (ct::test::Verbose()) std::printf("  frame %zu psnr %.1f dB\n", i, psnr);
        CT_CHECK(psnr >= 32.0);
        bool painted = true;
        for (uint32_t c : canvases[i])
            if ((c >> 24) == 0) painted = false;
        CT_CHECK(painted);
    }
}

CT_TEST(gif_stride_and_empty_file) {
    const int w = 20, h = 10, stride = w * 4 + 36;
    std::vector<uint8_t> px(static_cast<size_t>(stride) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[static_cast<size_t>(y) * stride + x * 4];
            p[0] = static_cast<uint8_t>(x * 10);
            p[1] = static_cast<uint8_t>(y * 20);
            p[2] = 128;
            p[3] = 255;
        }
    {
        TempFile file;
        GifEncoder enc;
        GifEncoder::Options opt;
        opt.dither = false;
        CT_REQUIRE(enc.Open(file.path, w, h, opt));
        CT_CHECK(enc.AddFrame(px.data(), stride, 10));
        CT_REQUIRE(enc.Close());
        gif::File g;
        CT_REQUIRE(ReadGif(file.path, g));
        std::vector<std::vector<uint32_t>> canvases;
        gif::Composite(g, canvases);
        CT_REQUIRE(canvases.size() == 1);
        CT_CHECK_EQ(canvases[0][static_cast<size_t>(5) * w + 7] & 0xFFFFFFu, (128u << 16) | (100u << 8) | 70u);
    }
    {
        TempFile file;
        GifEncoder enc;
        CT_REQUIRE(enc.Open(file.path, w, h, GifEncoder::Options{}));
        CT_REQUIRE(enc.Close());
        gif::File g;
        CT_REQUIRE(ReadGif(file.path, g));
        CT_CHECK(g.frames.empty());
    }
    GifEncoder bad;
    CT_CHECK(!bad.Open("Z:\\does\\not\\exist\\x.gif", w, h, GifEncoder::Options{}));
    CT_CHECK(!bad.AddFrame(px.data(), stride, 10));
    CT_CHECK(bad.Close());
    CT_CHECK_EQ(GifEncoder::DelayForFps(20), 5);
    CT_CHECK_EQ(GifEncoder::DelayForFps(15), 7);
    CT_CHECK_EQ(GifEncoder::DelayForFps(60), 2);
}
