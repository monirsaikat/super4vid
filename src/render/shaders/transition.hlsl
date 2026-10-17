// Transitions between two premultiplied layer images (fullscreen): t0 = outgoing clip A, t1 = incoming clip B.
// g_p0.x = progress u (0..1), g_p0.y = kind (0 crossfade, 1 dip to colour, 2 slide), g_p0.zw = slide direction
// (unit vector of the motion in uv). g_color = dip colour.
#include "common.hlsli"

float4 A(float2 uv) { return g_tex0.SampleLevel(g_lin, SrcUv0(uv), 0); }
float4 B(float2 uv) { return g_tex1.SampleLevel(g_lin, SrcUv1(uv), 0); }
float Inside(float2 uv) { return (any(uv < 0.0) || any(uv > 1.0)) ? 0.0 : 1.0; }

float4 ps_main(VsOut i) : SV_Target {
    float u = saturate(g_p0.x);
    int kind = (int)(g_p0.y + 0.5);
    if (kind == 1) {
        float4 c = float4(g_color.rgb, 1.0);
        if (u < 0.5) return lerp(A(i.uv), c, u * 2.0);
        return lerp(c, B(i.uv), u * 2.0 - 1.0);
    }
    if (kind == 2) {
        float2 dir = g_p0.zw;
        float2 ua = i.uv - dir * u, ub = i.uv - dir * (u - 1.0);
        float4 a = A(ua) * Inside(ua);
        float4 b = B(ub) * Inside(ub);
        return b + a * (1.0 - b.a);
    }
    return lerp(A(i.uv), B(i.uv), u);
}
