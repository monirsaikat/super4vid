#pragma once
// ct/export/scaler.h
// GPU resize of a rendered canvas to the export size: repeated 2x box passes (one bilinear tap between 4 texels) while
// the image is at least twice the target in both directions, then one bilinear pass to the exact size (also used for
// upscales). A single bilinear pass from 4K to 720p would alias (docs/phases/P10 pitfalls).
// Thread model: the render thread with D3DDevice::Lock held (like Compositor::RenderFrame). Pipeline objects are created
// on first use per device and kept for the process. Intermediate targets come from RenderContext's temp pool.

#include <ct/core/types.h>
#include <ct/render/render_context.h>

#include <d3d11.h>

namespace ct::exportng {

struct Scaler {
    // src: B8G8R8A8 (or any float4-sampled) view of a src-sized texture; dst: B8G8R8A8 RTV of dst_size.
    static HRESULT Downscale(render::RenderContext&, ID3D11ShaderResourceView* src, SizeI src_size, ID3D11RenderTargetView* dst,
                             SizeI dst_size);
    // Number of 2x box passes Downscale uses for these sizes (tests).
    static int BoxPasses(SizeI src_size, SizeI dst_size);
};

} // namespace ct::exportng
