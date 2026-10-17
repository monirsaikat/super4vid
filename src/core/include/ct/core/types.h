#pragma once
// ct/core/types.h
// Small value types shared by every module: integer points/sizes/rects, float rects, colors, microsecond time.
// Thread model: plain values, no shared state.

#include <cstdint>
#include <string>
#include <string_view>

namespace ct {

struct PointI { int32_t x = 0, y = 0; };
struct SizeI  { int32_t w = 0, h = 0; bool IsEmpty() const { return w <= 0 || h <= 0; } };
struct RectI  { int32_t x = 0, y = 0, w = 0, h = 0;
                int32_t Right() const { return x + w; } int32_t Bottom() const { return y + h; }
                bool Contains(PointI p) const; RectI Intersect(const RectI& o) const; bool IsEmpty() const { return w <= 0 || h <= 0; } };
struct RectF  { float x = 0, y = 0, w = 0, h = 0; };
struct ColorF { float r = 0, g = 0, b = 0, a = 1; static ColorF FromHex(std::string_view hex /* "#RRGGBB" or "#RRGGBBAA" */); std::string ToHex() const; };

using TimeUs = int64_t;
constexpr TimeUs kUsPerSecond = 1'000'000;

} // namespace ct
