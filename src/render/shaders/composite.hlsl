// Composite passes (quads in target pixels).
#include "common.hlsli"

// Shadow: t0 = blurred coverage mask covering the whole target at reduced size; i.uv = mask uv of this pixel (already
// shifted by the shadow offset). Black * opacity (g_color.a) * coverage.
float4 ps_shadow(QuadOut i) : SV_Target {
    float m = g_tex0.SampleLevel(g_lin, i.uv, 0).a;
    float a = m * g_color.a * g_opacity;
    return Premul(g_color.rgb, a);
}

// Highlight region: t0 = copy of the region (premultiplied, opaque canvas); multiply by g_color.rgb with strength
// g_color.a, drawn back with the rounded mask.
float4 ps_multiply(QuadOut i) : SV_Target {
    float4 c = g_tex0.Sample(g_lin, SrcUv0(i.uv));
    float3 rgb = c.rgb * lerp(float3(1, 1, 1), g_color.rgb, g_color.a);
    float a = RoundedMask(i.local) * g_opacity;
    return float4(rgb, c.a) * a;
}
