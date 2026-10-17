// Shared by the NV12 converters: source, constants, fetch with rotation/crop/HDR, BT.709 limited-range math.
// The constants below must match RgbToNv12Ref() (color_convert.cpp).
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
cbuffer CB : register(b0) {
    float2 src_offset;           // region origin in source uv (0 when converting a whole texture)
    float2 src_scale;            // region size in source uv (1 when converting a whole texture)
    row_major float4x4 rot_uv;   // output uv -> region uv (rotation back to desktop orientation)
    float hdr_scale;             // 80 / sdr_white_nits
    float tonemap;               // > 0.5: soft roll-off above SDR white
    float2 dst_size;             // NV12 size in pixels (compute path)
}

float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(abs(c), 1.0 / 2.4) - 0.055; }

float3 Fetch(float2 uv) {
    float2 region_uv = mul(float4(uv, 0, 1), rot_uv).xy;
    float4 p = src.SampleLevel(lin, src_offset + region_uv * src_scale, 0);
    float3 c = p.rgb;
#ifdef CT_HDR_SOURCE
    // scRGB FP16 input: 1.0 = 80 nits linear.
    c = c * hdr_scale;                                         // SDR white becomes 1.0
    if (tonemap > 0.5) c = c / (1.0 + max(c - 1.0, 0.0));     // soft roll-off above SDR white
    c = LinearToSrgb(saturate(c));
#endif
    return saturate(c);
}

float Luma(float3 c) { return 0.1826 * c.r + 0.6142 * c.g + 0.0620 * c.b + 16.0 / 255.0; }

float2 Chroma(float3 c) {
    return float2(-0.1006 * c.r - 0.3386 * c.g + 0.4392 * c.b + 128.0 / 255.0,
                   0.4392 * c.r - 0.3989 * c.g - 0.0403 * c.b + 128.0 / 255.0);
}

