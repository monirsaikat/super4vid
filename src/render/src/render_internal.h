#pragma once
// Internal to ct_render: the pipeline objects owned by RenderContext, the one constant-buffer layout every shader uses
// (cbuffer QuadCb in shaders/common.hlsli) and the draw helpers shared by the passes.

#include <ct/render/layer_math.h>
#include <ct/render/render_context.h>

#include <wrl/client.h>

namespace ct::render {

struct QuadCb {
    float corner[4][4];   // xy target px, zw local (TL, TR, BL, BR)
    float uv[4][4];       // xy uv, zw second uv
    float target_size[2];
    float half[2];
    float radius;
    float opacity;
    float mask;
    float premul;
    float color[4];
    float p0[4];
    float p1[4];
};
static_assert(sizeof(QuadCb) == 208, "must match cbuffer QuadCb in shaders/common.hlsli");

enum class Ps : int {
    Nv12, Rgba, Solid,             // layer.hlsl
    Gradient,                      // gradient.hlsl
    Blur, Copy,                    // blur.hlsl
    Pixelate,                      // pixelate.hlsl
    Ring, Disc, Spotlight, CursorShadow,   // cursor.hlsl
    ShadowMask,                    // shadow_mask.hlsl
    ShadowComposite, Multiply,     // composite.hlsl
    Transition,                    // transition.hlsl
    Count
};

// Blurred shadow masks of recent frames, keyed by everything that changes their pixels.
struct ShadowCache {
    static constexpr int kEntries = 8;
    static constexpr int kKeyFloats = 14;
    struct Entry {
        float key[kKeyFloats] = {};
        RenderContext::TempRT tex;   // persistent (not from the temp pool)
        uint64_t last_use = 0;
        bool valid = false;
    };
    Entry e[kEntries];
    uint64_t tick = 0;
    uint64_t renders = 0;
};

struct Pipelines {
    ShadowCache shadows;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_full, vs_quad;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps[static_cast<int>(Ps::Count)];
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb;   // QuadCb, dynamic, Map(DISCARD) per draw
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_premul, blend_opaque;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> lin, pt;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster;
    SizeI target_size;   // of the render target bound by SetFullscreen
};

// Corners of a quad and the attributes at each corner (TL, TR, BL, BR).
struct QuadGeom {
    PointF pos[4];
    PointF local[4];
    PointF uv[4];
};

// Builds the quad of `rect` (base coordinates) under similarity transform `m`, expanded by `expand_px` target pixels on
// every side so the 1 px mask edge is not clipped. uv_corners: texture uv at the rect's TL, TR, BL, BR (extrapolated
// for the expansion). local coordinates are rect-centred, in target pixels. Sets cb.half from the rect and the scale.
void BuildRectQuad(QuadCb& cb, const Mat3& m, const RectF& rect, float expand_px, const PointF uv_corners[4]);
// Axis-aligned target-pixel rect with uv = position / uv_space (uv_space 0 -> uv 0..1 across the rect); local = rect-centred.
void BuildScreenQuad(QuadCb& cb, const RectF& px, SizeI uv_space);
void SetColor(float out[4], const ColorF& c);
void ZeroCb(QuadCb& cb);

// Uploads cb (target_size filled from the bound target), binds t0/t1 and draws: a 4-vertex quad (vs_quad) or the
// fullscreen triangle (vs_full).
void DrawQuad(RenderContext&, Ps, QuadCb& cb, ID3D11ShaderResourceView* t0 = nullptr, ID3D11ShaderResourceView* t1 = nullptr);
void DrawFull(RenderContext&, Ps, QuadCb& cb, ID3D11ShaderResourceView* t0 = nullptr, ID3D11ShaderResourceView* t1 = nullptr);

// Uv corners of the whole texture (no rotation).
void FullUv(PointF out[4]);

} // namespace ct::render
