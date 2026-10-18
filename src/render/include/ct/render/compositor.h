#pragma once
// ct/render/compositor.h
// RenderFrame(project, t, inputs) -> canvas pixels on the GPU: background, video layers (fit, transform, crop, rounded
// corners, shadows, camera zoom/pan, PIP masks, transitions), the re-rendered cursor, keystroke badges and annotations.
// The single renderer behind both preview (P09) and export (P10): identical inputs give identical pixels.
// Colour policy: all render targets are B8G8R8A8_UNORM holding sRGB-encoded values; blending happens in gamma space
// with premultiplied alpha. NV12 inputs are BT.709 limited range.
// Thread model: one render thread; the caller holds D3DDevice::Lock for the whole RenderFrame. RenderFrame also holds
// D3DDevice::Batch (the device's multithread lock) for its duration when RenderOptions::gpu_timing is set.
// Lifetime: Init(RenderContext&) after RenderContext::Init; Shutdown (or destructor) before the context goes away.

#include <ct/project/project.h>
#include <ct/render/cursor_renderer.h>
#include <ct/render/frame_inputs.h>
#include <ct/render/layer_math.h>
#include <ct/render/render_context.h>
#include <ct/render/text_renderer.h>

#include <string>
#include <utility>
#include <vector>

namespace ct::render {

class Compositor {
public:
    Compositor() = default;
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    HRESULT Init(RenderContext&);
    void Shutdown();

    // target: B8G8R8A8 RTV of exactly the canvas size (E_INVALIDARG otherwise). Deterministic for identical inputs.
    // Leaves no render target or shader resource bound.
    HRESULT RenderFrame(const project::Project&, TimeUs t_us, const FrameInputs&, ID3D11RenderTargetView* target,
                        const RenderOptions& = {});

    // For UI gizmos/hit tests (canvas px): axis-aligned bounds of the clip's layer at t, camera included.
    RectF LayerRectOnCanvas(const project::Project&, const project::Clip&, TimeUs t_us, SizeI video_size) const;
    // Video px (displayed orientation, uncropped frame) -> canvas px, camera included.
    Mat3 VideoToCanvas(const project::Project&, const project::Clip&, TimeUs t_us, SizeI video_size) const;
    const RenderTiming& LastTiming() const { return m_timing; }

    // Helpers for the callers that build FrameInputs.
    // Transitions whose window [b.start - d/2, b.start + d/2) contains t, as (incoming clip, transition). Clears out.
    static void ActiveTransitions(const project::Project&, TimeUs t_us,
                                  std::vector<std::pair<const project::Clip*, const project::Transition*>>& out);
    // Progress u in [0,1) of a transition at t; false outside its window or when a clip is missing.
    static bool TransitionProgress(const project::Project&, const project::Transition&, TimeUs t_us, float& u);
    // Media time of a clip at timeline t, clamped into [in_us, out_us - 1] (a transition shows A's last and B's first
    // frame outside the clips' own ranges).
    static TimeUs ClipMediaTime(const project::Clip&, TimeUs t_us);
    // Whether `clip` gets the canvas padding/radius/shadow defaults: a Source::Screen clip on the track of
    // Project::PrimaryScreenClip().
    static bool IsPrimaryScreen(const project::Project&, const project::Clip&);
    // Opacity of the keystroke overlay badge at media time t (strokes held for hold_us): fades in over 120 ms from the
    // start of the continuous run of visible strokes and out over 200 ms before the newest stroke expires; 0 when no
    // stroke is visible.
    static float KeystrokeBadgeAlpha(const events::EventLog&, TimeUs t_us, TimeUs hold_us);

    TextRenderer& Text() { return m_text; }
    CursorRenderer& Cursor() { return m_cursor; }

private:
    struct LayerGeom {
        RectF base;              // canvas px before the clip transform
        Mat3 clip_m;             // ClipTransform
        Mat3 full;               // camera * clip transform
        Mat3 video_to_base;      // video px -> base rect coordinates
        PointF uv[4];            // texture uv at base TL, TR, BL, BR
        float radius = 0;        // base units
        const project::Shadow* shadow = nullptr;   // null = none
        bool primary = false;
    };
    bool ComputeGeom(const project::Project&, const project::Clip&, TimeUs t, SizeI video_size, int rotation_deg,
                     const Mat3& cam, LayerGeom& g) const;
    void DrawLayer(const project::Project&, const VideoLayerInput&, TimeUs t, const Mat3& cam, ID3D11RenderTargetView* rtv,
                   SizeI size);
    void DrawBackground(const project::Project&, const FrameInputs&, const RenderOptions&, ID3D11RenderTargetView*, SizeI);
    void DrawCursorAndBadges(const project::Project&, const FrameInputs&, const RenderOptions&, TimeUs t, const Mat3& cam,
                             ID3D11RenderTargetView*, SizeI);
    void PrepareText(const project::Project&, const FrameInputs&, const RenderOptions&, TimeUs t, float zoom);
    void DrawAnnotations(const project::Project&, TimeUs t, const Mat3& cam, float zoom, ID3D11RenderTargetView*, SizeI);
    void DrawRegionEffect(const project::Clip&, const project::Annotation&, TimeUs t, const Mat3& cam, float zoom, float alpha,
                          ID3D11RenderTargetView*, SizeI);
    const VideoLayerInput* TopScreenLayer(const FrameInputs&, TimeUs t) const;

    struct AnnItem {
        const project::Clip* clip = nullptr;
        const TextRenderer::Layer* layer = nullptr;   // null for region effects
    };

    RenderContext* m_rc = nullptr;
    TextRenderer m_text;
    CursorRenderer m_cursor;
    RenderTiming m_timing;
    platform::GpuTimer m_timers[4];   // total, background, layers, overlays (gpu_timing)
    ID3D11Resource* m_target_res = nullptr;   // this frame's target (for region copies); not owned beyond the frame
    std::vector<AnnItem> m_ann;               // this frame's annotations, track order
    const TextRenderer::Layer* m_badge = nullptr;
    float m_badge_alpha = 0;
    std::vector<std::string> m_keys;          // scratch for the badge
    size_t m_key_count = 0;
    std::vector<const project::Clip*> m_done;   // clips already drawn by a transition this frame
    // Blurred wallpaper (Background::Kind::Image with blur > 0), cached by (srv, size, blur). The key holds a reference:
    // a raw pointer could be freed and its address reused by a different wallpaper (stale blur).
    RenderContext::TempRT m_bg_blur;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_bg_key_srv;
    float m_bg_key_blur = 0;
};

} // namespace ct::render
