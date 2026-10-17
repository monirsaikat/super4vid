// BGRA8 (or scRGB FP16 with CT_HDR_SOURCE) -> NV12, BT.709 limited range.
// Pixel-shader path, two passes on one NV12 texture: ps_luma into the R8 view of plane 0 (W x H), ps_chroma into the
// R8G8 view of plane 1 (W/2 x H/2); the linear sampler at the 2x2 block centre averages the four source pixels (also
// inside a cropped region of a larger source).
// Compute path (bgra_to_nv12_cs.hlsl, one thread per 2x2 block): the same samples, written through R8 / R8G8 UAVs on the
// two planes, so no render-target passes run. Identical output to the pixel-shader path.
#include "nv12_common.hlsli"

float ps_luma(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target { return Luma(Fetch(uv)); }

float2 ps_chroma(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    return Chroma(Fetch(uv));   // uv already at the 2x2 centre
}
