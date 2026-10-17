// BGRA8 (or scRGB FP16 with CT_HDR_SOURCE) -> NV12, BT.709 limited range. Two passes on one NV12 texture:
// ps_luma into the R8 view of plane 0 (W x H), ps_chroma into the R8G8 view of plane 1 (W/2 x H/2); the linear sampler
// at the 2x2 block centre averages the four source pixels. The constants below must match RgbToNv12Ref() (color_convert.cpp).
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
cbuffer CB : register(b0) {
    float2 inv_src_size;
    float2 pad;
    row_major float4x4 rot_uv;   // output uv -> source uv (rotation back to desktop orientation)
    float hdr_scale;             // 80 / sdr_white_nits
    float tonemap;               // > 0.5: soft roll-off above SDR white
    float2 pad2;
}

float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055; }

float3 Fetch(float2 uv) {
    float4 p = src.SampleLevel(lin, mul(float4(uv, 0, 1), rot_uv).xy, 0);
    float3 c = p.rgb;
#ifdef CT_HDR_SOURCE
    // scRGB FP16 input: 1.0 = 80 nits linear.
    c = c * hdr_scale;                                         // SDR white becomes 1.0
    if (tonemap > 0.5) c = c / (1.0 + max(c - 1.0, 0.0));     // soft roll-off above SDR white
    c = LinearToSrgb(saturate(c));
#endif
    return saturate(c);
}

float ps_luma(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float3 c = Fetch(uv);
    return 0.1826 * c.r + 0.6142 * c.g + 0.0620 * c.b + 16.0 / 255.0;
}

float2 ps_chroma(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float3 c = Fetch(uv);   // uv already at the 2x2 centre
    return float2(-0.1006 * c.r - 0.3386 * c.g + 0.4392 * c.b + 128.0 / 255.0,
                   0.4392 * c.r - 0.3989 * c.g - 0.0403 * c.b + 128.0 / 255.0);
}
