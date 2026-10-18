// Palette (median cut on a 15-bit histogram), nearest-color cache and Floyd-Steinberg quantization.
#include <ct/export/gif_encoder.h>

#include "gif_internal.h"

#include <algorithm>
#include <cstring>

namespace ct::exportng {

namespace {

constexpr int kBins = 32768;

struct ColorBin {
    uint64_t count;
    uint8_t c[3];   // 5-bit r, g, b
    uint16_t key;
};

struct Box {
    size_t begin, end;
    uint64_t count;
    int lo[3], hi[3];
    int Widest() const {
        int best = 0;
        for (int ch = 1; ch < 3; ++ch)
            if (hi[ch] - lo[ch] > hi[best] - lo[best]) best = ch;
        return best;
    }
    int Range() const { const int ch = Widest(); return hi[ch] - lo[ch]; }
};

void Measure(Box& box, const std::vector<ColorBin>& bins) {
    box.count = 0;
    for (int ch = 0; ch < 3; ++ch) {
        box.lo[ch] = 31;
        box.hi[ch] = 0;
    }
    for (size_t i = box.begin; i < box.end; ++i) {
        const ColorBin& b = bins[i];
        box.count += b.count;
        for (int ch = 0; ch < 3; ++ch) {
            box.lo[ch] = std::min<int>(box.lo[ch], b.c[ch]);
            box.hi[ch] = std::max<int>(box.hi[ch], b.c[ch]);
        }
    }
}

inline int Clamp255(int v) { return std::min(std::max(v, 0), 255); }
inline int ClampErr(int v) { return std::min(std::max(v, -32), 32); }

} // namespace

// ---- histogram ----------------------------------------------------------------------------------------------------

GifEncoder::Histogram::Histogram() : bins(kBins) {}

void GifEncoder::Histogram::Clear() { std::fill(bins.begin(), bins.end(), Bin{}); }

void GifEncoder::Histogram::Add(const uint8_t* bgra, int w, int h, int stride) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = bgra + static_cast<ptrdiff_t>(y) * stride;
        for (int x = 0; x < w; ++x, p += 4) Add(p[2], p[1], p[0]);
    }
}

// ---- median cut ---------------------------------------------------------------------------------------------------

void GifEncoder::BuildPalette(const Histogram& hist, int max_colors, std::vector<uint32_t>& palette) {
    palette.clear();
    max_colors = std::clamp(max_colors, 1, 256);
    std::vector<ColorBin> bins;
    for (int k = 0; k < static_cast<int>(hist.bins.size()); ++k) {
        if (!hist.bins[k].count) continue;
        bins.push_back({hist.bins[k].count,
                        {static_cast<uint8_t>(k >> 10), static_cast<uint8_t>((k >> 5) & 31), static_cast<uint8_t>(k & 31)},
                        static_cast<uint16_t>(k)});
    }
    if (bins.empty()) {
        palette.push_back(0);
        return;
    }
    std::vector<Box> boxes;
    boxes.push_back({0, bins.size(), 0, {}, {}});
    Measure(boxes[0], bins);
    while (static_cast<int>(boxes.size()) < max_colors) {
        int pick = -1;
        double best = 0;
        for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
            const Box& b = boxes[i];
            if (b.end - b.begin < 2) continue;
            const double score = static_cast<double>(b.Range()) * static_cast<double>(b.count);
            if (score > best) {
                best = score;
                pick = i;
            }
        }
        if (pick < 0) break;   // every box holds a single 15-bit color
        Box box = boxes[pick];
        const int ch = box.Widest();
        std::sort(bins.begin() + box.begin, bins.begin() + box.end, [ch](const ColorBin& a, const ColorBin& b) {
            return a.c[ch] != b.c[ch] ? a.c[ch] < b.c[ch] : a.key < b.key;
        });
        // Weighted median; both halves keep at least one bin.
        const uint64_t half = box.count / 2;
        uint64_t acc = 0;
        size_t mid = box.begin + 1;
        for (size_t i = box.begin; i < box.end; ++i) {
            acc += bins[i].count;
            if (acc >= half) {
                mid = i + 1;
                break;
            }
        }
        mid = std::clamp(mid, box.begin + 1, box.end - 1);
        Box a{box.begin, mid, 0, {}, {}}, b{mid, box.end, 0, {}, {}};
        Measure(a, bins);
        Measure(b, bins);
        boxes[pick] = a;
        boxes.push_back(b);
    }
    palette.reserve(boxes.size());
    for (const Box& box : boxes) {
        uint64_t n = 0, r = 0, g = 0, b = 0;
        for (size_t i = box.begin; i < box.end; ++i) {
            const Histogram::Bin& bin = hist.bins[bins[i].key];
            n += bin.count;
            r += bin.r;
            g += bin.g;
            b += bin.b;
        }
        const uint32_t pr = static_cast<uint32_t>((r + n / 2) / n), pg = static_cast<uint32_t>((g + n / 2) / n),
                       pb = static_cast<uint32_t>((b + n / 2) / n);
        palette.push_back((pr << 16) | (pg << 8) | pb);
    }
}

void GifEncoder::BuildPalette(const uint8_t* bgra, int w, int h, int stride, int max_colors, std::vector<uint32_t>& palette) {
    Histogram hist;
    hist.Add(bgra, w, h, stride);
    BuildPalette(hist, max_colors, palette);
}

// ---- nearest color ------------------------------------------------------------------------------------------------

void GifEncoder::NearestCache::Reset(const uint32_t* palette, int count) {
    m_palette.assign(palette, palette + std::clamp(count, 0, 256));
    if (m_palette.empty()) m_palette.push_back(0);
    m_table.assign(kBins, -1);
}

uint8_t GifEncoder::NearestCache::Fill(uint32_t key) {
    const int r = static_cast<int>(((key >> 10) & 31) << 3) | 4;
    const int g = static_cast<int>(((key >> 5) & 31) << 3) | 4;
    const int b = static_cast<int>((key & 31) << 3) | 4;
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < static_cast<int>(m_palette.size()); ++i) {
        const uint32_t p = m_palette[i];
        const int dr = r - static_cast<int>((p >> 16) & 255), dg = g - static_cast<int>((p >> 8) & 255),
                  db = b - static_cast<int>(p & 255);
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    m_table[key] = static_cast<int16_t>(best);
    return static_cast<uint8_t>(best);
}

// ---- quantization -------------------------------------------------------------------------------------------------

namespace {

// One serpentine Floyd-Steinberg row. kStep = +1 (left to right) or -1. cur/next: (w + 2) * 3 errors x16.
template <bool kSkip, int kStep>
void DitherRow(const uint8_t* src, uint8_t* out, const uint8_t* skip, const int32_t* cur, int32_t* next, int w,
               GifEncoder::NearestCache& cache, uint8_t skip_index) {
    const uint32_t* pal = cache.Palette();
    int x = 0;
    if constexpr (kStep < 0) x = w - 1;
    int carry_r = 0, carry_g = 0, carry_b = 0;   // 7/16 from the previous pixel
    int prev_r = 0, prev_g = 0, prev_b = 0;      // pending next[x]: 1/16 from x - step plus 5/16 from x
    int ahead_r = 0, ahead_g = 0, ahead_b = 0;   // pending next[x + step]: 1/16 from x
    for (int n = 0; n < w; ++n, x += kStep) {
        int er = 0, eg = 0, eb = 0;
        bool skipped = false;
        if constexpr (kSkip) skipped = skip[x] != 0;
        if (skipped) {
            out[x] = skip_index;
        } else {
            const uint8_t* p = src + static_cast<ptrdiff_t>(x) * 4;
            const int32_t* e = cur + (x + 1) * 3;
            const int r = Clamp255(p[2] + ClampErr((e[0] + carry_r + 8) >> 4));
            const int g = Clamp255(p[1] + ClampErr((e[1] + carry_g + 8) >> 4));
            const int b = Clamp255(p[0] + ClampErr((e[2] + carry_b + 8) >> 4));
            const uint8_t idx = cache.Lookup(r, g, b);
            out[x] = idx;
            const uint32_t c = pal[idx];
            er = r - static_cast<int>((c >> 16) & 255);
            eg = g - static_cast<int>((c >> 8) & 255);
            eb = b - static_cast<int>(c & 255);
        }
        carry_r = er * 7;
        carry_g = eg * 7;
        carry_b = eb * 7;
        int32_t* behind = next + (x + 1 - kStep) * 3;   // final value of next[x - step]
        behind[0] = prev_r + er * 3;
        behind[1] = prev_g + eg * 3;
        behind[2] = prev_b + eb * 3;
        prev_r = ahead_r + er * 5;
        prev_g = ahead_g + eg * 5;
        prev_b = ahead_b + eb * 5;
        ahead_r = er;
        ahead_g = eg;
        ahead_b = eb;
    }
    int32_t* last = next + (x + 1 - kStep) * 3;   // x is one past the end: next[last pixel]
    last[0] = prev_r;
    last[1] = prev_g;
    last[2] = prev_b;
}

} // namespace

void GifEncoder::QuantizeRegion(const uint8_t* bgra, int w, int h, int stride, NearestCache& cache, bool dither,
                                const uint8_t* skip, uint8_t skip_index, uint8_t* out) {
    if (w <= 0 || h <= 0) return;
    const int bands = gif_detail::QuantBands(h);
    for (int k = 0; k < bands; ++k)
        gif_detail::QuantizeBand(bgra, w, h, stride, cache, dither, skip, skip_index, out, gif_detail::BandStart(h, k),
                                 gif_detail::BandStart(h, k + 1));
}

void GifEncoder::NearestCache::FillRange(uint32_t begin, uint32_t end) {
    end = std::min<uint32_t>(end, kBins);
    for (uint32_t k = begin; k < end; ++k)
        if (m_table[k] < 0) Fill(k);
}

namespace gif_detail {

int QuantBands(int h) { return std::clamp(h / 48, 1, 64); }

void QuantizeBand(const uint8_t* bgra, int w, int h, int stride, GifEncoder::NearestCache& cache, bool dither, const uint8_t* skip,
                  uint8_t skip_index, uint8_t* out, int y0, int y1) {
    (void)h;
    if (w <= 0 || y1 <= y0) return;
    if (!dither) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* p = bgra + static_cast<ptrdiff_t>(y) * stride;
            uint8_t* o = out + static_cast<ptrdiff_t>(y) * w;
            const uint8_t* s = skip ? skip + static_cast<ptrdiff_t>(y) * w : nullptr;
            for (int x = 0; x < w; ++x, p += 4)
                o[x] = (s && s[x]) ? skip_index : cache.Lookup(p[2], p[1], p[0]);
        }
        return;
    }
    // Errors are kept x16 per channel (r, g, b) with one guard pixel on each side. `cur` holds what the previous
    // row pushed down; the next-row contributions are written once each from rolling registers, so `next` needs
    // no clearing.
    const size_t row = static_cast<size_t>(w + 2) * 3;
    std::vector<int32_t> err_a(row, 0), err_b(row, 0);
    int32_t* cur = err_a.data();
    int32_t* next = err_b.data();
    for (int y = y0; y < y1; ++y) {
        const uint8_t* src = bgra + static_cast<ptrdiff_t>(y) * stride;
        uint8_t* o = out + static_cast<ptrdiff_t>(y) * w;
        const bool ltr = (y & 1) == 0;
        if (skip) {
            const uint8_t* s = skip + static_cast<ptrdiff_t>(y) * w;
            if (ltr) DitherRow<true, 1>(src, o, s, cur, next, w, cache, skip_index);
            else DitherRow<true, -1>(src, o, s, cur, next, w, cache, skip_index);
        } else {
            if (ltr) DitherRow<false, 1>(src, o, nullptr, cur, next, w, cache, skip_index);
            else DitherRow<false, -1>(src, o, nullptr, cur, next, w, cache, skip_index);
        }
        std::swap(cur, next);
    }
}

} // namespace gif_detail

void GifEncoder::Quantize(const uint8_t* bgra, int w, int h, int stride, const std::vector<uint32_t>& palette, bool dither,
                          std::vector<uint8_t>& indices) {
    indices.assign(static_cast<size_t>(std::max(w, 0)) * static_cast<size_t>(std::max(h, 0)), 0);
    NearestCache cache;
    cache.Reset(palette.data(), static_cast<int>(palette.size()));
    QuantizeRegion(bgra, w, h, stride, cache, dither, nullptr, 0, indices.data());
}

} // namespace ct::exportng
