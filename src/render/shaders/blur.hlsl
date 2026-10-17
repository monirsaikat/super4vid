// Separable Gaussian blur pass and a 2x box downsample (fullscreen triangle).
// ps_blur: g_p0.xy = one texel step along the blur direction (uv), g_p0.z = sigma (texels), g_p0.w = radius (taps).
#include "common.hlsli"

float4 ps_blur(VsOut i) : SV_Target {
    float sigma = max(g_p0.z, 0.001);
    int r = (int)g_p0.w;
    float k2 = -0.5 / (sigma * sigma);
    float4 acc = 0;
    float wsum = 0;
    [loop] for (int k = -r; k <= r; ++k) {
        float w = exp(k2 * (float)(k * k));
        acc += w * g_tex0.SampleLevel(g_lin, i.uv + g_p0.xy * (float)k, 0);
        wsum += w;
    }
    return acc / wsum;
}

// 2x downsample: the destination pixel centre lies between 4 source texels, so one bilinear tap averages them.
float4 ps_copy(VsOut i) : SV_Target {
    return g_tex0.SampleLevel(g_lin, i.uv, 0);
}
