// Compute-shader BGRA8 (or scRGB FP16 with CT_HDR_SOURCE) -> NV12 converter; see bgra_to_nv12.hlsl for the passes it
// replaces. Needs typed UAV stores on NV12 (R8 view of plane 0, R8G8 view of plane 1).
#include "nv12_common.hlsli"

RWTexture2D<unorm float> out_luma : register(u0);
RWTexture2D<unorm float2> out_chroma : register(u1);

// One thread per 2x2 block: four luma pixels and one chroma pixel. (A 16x16-tile variant with one luma store per thread
// and group-shared chroma averaging was not faster on the reference iGPU.)
[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    const float2 half_size = dst_size * 0.5;
    if (id.x >= (uint)half_size.x || id.y >= (uint)half_size.y) return;
    const float2 inv = 1.0 / dst_size;
    const uint2 p = id.xy * 2;
    float3 sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k) {
        const uint2 q = p + uint2(k & 1, k >> 1);
        const float3 c = Fetch((float2(q) + 0.5) * inv);
        out_luma[q] = Luma(c);
        sum += c;
    }
#ifdef CT_HDR_SOURCE
    // Average in linear light like the pixel-shader path's bilinear tap at the block centre (before tone-mapping).
    out_chroma[id.xy] = Chroma(Fetch((float2(p) + 1.0) * inv));
#else
    out_chroma[id.xy] = Chroma(sum * 0.25);   // == the bilinear tap at the 2x2 block centre
#endif
}
