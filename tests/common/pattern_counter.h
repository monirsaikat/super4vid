#pragma once
// tests/common/pattern_counter.h
// Reader for the frame counter of the committed tests/data/pattern_*.mp4 files (tools/gen_test_media layout, see
// tests/data/README.md): digit d at cell (34 + 33 d, 36), 28 x 56 px, segment centres sampled from luma > 128.
// (tests/common/frame_index.h describes the P06 generator's layout, which the committed files do not use.)

#include <cstdint>

namespace ct::test {

// Luma lookup `luma(x, y)` -> 0..255. Returns the frame number, or -1 when a digit is unreadable.
template <typename LumaFn>
inline int64_t ReadPatternCounterWith(LumaFn luma) {
    static constexpr int kSeg[7][2] = {{14, 2}, {26, 15}, {26, 41}, {14, 54}, {2, 41}, {2, 15}, {14, 28}};
    static constexpr uint8_t kMask[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    int64_t v = 0;
    for (int d = 0; d < 12; ++d) {
        const int x0 = 34 + 33 * d, y0 = 36;
        uint8_t bits = 0;
        for (int s = 0; s < 7; ++s)
            if (luma(x0 + kSeg[s][0], y0 + kSeg[s][1]) > 128) bits = static_cast<uint8_t>(bits | (1u << s));
        int digit = -1;
        for (int k = 0; k < 10; ++k)
            if (kMask[k] == bits) digit = k;
        if (digit < 0) return -1;
        v = v * 10 + digit;
    }
    return v;
}

inline int64_t ReadPatternCounterY(const uint8_t* y, int width, int height, int pitch) {
    if (!y || width < 440 || height < 100) return -1;
    return ReadPatternCounterWith([&](int x, int yy) { return static_cast<int>(y[static_cast<size_t>(yy) * pitch + x]); });
}

// BGRA/RGBA 8-bit, 4 bytes per pixel; brightness (r + 2g + b) / 4. (ox, oy) = where the frame's top-left lands.
inline int64_t ReadPatternCounterRgba(const uint8_t* px, int width, int height, int stride, int ox = 0, int oy = 0) {
    if (!px || width < ox + 440 || height < oy + 100) return -1;
    return ReadPatternCounterWith([&](int x, int yy) {
        const uint8_t* p = px + static_cast<size_t>(yy + oy) * stride + static_cast<size_t>(x + ox) * 4;
        return (p[0] + 2 * p[1] + p[2]) / 4;
    });
}

} // namespace ct::test
