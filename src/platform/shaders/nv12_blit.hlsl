// NV12 (BT.709 limited range) -> RGBA. A full-screen triangle samples the Y (R8) and UV (R8G8) planes of an NV12
// texture through separate views. ps_main: one bilinear sample per output pixel (preview, upscale, mild downscale).
// ps_box: averages taps x taps bilinear samples spread over the output pixel's footprint (thumbnails, big downscales).
// The inverse of the forward matrix in src/capture/shaders/nv12_common.hlsli (Kr 0.2126, Kb 0.0722).

Texture2D<float> plane_y : register(t0);
Texture2D<float2> plane_uv : register(t1);
SamplerState lin : register(s0);

cbuffer CB : register(b0) {
    float2 dst_texel_uv;   // one output pixel in source uv
    float taps;            // ps_box: samples per axis (>= 1)
    float alpha;           // output alpha
}

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint id : SV_VertexID) {
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 YuvToRgb(float y, float2 c) {
    float yl = (y - 16.0 / 255.0) * (255.0 / 219.0);
    float2 cc = (c - 128.0 / 255.0) * (255.0 / 224.0);
    float3 rgb;
    rgb.r = yl + 1.5748 * cc.y;
    rgb.g = yl - 0.187324 * cc.x - 0.468124 * cc.y;
    rgb.b = yl + 1.8556 * cc.x;
    return saturate(rgb);
}

float4 ps_main(VsOut i) : SV_Target {
    return float4(YuvToRgb(plane_y.Sample(lin, i.uv), plane_uv.Sample(lin, i.uv)), alpha);
}

float4 ps_box(VsOut i) : SV_Target {
    int n = max((int)taps, 1);
    float y = 0;
    float2 c = 0;
    [loop] for (int ky = 0; ky < n; ++ky) {
        [loop] for (int kx = 0; kx < n; ++kx) {
            float2 off = (float2(kx, ky) + 0.5) / n - 0.5;
            float2 uv = i.uv + off * dst_texel_uv;
            y += plane_y.Sample(lin, uv);
            c += plane_uv.Sample(lin, uv);
        }
    }
    float inv = 1.0 / (n * n);
    return float4(YuvToRgb(y * inv, c * inv), alpha);
}
