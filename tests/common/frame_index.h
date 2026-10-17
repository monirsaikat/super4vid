#pragma once
// tests/common/frame_index.h
// The 7-segment frame counter that the test pattern generator burns into every frame, and its reader.
// Layout: exactly what tools/gen_test_media (P04, the one canonical generator) draws, in absolute pixels:
//   - box (32,32)-(432,96) filled black, drawn after the background and after the every-60th-frame flash;
//   - 12 decimal digits, most significant first, leading zeros drawn; digit k's glyph cell is 28 x 56 px with its
//     top-left at (34 + 33 * k, 36); segments are white bars 4 px thick (rects below, relative to the cell).
// The reader samples a 3x3 luma patch at the centre of every segment, so chroma subsampling and H.264 ringing
// do not matter.
// The one canonical counter reader (P10's tests/common/pattern_counter.h samples the same points and should become
// a thin include of this file). Header-only; no dependencies beyond the standard library.

#include <cstdint>

namespace ct::test {

struct SegRect { int x, y, w, h; };

struct FrameCounterLayout {
    static constexpr int kDigits = 12;
    static constexpr int kBoxX = 32, kBoxY = 32, kBoxW = 400, kBoxH = 64;
    static constexpr int kCellX0 = 34, kCellW = 33, kCellY = 36;

    static constexpr int CellX(int digit) { return kCellX0 + kCellW * digit; }

    // Segment s in a..g order (0 = a top, 1 = b top right, 2 = c bottom right, 3 = d bottom, 4 = e bottom left,
    // 5 = f top left, 6 = g middle) as {x, y, w, h}; mirrors kSeg in tools/gen_test_media/main.cpp.
    static constexpr SegRect Segment(int digit, int s) {
        constexpr SegRect kSeg[7] = {{4, 0, 20, 4},  {24, 4, 4, 22}, {24, 30, 4, 22}, {4, 52, 20, 4},
                                     {0, 30, 4, 22}, {0, 4, 4, 22},  {4, 26, 20, 4}};
        const SegRect r = kSeg[s < 0 || s > 6 ? 6 : s];
        return {CellX(digit) + r.x, kCellY + r.y, r.w, r.h};
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

// RGBA (or BGRA) readback, 4 bytes per pixel: brightness = (r + 2g + b) / 4. (ox, oy) = where the frame's top-left
// lands in the image (e.g. a composited canvas).
inline int64_t ReadFrameIndex(const uint8_t* rgba, int width, int height, int stride, int ox = 0, int oy = 0) {
    if (!rgba || ox < 0 || oy < 0 || width < ox + FrameCounterLayout::kBoxX + FrameCounterLayout::kBoxW ||
        height < oy + FrameCounterLayout::kBoxY + FrameCounterLayout::kBoxH)
        return -1;
    return ReadFrameIndexWith([&](int x, int yy) {
        const uint8_t* p = rgba + static_cast<size_t>(yy + oy) * stride + static_cast<size_t>(x + ox) * 4;
        return (p[0] + 2 * p[1] + p[2]) / 4;
    });
}

} // namespace ct::test
