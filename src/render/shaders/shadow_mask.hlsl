// Rounded-rect coverage mask (white, premultiplied) drawn at reduced resolution before blurring into a shadow.
#include "common.hlsli"

float4 ps_main(QuadOut i) : SV_Target {
    float a = saturate(0.5 - RoundedRectSdf(i.local, g_half, g_radius));
    return float4(a, a, a, a);
}
