# P08 · Compositor (deterministic GPU frame renderer)

**Goal:** `RenderFrame(project, t, inputs) -> canvas texture` on the GPU: background, padded and rounded video
layers with shadows, camera zoom/pan, webcam PIP, re-rendered cursor with smoothing and click effects, keystroke
badges, Direct2D/DirectWrite annotations, blur/pixelate regions, transitions. Same pixels for preview and export.

**Read first:** `docs/03-architecture.md` sections 6, 7, 8; `docs/04` section 3 (semantics of every field); `docs/05` section 1 (visual tokens for badges); `docs/07` (Direct2D rows).
**Depends on:** P06 (`FrameRef`, `Nv12Blit`), P07 (types, `Camera::At`, `events::EventLog`). **Size:** L (about 4,500 lines incl. shaders).

## Deliverables

```
src/render/include/ct/render/{render_context.h,compositor.h,frame_inputs.h,layer_math.h,text_renderer.h,cursor_renderer.h,effects.h,transitions.h,readback.h}
src/render/src/*.cpp
src/render/shaders/{fullscreen.hlsl,layer.hlsl,gradient.hlsl,blur.hlsl,pixelate.hlsl,cursor.hlsl,shadow_mask.hlsl,composite.hlsl,transition.hlsl}
tests/render/*.cpp   tests/data/golden/*.png
```

## Interfaces

```cpp
namespace ct::render {
struct Mat3 { float m[9]; static Mat3 Identity(); static Mat3 Translate(float,float); static Mat3 Scale(float,float); static Mat3 Rotate(float deg); Mat3 operator*(const Mat3&) const; PointF Apply(PointF) const; Mat3 Inverse() const; };

class RenderContext {                       // one per D3DDevice; all pipeline objects created once
public:
    HRESULT Init(platform::D3DDevice&);
    platform::D3DDevice& Device();  ID3D11DeviceContext4* Ctx();            // caller holds D3DDevice::Lock for the whole RenderFrame
    ID2D1DeviceContext* D2D();  IDWriteFactory3* DWrite();                   // D2D device created from the DXGI device (needs BGRA_SUPPORT)
    struct TempRT { Microsoft::WRL::ComPtr<ID3D11Texture2D> tex; Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv; Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; SizeI size; };
    TempRT* AcquireTemp(SizeI, DXGI_FORMAT = DXGI_FORMAT_B8G8R8A8_UNORM);  void ReleaseTemp(TempRT*);   // pooled per size, reset each frame
    // pipeline helpers used by all passes
    void SetFullscreen(ID3D11RenderTargetView*, SizeI viewport);  void DrawFullscreen();   // 3-vertex triangle
    void SetBlendPremul();  void SetBlendOpaque();  ID3D11SamplerState* Linear();  ID3D11SamplerState* Point();
    platform::GpuTimer& Timer(int slot);
};

struct VideoLayerInput {                   // resolved by the caller (Player/Exporter) for time t
    const project::Clip* clip;  playback::FrameRef frame;  /* y/uv SRVs or rgba */  ID3D11ShaderResourceView* rgba = nullptr;  bool is_rgba = false;  int rotation_deg = 0;  TimeUs media_t_us = 0;
    const events::EventLog* events = nullptr;      // for Source::Screen clips with events
    SizeI video_size;                               // source pixel size (after rotation)
};
struct FrameInputs { std::vector<VideoLayerInput> layers; /* bottom to top, track order */ std::vector<std::pair<const project::Clip*, const project::Transition*>> active_transitions; };
struct RenderOptions { bool cursor = true; bool annotations = true; bool keystrokes = true; bool background = true; bool deterministic_text = true; };
struct RenderTiming { double background_ms, layers_ms, cursor_ms, annotations_ms, total_ms; };

class Compositor {
public:
    HRESULT Init(RenderContext&);
    // target: BGRA8 RTV of exactly canvas size. Deterministic for identical inputs.
    HRESULT RenderFrame(const project::Project&, TimeUs t_us, const FrameInputs&, ID3D11RenderTargetView* target, const RenderOptions& = {});
    RectF LayerRectOnCanvas(const project::Project&, const project::Clip&, TimeUs t_us, SizeI video_size) const;   // for UI gizmos/hit tests (canvas px)
    Mat3  VideoToCanvas(const project::Project&, const project::Clip&, TimeUs t_us, SizeI video_size) const;      // video px -> canvas px (incl. camera)
    const RenderTiming& LastTiming() const;
};

namespace layer_math {
    RectF StageRect(const project::Canvas&);                                                   // canvas minus padding
    RectF FitContain(SizeI content, RectF bounds);                                             // centered, aspect-preserving
    Mat3  ClipTransform(const project::Clip&, TimeUs t, RectF base_rect, const project::Canvas&);   // base rect -> transform (x,y offsets in canvas fraction, scale about centre, rotation)
    Mat3  CameraTransform(const project::Camera::View&, const project::Canvas&);              // zoom about (cx,cy) in canvas fraction
    RectF PipRect(const project::Pip&, SizeI cam_size, const project::Canvas&);
}

class TextRenderer {                        // Direct2D into cached BGRA premultiplied textures
public:
    HRESULT Init(RenderContext&);
    struct Layer { Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; SizeI size; RectF canvas_rect; };
    HRESULT Annotation(const project::Annotation&, SizeI canvas, float scale, const Layer*& out);    // cache key: hash(annotation fields, canvas, scale)
    HRESULT KeystrokeBadge(const std::vector<std::string>& keys, const project::KeystrokeStyle&, SizeI canvas, float scale, const Layer*& out);
    void Trim(size_t max_bytes = 64 << 20);
};
class CursorRenderer {
public:
    HRESULT Init(RenderContext&);
    void Prepare(const events::EventLog&);  // builds/refreshes the shape atlas (2048x2048 BGRA, shelf packing; 2 atlases max)
    void Draw(const project::CursorStyle&, const events::EventLog&, TimeUs media_t, const Mat3& video_to_canvas, float effective_scale, ID3D11RenderTargetView* target, SizeI target_size);
    static PointF SmoothedPosition(const events::EventLog&, TimeUs t, float smoothing);   // zero-lag Gaussian over a +-(smoothing*120 ms) window of samples; pure, tested
};
namespace effects {
    void GaussianBlur(RenderContext&, ID3D11ShaderResourceView* src, SizeI src_size, float sigma_px, RenderContext::TempRT& dst);   // separable, downsample by 2 when sigma > 8
    void Pixelate(RenderContext&, ID3D11ShaderResourceView* src, SizeI, int block_px, RectF region, ID3D11RenderTargetView* dst);
    void RoundedShadow(RenderContext&, RectF rect_px, float radius_px, const project::Shadow&, Mat3 transform, ID3D11RenderTargetView* dst, SizeI dst_size);   // mask at 1/4 res -> blur -> composite; cached by (rect,radius,params,transform)
}
namespace transitions { void Apply(RenderContext&, const project::Transition&, float u, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, ID3D11RenderTargetView* dst, SizeI); }
struct Readback { static HRESULT ToBgra8(platform::D3DDevice&, ID3D11Texture2D* src, std::vector<uint8_t>& out, SizeI& size); };   // staging + Map; tests, PNG export, thumbnails
}
```

## Frame algorithm (`Compositor::RenderFrame`)
```
0. timing.begin; ctx.SetFullscreen(target, canvas)
1. view = project.camera.At(t) (Manual or Auto keys both live in camera.keys); M_cam = CameraTransform(view)
2. Background (options.background): Solid -> ClearRenderTargetView; Gradient -> gradient.hlsl (angle, two colors, sRGB lerp); Image -> cover-fit blit (+ optional cached blur)
3. Layers, bottom to top, for each VideoLayerInput L whose clip is active at t and track not hidden:
   a. base rect: Source::Screen (primary) -> FitContain(video_size, StageRect); Source::Camera with pip -> PipRect; others -> FitContain(video_size, canvas full rect)
   b. M = M_cam * ClipTransform(clip, t, base, canvas)
   c. radius = clip.corner_radius_px.value_or(canvas.corner_radius_px for primary screen clip, 0 otherwise); pip mask Circle -> radius = min(w,h)/2
   d. shadow = clip.shadow.value_or(canvas.shadow for primary screen clip, none) -> effects::RoundedShadow(base rect, radius, shadow, M)
   e. layer.hlsl: quad = base rect transformed by M (4 vertices via SV_VertexID from a constant buffer); PS samples NV12 (Y/UV -> BT.709 limited -> RGB) or RGBA; applies crop uv; SDF rounded-rect mask with 1 px analytic AA in canvas space; opacity = clip.Prop("transform.opacity", t) ; premultiplied blend
   f. if an active transition involves this clip: render this clip and its partner into two temps and transitions::Apply(u) instead of e.
4. Cursor (options.cursor): for the topmost Source::Screen layer with events and cursor.visible: CursorRenderer.Draw(style, events, media_t, VideoToCanvas, scale = style.scale * layer_scale)
   - position = SmoothedPosition(events, media_t, style.smoothing) ; hidden if no movement for hide_when_idle_us (fade out 300 ms) or !visible flag
   - click effects: for each mouse-down within [media_t - 450 ms, media_t]: Ripple = ring radius 12..48 px (x scale) alpha 0.7*(1-u); Highlight = filled circle 40 px alpha 0.35 while button held
   - spotlight: darken everything outside a 220 px soft circle (drawn before the cursor sprite)
   - sprite: atlas quad at (pos - hotspot*scale), linear sampling, optional drop shadow (offset 2 px, alpha 0.5, blur via 4 taps)
5. Keystrokes (options.keystrokes): keys pressed within [media_t - hold_ms, media_t] from the screen clip's events -> TextRenderer.KeystrokeBadge -> blit at the style position (bottom centre 6 % up), fade in 120 ms / out 200 ms
6. Annotations (options.annotations): clips on Annotations tracks active at t (sorted by track order):
   - u_in = clamp((t - start)/anim_ms), u_out = clamp((end - t)/anim_ms); anim Pop = scale 0.8->1 + alpha; Fade = alpha; Slide = +24 px offset
   - Blur/Highlight/Pixelate types operate on the current target: copy the region to a temp, apply effect, draw back with the rounded mask (region transformed by M_cam)
   - others: TextRenderer.Annotation -> blit at rect (transformed by M_cam) with anim transform
7. timing.end (GpuTimer per pass when stats enabled)
```
Color policy: all RTs `B8G8R8A8_UNORM`, values are sRGB-encoded, blending in gamma space (documented, matches common editors). NV12 -> RGB uses BT.709 limited range (`R = 1.1644(Y-16/255) + 1.7927(V-.5)`, `G = 1.1644(Y') - 0.2133(U-.5) - 0.5329(V-.5)`, `B = 1.1644(Y') + 2.1124(U-.5)`).

## Text rendering details
`D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_ID2D1Factory1)`, `factory->CreateDevice(dxgi_device)`, `device->CreateDeviceContext(NONE)`. Per layer texture: `D3D11_TEXTURE2D_DESC{ BGRA8, BIND_RENDER_TARGET | BIND_SHADER_RESOURCE }` -> `IDXGISurface` -> `CreateBitmapFromDxgiSurface(props{ D2D1_BITMAP_OPTIONS_TARGET | CANNOT_DRAW, {BGRA8, PREMULTIPLIED} })` -> `SetTarget` -> `BeginDraw` -> `Clear(0,0,0,0)` -> draw -> `EndDraw`. `deterministic_text` sets `SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE)` so goldens match across ClearType settings. Fonts via `IDWriteFactory::CreateTextFormat(font, nullptr, weight, NORMAL, NORMAL, size_px, L"en-us")` + `CreateTextLayout` for measuring; fallback to "Segoe UI" when a font is missing. Callout: rounded rect + triangle pointer path (`ID2D1PathGeometry`) to `pointer`; Arrow: stroked line with a filled head; Rect/Ellipse: stroke/fill; Keycap style badge: individual rounded rects per key with a 2 px bottom shade.

## Cursor smoothing (pure function, tested)
```
samples = events.CursorSamplesBetween(t - w, t + w), w = smoothing * 120 ms
if smoothing == 0: return linear interpolation between neighbours of t
weights = Gaussian(sigma = w / 2) evaluated at (sample.t - t); position = sum(w_i * p_i) / sum(w_i)
```
Clicks snap: within 60 ms before a mouse-down, blend the smoothed position toward the click position so ripples appear where the click actually happened.

## Tests
- Unit: `layer_math` (FitContain, transforms round-trip, camera clamp), `SmoothedPosition` (constant velocity stays on the line; zero smoothing equals interpolation), atlas packing.
- Golden (device, required locally): project fixtures with synthetic inputs (a gradient uploaded as the "video" NV12 via `ColorConverter`, a synthetic arrow cursor shape, a 1-click event log): `bg_gradient`, `layer_rounded_shadow`, `camera_zoom_2x`, `pip_circle`, `cursor_ripple_t+150ms`, `annotation_callout`, `annotation_blur`, `keystroke_badge`, `transition_crossfade_u0.5`. Tolerance per `docs/06` (text goldens PSNR >= 35 dB).
- Determinism: render the same frame 10 times -> identical bytes.
- Perf (`--bench`): 1080p, 3 layers + cursor + 2 annotations, 300 frames: GPU < 4 ms avg, CPU < 1 ms avg, zero heap allocations after warm-up (count with `_CrtMemCheckpoint` in Debug).

## Acceptance (8.7)
Golden tests pass on the reference machine; perf numbers in `PROGRESS.md`; a Debug run of the bench shows no D3D debug-layer messages.

## Pitfalls
- Direct2D and D3D11 share the device; **flush D2D (`EndDraw`) before sampling its target texture** in D3D, and never draw D2D onto a texture currently bound as a D3D SRV.
- D2D needs the D3D device created with `D3D11_CREATE_DEVICE_BGRA_SUPPORT` (P00 did this; assert).
- SDF rounded-rect AA must be computed in canvas pixel units after the transform, or corners look blurry when zoomed.
- `AcquireTemp` must return textures of exactly the requested size; the blur downsample path uses sizes rounded up to even.
- Keep constant buffers 16-byte aligned; `Map(DISCARD)` per draw, never `UpdateSubresource` on dynamic buffers.
- Do not read back the target for Blur/Pixelate every frame; copy only the region (`CopySubresourceRegion` with a box).
- Fonts: `CreateTextFormat` fails for unknown family names; fall back and log once per name.
