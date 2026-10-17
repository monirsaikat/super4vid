// Shared by every compositor shader: the one constant buffer layout (QuadCb in render_internal.h), textures, samplers,
// the quad vertex shader and the rounded-rect SDF mask. All colours are sRGB-encoded and premultiplied on output.

cbuffer QuadCb : register(b0) {
    float4 g_corner[4];      // xy: target pixels; zw: local coordinates (rect-centred, target-pixel units). TL TR BL BR
    float4 g_uv[4];          // xy: texture uv; zw: second uv set
    float2 g_target_size;    // render target size in pixels
    float2 g_half;           // rounded-rect half extent in local units
    float g_radius;          // corner radius in local units
    float g_opacity;
    float g_mask;            // > 0.5: apply the rounded-rect mask
    float g_premul;          // > 0.5: g_tex0 holds premultiplied colour
    float4 g_color;          // straight-alpha colour (solid fills, tints, dip colour)
    float4 g_p0;             // shader-specific parameters
    float4 g_p1;
}

Texture2D<float4> g_tex0 : register(t0);
Texture2D<float4> g_tex1 : register(t1);
SamplerState g_lin : register(s0);
SamplerState g_pt : register(s1);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

struct QuadOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 local : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
};

// Signed distance to a rounded rectangle centred at the origin (negative inside), in local units.
float RoundedRectSdf(float2 p, float2 half_size, float radius) {
    float r = min(radius, min(half_size.x, half_size.y));
    float2 q = abs(p) - half_size + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Coverage with a 1 px analytic edge: local units are target pixels, so the edge stays 1 px wide under any zoom.
float RoundedMask(float2 local) {
    float m = saturate(0.5 - RoundedRectSdf(local, g_half, g_radius));
    return g_mask < 0.5 ? 1.0 : m;
}

float4 Premul(float3 rgb, float a) { return float4(rgb * a, a); }
