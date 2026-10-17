#pragma once
// ct/render/transitions.h
// Transition composites between two premultiplied layer images (crossfade, dip to colour, slide).
// Thread model: the render thread, with D3DDevice::Lock held.

#include <ct/project/types.h>
#include <ct/render/render_context.h>

namespace ct::render {

namespace transitions {
// Composites the transition from a (outgoing) to b (incoming) at progress u into dst (premultiplied over what is there).
// a and b are premultiplied images of dst's size; either may be null (transparent). Crossfade lerps; Dip goes through
// transition.color (opaque) at u = 0.5; Slide moves both images in transition.dir.
void Apply(RenderContext&, const project::Transition&, float u, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b,
           ID3D11RenderTargetView* dst, SizeI);
} // namespace transitions

} // namespace ct::render
