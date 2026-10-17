// gif_demo: encodes a synthetic animated sequence (moving gradient + text-like blocks, default 720p, 20 fps, 5 s) with
// ct::exportng::GifEncoder and reports the encode speed against real time and the file size. The P10 budget is
// 720p20 >= 2x real time. Optionally decodes the result with tests/common/gif_reader.h and writes one composited frame
// (and the matching source frame) as PNG for a visual check.
//
//   gif_demo [--out file.gif] [--width 1280] [--height 720] [--fps 20] [--seconds 5] [--colors 256]
//            [--no-dither] [--no-diff] [--per-frame] [--static-bg] [--png N]
#include "gif_reader.h"

#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/export/gif_encoder.h>
#include <ct/platform/image_io.h>
#include <ct/platform/win.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::exportng;

namespace {

struct Args {
    std::string out = "gif_demo.gif";
    int width = 1280, height = 720, fps = 20, colors = 256, png = -1;
    double seconds = 5;
    bool dither = true, diff = true, per_frame = false, static_bg = false;
};

// 5x7 glyph-like bit patterns (not real letters; enough to look like text after quantization).
const uint8_t kGlyphs[8][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x1E, 0x11, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x11, 0x13, 0x15, 0x19, 0x11, 0x11, 0x11}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
};

class Generator {
public:
    Generator(int w, int h, bool static_bg) : m_w(w), m_h(h), m_static(static_bg), m_col(static_cast<size_t>(w) * 3) {}

    void Render(int frame, std::vector<uint8_t>& px) {
        px.resize(static_cast<size_t>(m_w) * m_h * 4);
        const double t = m_static ? 0.0 : frame / 20.0;
        // Moving gradient: per-column RGB from slow sines, vertical blend towards a deep blue.
        for (int x = 0; x < m_w; ++x) {
            const double u = static_cast<double>(x) / m_w;
            m_col[x * 3 + 0] = 0.5 + 0.45 * std::sin(6.2832 * (u * 0.8 + t * 0.15));
            m_col[x * 3 + 1] = 0.5 + 0.45 * std::sin(6.2832 * (u * 0.6 - t * 0.10) + 2.0);
            m_col[x * 3 + 2] = 0.5 + 0.45 * std::sin(6.2832 * (u * 0.4 + t * 0.05) + 4.0);
        }
        for (int y = 0; y < m_h; ++y) {
            const double v = static_cast<double>(y) / m_h;
            uint8_t* row = px.data() + static_cast<size_t>(y) * m_w * 4;
            for (int x = 0; x < m_w; ++x) {
                const double r = m_col[x * 3 + 0] * (1 - v) + 0.10 * v;
                const double g = m_col[x * 3 + 1] * (1 - v) + 0.15 * v;
                const double b = m_col[x * 3 + 2] * (1 - v) + 0.45 * v;
                row[x * 4 + 0] = static_cast<uint8_t>(b * 255);
                row[x * 4 + 1] = static_cast<uint8_t>(g * 255);
                row[x * 4 + 2] = static_cast<uint8_t>(r * 255);
                row[x * 4 + 3] = 255;
            }
        }
        // A "window" with a title bar and text being typed (4 glyphs per frame), plus a scrolling log panel.
        const int wx = m_w / 10, wy = m_h / 8, ww = m_w / 2, wh = m_h * 3 / 5;
        Rect(px, wx + 8, wy + 8, ww, wh, 0x30303030u, true);   // shadow (blended)
        Rect(px, wx, wy, ww, wh, 0xF4F4F4, false);
        Rect(px, wx, wy, ww, 28, 0x2B5797, false);
        Rect(px, wx + ww - 24, wy + 8, 12, 12, 0xE81123, false);
        const int cols = (ww - 32) / 8, typed = frame * 4;
        for (int i = 0; i < typed && i / cols < (wh - 60) / 14; ++i) {
            const int cx = wx + 16 + (i % cols) * 8, cy = wy + 44 + (i / cols) * 14;
            if ((i * 7 + 3) % 11 == 0) continue;   // spaces
            Glyph(px, cx, cy, kGlyphs[(i * 5 + i / 3) % 8], (i / cols) % 5 == 0 ? 0x0063B1u : 0x202020u);
        }
        const int lx = m_w * 2 / 3, ly = m_h / 6, lw = m_w / 4, lh = m_h / 2;
        Rect(px, lx, ly, lw, lh, 0x1E1E1E, false);
        const int line_h = 12, scroll = frame * 3;
        for (int line = 0; line < lh / line_h + 1; ++line) {
            const int src_line = line + scroll / line_h;
            const int y = ly + 6 + line * line_h - scroll % line_h;
            const int len = 6 + (src_line * 13) % 24;
            for (int c = 0; c < len && 10 + c * 7 < lw - 10; ++c)
                Glyph(px, lx + 10 + c * 7, y, kGlyphs[(src_line + c) % 8], src_line % 4 == 0 ? 0x6A9955u : 0xD4D4D4u, ly, ly + lh);
        }
        // A moving cursor arrow-ish block.
        const int mx = static_cast<int>(m_w * (0.3 + 0.25 * std::sin(frame * 0.12))), my = static_cast<int>(m_h * (0.5 + 0.2 * std::cos(frame * 0.09)));
        Rect(px, mx, my, 12, 18, 0x000000, false);
        Rect(px, mx + 2, my + 2, 8, 14, 0xFFFFFF, false);
    }

private:
    void Put(std::vector<uint8_t>& px, int x, int y, uint32_t rgb) {
        if (x < 0 || y < 0 || x >= m_w || y >= m_h) return;
        uint8_t* p = px.data() + (static_cast<size_t>(y) * m_w + x) * 4;
        p[0] = static_cast<uint8_t>(rgb);
        p[1] = static_cast<uint8_t>(rgb >> 8);
        p[2] = static_cast<uint8_t>(rgb >> 16);
    }
    void Rect(std::vector<uint8_t>& px, int x0, int y0, int w, int h, uint32_t rgb, bool darken) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x) {
                if (x < 0 || y < 0 || x >= m_w || y >= m_h) continue;
                if (darken) {
                    uint8_t* p = px.data() + (static_cast<size_t>(y) * m_w + x) * 4;
                    p[0] = static_cast<uint8_t>(p[0] * 3 / 5);
                    p[1] = static_cast<uint8_t>(p[1] * 3 / 5);
                    p[2] = static_cast<uint8_t>(p[2] * 3 / 5);
                } else {
                    Put(px, x, y, rgb);
                }
            }
    }
    void Glyph(std::vector<uint8_t>& px, int x0, int y0, const uint8_t* bits, uint32_t rgb, int clip_y0 = 0, int clip_y1 = 1 << 30) {
        for (int r = 0; r < 7; ++r) {
            const int y = y0 + r;
            if (y < clip_y0 || y >= clip_y1) continue;
            for (int c = 0; c < 5; ++c)
                if (bits[r] & (0x10 >> c)) Put(px, x0 + c, y, rgb);
        }
    }
    int m_w, m_h;
    bool m_static;
    std::vector<double> m_col;
};

bool ParseArgs(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&](void) -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (s == "--out" && (v = next())) a.out = v;
        else if (s == "--width" && (v = next())) a.width = std::atoi(v);
        else if (s == "--height" && (v = next())) a.height = std::atoi(v);
        else if (s == "--fps" && (v = next())) a.fps = std::atoi(v);
        else if (s == "--seconds" && (v = next())) a.seconds = std::atof(v);
        else if (s == "--colors" && (v = next())) a.colors = std::atoi(v);
        else if (s == "--png" && (v = next())) a.png = std::atoi(v);
        else if (s == "--no-dither") a.dither = false;
        else if (s == "--no-diff") a.diff = false;
        else if (s == "--per-frame") a.per_frame = true;
        else if (s == "--static-bg") a.static_bg = true;
        else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", s.c_str());
            return false;
        }
    }
    return a.width > 0 && a.height > 0 && a.fps > 0 && a.seconds > 0;
}

bool WritePng(const std::string& path, const std::vector<uint32_t>& argb, int w, int h) {
    std::vector<uint8_t> rgba(argb.size() * 4);
    for (size_t i = 0; i < argb.size(); ++i) {
        rgba[i * 4 + 0] = static_cast<uint8_t>(argb[i] >> 16);
        rgba[i * 4 + 1] = static_cast<uint8_t>(argb[i] >> 8);
        rgba[i * 4 + 2] = static_cast<uint8_t>(argb[i]);
        rgba[i * 4 + 3] = 255;
    }
    return platform::ImageIo::WritePngRgba(path.c_str(), rgba.data(), w, h, w * 4);
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!ParseArgs(argc, argv, a)) {
        std::fprintf(stderr, "usage: gif_demo [--out f.gif] [--width W] [--height H] [--fps N] [--seconds S] [--colors N]\n"
                             "                [--no-dither] [--no-diff] [--per-frame] [--static-bg] [--png N]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Async::Init(2);
    const int frames = static_cast<int>(std::ceil(a.seconds * a.fps));
    const int delay = GifEncoder::DelayForFps(a.fps);
    Generator gen(a.width, a.height, a.static_bg);
    std::vector<uint8_t> px;

    GifEncoder::Options opt;
    opt.max_colors = a.colors;
    opt.dither = a.dither;
    opt.frame_diff = a.diff;
    opt.per_frame_palette = a.per_frame;
    GifEncoder enc;
    if (!enc.Open(a.out, a.width, a.height, opt)) {
        std::fprintf(stderr, "cannot create %s\n", a.out.c_str());
        return 1;
    }
    Stopwatch total;
    total.Start();
    double gen_ms = 0, sample_ms = 0;
    if (!a.per_frame) {   // pass 1: palette from every 30th frame (what ExportJob does)
        Stopwatch sw;
        sw.Start();
        for (int i = 0; i < frames; i += 30) {
            gen.Render(i, px);
            enc.AddPaletteSample(px.data(), a.width * 4);
        }
        sample_ms = sw.ElapsedMs();
    }
    double add_wait_ms = 0;
    for (int i = 0; i < frames; ++i) {
        Stopwatch sw;
        sw.Start();
        gen.Render(i, px);
        gen_ms += sw.ElapsedMs();
        Stopwatch aw;
        aw.Start();
        if (!enc.AddFrame(px.data(), a.width * 4, delay)) {
            std::fprintf(stderr, "AddFrame failed\n");
            return 1;
        }
        add_wait_ms += aw.ElapsedMs();
    }
    Stopwatch cw;
    cw.Start();
    const bool ok = enc.Close();
    const double close_ms = cw.ElapsedMs();
    const double total_ms = total.ElapsedMs();
    const double video_ms = frames * 1000.0 / a.fps;
    const double busy_ms = enc.EncodeBusyMs();
    std::printf("gif_demo: %dx%d, %d fps, %d frames (%.2f s of video), colors %d, dither %s, frame diff %s, %s palette%s\n",
                a.width, a.height, a.fps, frames, video_ms / 1000, a.colors, a.dither ? "on" : "off", a.diff ? "on" : "off",
                a.per_frame ? "per-frame" : "global", a.static_bg ? ", static background" : "");
    std::printf("  palette pass %.0f ms; frame generation %.0f ms (%.1f ms/frame, overlapped with encoding)\n", sample_ms, gen_ms,
                gen_ms / frames);
    std::printf("  AddFrame blocked %.0f ms, Close drained %.0f ms\n", add_wait_ms, close_ms);
    std::printf("  encoder busy %.0f ms (%.1f ms/frame) -> encode speed %.2fx real time (budget 720p20 >= 2x)\n", busy_ms,
                busy_ms / frames, video_ms / std::max(1.0, busy_ms));
    std::printf("  wall %.0f ms incl. generation -> %.2fx real time end to end\n", total_ms, video_ms / total_ms);
    std::printf("  file %s: %s (%llu bytes), %d GIF frames, %.1f KB/frame\n", a.out.c_str(), FormatBytes(enc.BytesWritten()).c_str(),
                static_cast<unsigned long long>(enc.BytesWritten()), enc.FramesWritten(), enc.BytesWritten() / 1024.0 / frames);
    if (!ok) {
        std::fprintf(stderr, "Close failed\n");
        return 1;
    }

    if (a.png >= 0) {
        std::string data, err;
        ct::test::gif::File g;
        if (!fs::ReadAll(a.out, data) || !ct::test::gif::Parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), g, &err)) {
            std::fprintf(stderr, "decode failed: %s\n", err.c_str());
            return 1;
        }
        std::vector<std::vector<uint32_t>> canvases;
        ct::test::gif::Composite(g, canvases);
        const int n = std::min(a.png, static_cast<int>(canvases.size()) - 1);
        const std::string base(PathStem(a.out));
        const std::string dir(PathParent(a.out));
        const std::string dec_path = PathJoin(dir, base + Format("_frame%d_decoded.png", n));
        const std::string src_path = PathJoin(dir, base + Format("_frame%d_source.png", n));
        gen.Render(n, px);
        std::vector<uint32_t> src(static_cast<size_t>(a.width) * a.height);
        double se = 0;
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = (static_cast<uint32_t>(px[i * 4 + 2]) << 16) | (static_cast<uint32_t>(px[i * 4 + 1]) << 8) | px[i * 4];
            for (int s = 0; s < 24; s += 8) {
                const double d = static_cast<double>((src[i] >> s) & 255) - static_cast<double>((canvases[n][i] >> s) & 255);
                se += d * d;
            }
        }
        const double mse = se / (src.size() * 3.0);
        std::printf("  frame %d (GIF frames == source frames: %s): PSNR %.2f dB vs source\n", n,
                    canvases.size() == static_cast<size_t>(frames) ? "yes" : "no", 10 * std::log10(255.0 * 255 / std::max(mse, 1e-9)));
        if (WritePng(dec_path, canvases[n], a.width, a.height) && WritePng(src_path, src, a.width, a.height))
            std::printf("  wrote %s and %s\n", dec_path.c_str(), src_path.c_str());
    }
    Async::Shutdown();
    CoUninitialize();
    return 0;
}
