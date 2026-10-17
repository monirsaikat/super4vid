#pragma once
// ct/render/effects.h
// GPU image effects shared by the compositor passes: separable Gaussian blur, region pixelation, rounded-rect drop
// shadows (mask at 1/4 resolution -> blur -> composite, cached).
// Thread model: the render thread, with D3DDevice::Lock held.

#include <ct/project/types.h>
#include <ct/render/layer_math.h>
#include <ct/render/render_context.h>

#include <cstdint>
#include <vector>

namespace ct::render {

namespace effects {

// Blurs src (src_size) into dst (same size). Separable; while sigma > 8 px the image is first downsampled by 2 (sizes
// rounded up), so a pass never takes more than 25 taps, and the result is upsampled into dst. Leaves dst bound as the
// render target.
void GaussianBlur(RenderContext&, ID3D11ShaderResourceView* src, SizeI src_size, float sigma_px, RenderContext::TempRT& dst);

// Pixelates: src holds a copy of `region` (src_size == region size); draws it into dst at region with blocks of
// block_px (aligned to the region origin), a rounded mask of radius_px and opacity. dst_size = dst's size.
void Pixelate(RenderContext&, ID3D11ShaderResourceView* src, SizeI src_size, int block_px, RectF region,
              ID3D11RenderTargetView* dst, SizeI dst_size, float radius_px = 0, float opacity = 1);

// Drop shadow of the rounded rect `rect_px` (base coordinates) under `transform` (to dst pixels), drawn into dst with
// premultiplied blending. blur_px is the CSS-style blur (sigma = blur_px / 2), offset_y_px moves the shadow down; both
// scale with the transform. Masks are cached by (rect, radius, blur, transform, dst size): an unchanged layer costs
// only the composite pass. opacity_mul scales shadow.opacity (layer opacity). Leaves dst bound.
void RoundedShadow(RenderContext&, RectF rect_px, float radius_px, const project::Shadow&, Mat3 transform,
                   ID3D11RenderTargetView* dst, SizeI dst_size, float opacity_mul = 1.f);

// Number of shadow mask computations since Init (cache misses; tests check the cache).
uint64_t ShadowMaskRenders(RenderContext&);

} // namespace effects

} // namespace ct::render
