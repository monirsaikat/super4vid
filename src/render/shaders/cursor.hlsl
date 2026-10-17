// Cursor overlay shaders (quads): click ripple ring, held-button disc, spotlight and the sprite drop shadow.
// The sprite itself uses layer.hlsl ps_rgba with premultiplied atlas texels.
#include "common.hlsli"

// Ring centred at local (0,0): g_p0.x = radius, g_p0.y = thickness (target px). Colour g_color, alpha * g_opacity.
float4 ps_ring(QuadOut i) : SV_Target {
    float d = abs(length(i.local) - g_p0.x) - g_p0.y * 0.5;
    float a = saturate(0.5 - d) * g_color.a * g_opacity;
    return Premul(g_color.rgb, a);
}

// Filled disc centred at local (0,0) with radius g_p0.x.
float4 ps_disc(QuadOut i) : SV_Target {
    float d = length(i.local) - g_p0.x;
    float a = saturate(0.5 - d) * g_color.a * g_opacity;
    return Premul(g_color.rgb, a);
}

// Darkens everything outside a soft circle centred at local (0,0): g_p0.x = inner radius, g_p0.y = soft edge width,
// g_p0.z = darkness (alpha of black outside).
float4 ps_spotlight(QuadOut i) : SV_Target {
    float d = length(i.local);
    float a = smoothstep(g_p0.x, g_p0.x + g_p0.y, d) * g_p0.z * g_opacity;
    return float4(0, 0, 0, a);
}

// Drop shadow of the sprite: average atlas alpha of 4 taps around uv (g_p0.xy = tap offset in uv), black * g_color.a.
float4 ps_shadow(QuadOut i) : SV_Target {
    float2 o = g_p0.xy;
    float a = g_tex0.Sample(g_lin, i.uv + float2(-o.x, -o.y)).a + g_tex0.Sample(g_lin, i.uv + float2(o.x, -o.y)).a +
              g_tex0.Sample(g_lin, i.uv + float2(-o.x, o.y)).a + g_tex0.Sample(g_lin, i.uv + float2(o.x, o.y)).a;
    // g_p1 = the shape's cell in atlas uv (x0, y0, x1, y1): pixels outside it get no shadow, so neighbours never bleed in.
    float2 inside = step(g_p1.xy, i.uv) * step(i.uv, g_p1.zw);
    a = a * 0.25 * g_color.a * g_opacity * inside.x * inside.y;
    return float4(0, 0, 0, a);
}
