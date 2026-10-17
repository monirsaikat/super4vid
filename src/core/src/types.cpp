#include <ct/core/types.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ct {

bool RectI::Contains(PointI p) const {
    return p.x >= x && p.x < Right() && p.y >= y && p.y < Bottom();
}

RectI RectI::Intersect(const RectI& o) const {
    const int32_t l = std::max(x, o.x);
    const int32_t t = std::max(y, o.y);
    const int32_t r = std::min(Right(), o.Right());
    const int32_t b = std::min(Bottom(), o.Bottom());
    if (r <= l || b <= t) return RectI{};
    return RectI{l, t, r - l, b - t};
}

namespace {

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint8_t ToByte(float v) {
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

} // namespace

// Accepts "#RRGGBB", "#RRGGBBAA" and the same without '#'. Malformed input yields opaque black.
ColorF ColorF::FromHex(std::string_view hex) {
    if (!hex.empty() && hex.front() == '#') hex.remove_prefix(1);
    if (hex.size() != 6 && hex.size() != 8) return ColorF{};
    float channels[4] = {0, 0, 0, 1};
    for (size_t i = 0; i < hex.size() / 2; ++i) {
        const int hi = HexDigit(hex[i * 2]);
        const int lo = HexDigit(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return ColorF{};
        channels[i] = static_cast<float>(hi * 16 + lo) / 255.0f;
    }
    return ColorF{channels[0], channels[1], channels[2], channels[3]};
}

// "#RRGGBB" when fully opaque, otherwise "#RRGGBBAA". Upper-case digits.
std::string ColorF::ToHex() const {
    char buf[10];
    const uint8_t alpha = ToByte(a);
    if (alpha == 255)
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", ToByte(r), ToByte(g), ToByte(b));
    else
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", ToByte(r), ToByte(g), ToByte(b), alpha);
    return std::string(buf);
}

} // namespace ct
