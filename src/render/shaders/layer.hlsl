// Video/image layer pixel shaders: NV12 (BT.709 limited range) or RGBA, rounded-rect mask, opacity, premultiplied out.
#include "common.hlsli"

// BT.709 limited range -> sRGB-encoded RGB. Chroma is centred at 128/255, matching ColorConverter (capture).
float3 Nv12ToRgb(float y, float2 uv) {
    float yy = 1.1644 * (y - 16.0 / 255.0);
    float u = uv.x - 128.0 / 255.0;
    float v = uv.y - 128.0 / 255.0;
    return saturate(float3(yy + 1.7927 * v, yy - 0.2133 * u - 0.5329 * v, yy + 2.1124 * u));
}

// t0 = R8 view of the luma plane, t1 = R8G8 view of the chroma plane.
float4 ps_nv12(QuadOut i) : SV_Target {
    float y = g_tex0.Sample(g_lin, i.uv).r;
    float2 c = g_tex1.Sample(g_lin, i.uv).rg;
    float a = RoundedMask(i.local) * g_opacity;
    return Premul(Nv12ToRgb(y, c), a);
}

// t0 = RGBA (straight unless g_premul).
float4 ps_rgba(QuadOut i) : SV_Target {
    float4 c = g_tex0.Sample(g_lin, i.uv);
    if (g_premul < 0.5) c.rgb *= c.a;
    float a = RoundedMask(i.local) * g_opacity;
    return c * a;
}

// Solid colour (straight alpha in g_color) with the rounded mask.
float4 ps_solid(QuadOut i) : SV_Target {
    float a = RoundedMask(i.local) * g_opacity * g_color.a;
    return Premul(g_color.rgb, a);
}
