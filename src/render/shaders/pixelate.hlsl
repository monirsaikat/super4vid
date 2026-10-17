// Pixelate a region (quad): every block of g_p0.xy (uv units of t0) takes the average of 4 bilinear taps at its
// quarter points (approximately the block's mean, so thin lines do not decide a whole block).
// t0 holds a copy of the region (premultiplied). Drawn back with the rounded mask and opacity.
#include "common.hlsli"

float4 ps_main(QuadOut i) : SV_Target {
    float2 block = max(g_p0.xy, float2(1e-6, 1e-6));
    float2 uv = (floor(i.uv / block) + 0.5) * block;
    float2 q = block * 0.25;
    float4 c = (g_tex0.SampleLevel(g_lin, saturate(uv + float2(-q.x, -q.y)), 0) +
                g_tex0.SampleLevel(g_lin, saturate(uv + float2(q.x, -q.y)), 0) +
                g_tex0.SampleLevel(g_lin, saturate(uv + float2(-q.x, q.y)), 0) +
                g_tex0.SampleLevel(g_lin, saturate(uv + float2(q.x, q.y)), 0)) * 0.25;
    float a = RoundedMask(i.local) * g_opacity;
    return c * a;
}
