// Export scaler: fullscreen triangle sampling the source with the linear sampler.
// A 2x downscale puts each destination pixel centre between 4 source texels, so one bilinear tap is the 2x2 box mean;
// the final pass (any ratio <= 2, or an upscale) is plain bilinear.
// The source may be a pooled temp larger than its used area (RenderContext buckets temps to 64 px): g_src_uv.xy =
// used / texture size, g_src_uv.zw = half a used texel (clamp like an exactly sized texture would).
cbuffer ScaleCb : register(b0) {
    float4 g_src_uv;
}
Texture2D<float4> g_src : register(t0);
SamplerState g_lin : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VsOut i) : SV_Target {
    float2 uv = clamp(i.uv, g_src_uv.zw, 1.0 - g_src_uv.zw) * g_src_uv.xy;
    return g_src.SampleLevel(g_lin, uv, 0);
}
