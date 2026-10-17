// Fullscreen triangle from SV_VertexID 0..2 (no vertex buffer, no input layout), and the textured quad vertex shader
// (4 vertices as a triangle strip, corners from the constant buffer). uv (0,0) is the top-left of the viewport.
#include "common.hlsli"

VsOut vs_main(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

QuadOut vs_quad(uint id : SV_VertexID) {
    QuadOut o;
    float4 c = g_corner[id];
    float2 ndc = c.xy / g_target_size * float2(2.0, -2.0) + float2(-1.0, 1.0);
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv = g_uv[id].xy;
    o.uv2 = g_uv[id].zw;
    o.local = c.zw;
    return o;
}
