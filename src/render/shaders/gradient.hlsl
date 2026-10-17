// Linear background gradient (CSS semantics: 0 deg points up, 90 deg right, 135 deg to the bottom-right), lerped in
// sRGB-encoded space. g_p0.xy = unit direction (y down), g_p0.z = gradient line length in pixels.
// g_color = colour A (start), g_p1 = colour B (end).
#include "common.hlsli"

float4 ps_main(VsOut i) : SV_Target {
    float2 p = i.pos.xy - g_target_size * 0.5;
    float t = saturate(dot(p, g_p0.xy) / g_p0.z + 0.5);
    return float4(lerp(g_color.rgb, g_p1.rgb, t), 1.0);
}
