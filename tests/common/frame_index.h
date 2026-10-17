#pragma once
// tests/common/frame_index.h
// The 7-segment frame counter that the test pattern generator burns into every frame, and its reader.
// Layout (shared contract with tools/gen_test_media from P04, see DECISIONS.md P06 6.2), in absolute pixels:
//   - box (32,32)-(432,96) filled black (Y = 16), drawn after the background and after the every-60th-frame flash;
//   - 12 decimal digits, most significant first, leading zeros drawn; digit k occupies the cell
//     x = 34 + 33 * k .. +33; the glyph is 24 x 48 px at (cell + 4, 40); segments are white (Y = 235) bars 6 px thick.
// The reader samples a 3x3 patch at the centre of every segment, so chroma subsampling and H.264 ringing do not matter.
// Header-only; no dependencies beyond the standard library.

#include <cstdint>

namespace ct::test {

struct SegRect { int x, y, w, h; };

struct FrameCounterLayout {
    static constexpr int kDigits = 12;
    static constexpr int kBoxX = 32, kBoxY = 32, kBoxW = 400, kBoxH = 64;
    static constexpr int kCellX0 = 34, kCellW = 33;
    static constexpr int kGlyphDx = 4, kGlyphY = 40, kGlyphW = 24, kGlyphH = 48, kThick = 6;

    static constexpr int GlyphX(int digit) { return kCellX0 + kCellW * digit + kGlyphDx; }

    // Segment s in a..g order (0 = a top, 1 = b top right, 2 = c bottom right, 3 = d bottom, 4 = e bottom left,
    // 5 = f top left, 6 = g middle).
    static constexpr SegRect Segment(int digit, int s) {
        const int gx = GlyphX(digit), gy = kGlyphY, gw = kGlyphW, gh = kGlyphH, t = kThick;
        switch (s) {
        case 0: return {gx, gy, gw, t};
        case 1: return {gx + gw - t, gy, t, gh / 2};
        case 2: return {gx + gw - t, gy + gh / 2, t, gh / 2};
        case 3: return {gx, gy + gh - t, gw, t};
        case 4: return {gx, gy + gh / 2, t, gh / 2};
        case 5: return {gx, gy, t, gh / 2};
        default: return {gx, gy + gh / 2 - t / 2, gw, t};
        }
    }
    // Sample point: the middle of the segment's long axis, away from the corner overlaps.
    static constexpr void SamplePoint(int digit, int s, int& x, int& y) {
        const SegRect r = Segment(digit, s);
        x = r.x + r.w / 2;
        y = r.y + r.h / 2;
    }
    // Bit s set = segment s lit, for digits 0..9.
    static constexpr uint8_t Mask(int d) {
        constexpr uint8_t kMasks[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
        return kMasks[d];
    }
};

// Reads the counter from any 8-bit image where `luma(x, y)` returns 0..255 brightness.
template <typename LumaFn>
inline int64_t ReadFrameIndexWith(LumaFn luma) {
    int64_t value = 0;
    for (int d = 0; d < FrameCounterLayout::kDigits; ++d) {
        uint8_t mask = 0;
        for (int s = 0; s < 7; ++s) {
            int cx = 0, cy = 0;
            FrameCounterLayout::SamplePoint(d, s, cx, cy);
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) sum += luma(cx + dx, cy + dy);
            if (sum / 9 >= 128) mask = static_cast<uint8_t>(mask | (1u << s));
        }
        int digit = -1;
        for (int k = 0; k < 10; ++k)
            if (FrameCounterLayout::Mask(k) == mask) digit = k;
        if (digit < 0) return -1;
        value = value * 10 + digit;
    }
    return value;
}

// NV12 / any planar luma: `y` points at the luma plane, `pitch` bytes per row. Returns -1 when unreadable.
inline int64_t ReadFrameIndexY(const uint8_t* y, int width, int height, int pitch) {
    if (!y || width < FrameCounterLayout::kBoxX + FrameCounterLayout::kBoxW || height < FrameCounterLayout::kBoxY + FrameCounterLayout::kBoxH)
        return -1;
    return ReadFrameIndexWith([&](int x, int yy) { return static_cast<int>(y[static_cast<size_t>(yy) * pitch + x]); });
}

// RGBA (or BGRA) readback, 4 bytes per pixel: brightness = (r + 2g + b) / 4.
inline int64_t ReadFrameIndex(const uint8_t* rgba, int width, int height, int stride) {
    if (!rgba || width < FrameCounterLayout::kBoxX + FrameCounterLayout::kBoxW || height < FrameCounterLayout::kBoxY + FrameCounterLayout::kBoxH)
        return -1;
    return ReadFrameIndexWith([&](int x, int yy) {
        const uint8_t* p = rgba + static_cast<size_t>(yy) * stride + static_cast<size_t>(x) * 4;
        return (p[0] + 2 * p[1] + p[2]) / 4;
    });
}

// Draws the counter into a luma plane (for generators): box black (16), lit segments white (235).
inline void DrawFrameIndexY(uint8_t* y, int pitch, int64_t index) {
    using L = FrameCounterLayout;
    for (int row = L::kBoxY; row < L::kBoxY + L::kBoxH; ++row)
        for (int x = L::kBoxX; x < L::kBoxX + L::kBoxW; ++x) y[static_cast<size_t>(row) * pitch + x] = 16;
    int64_t v = index < 0 ? 0 : index;
    for (int d = L::kDigits - 1; d >= 0; --d) {
        const int digit = static_cast<int>(v % 10);
        v /= 10;
        for (int s = 0; s < 7; ++s) {
            if (!(L::Mask(digit) & (1u << s))) continue;
            const SegRect r = L::Segment(d, s);
            for (int row = r.y; row < r.y + r.h; ++row)
                for (int x = r.x; x < r.x + r.w; ++x) y[static_cast<size_t>(row) * pitch + x] = 235;
        }
    }
}

} // namespace ct::test
