#pragma once
// ct/render/frame_inputs.h
// What the caller (Player in P09, Exporter in P10) resolves for one timeline time before calling
// Compositor::RenderFrame: the decoded frame of every visible video clip, the active transitions and the background
// image. Plain fields only: ct_render does not depend on ct_playback; P09/P10 convert playback::FrameRef into
// VideoLayerInput (DECISIONS.md, P08).
// Thread model: plain values; the SRVs must stay alive until RenderFrame returns.

#include <ct/core/types.h>
#include <ct/events/event_log.h>
#include <ct/project/project.h>

#include <d3d11.h>

#include <utility>
#include <vector>

namespace ct::render {

struct VideoLayerInput {
    // NV12 frame: y_srv = R8_UNORM view of plane 0, uv_srv = R8G8_UNORM view of plane 1 (BT.709 limited range).
    ID3D11ShaderResourceView* y_srv = nullptr;
    ID3D11ShaderResourceView* uv_srv = nullptr;
    // RGBA frame (images, already converted video): B8G8R8A8/R8G8B8A8 view, straight alpha unless rgba_premultiplied.
    ID3D11ShaderResourceView* rgba_srv = nullptr;
    bool is_rgba = false;
    bool rgba_premultiplied = false;
    SizeI video_size;               // source pixel size after rotation (what is displayed)
    int rotation_deg = 0;           // 0, 90, 180, 270: clockwise rotation to apply to the decoded texture
    TimeUs media_t_us = 0;          // media time of this frame (events are looked up at this time)
    const project::Clip* clip = nullptr;
    const events::EventLog* events = nullptr;   // Source::Screen clips with events (cursor, keystrokes)
};

struct FrameInputs {
    std::vector<VideoLayerInput> layers;   // bottom to top, track order
    // Active transitions at t. .second is the transition; .first is its incoming clip (b) (the compositor matches both
    // clips by id). Compositor::ActiveTransitions() fills this from the project.
    std::vector<std::pair<const project::Clip*, const project::Transition*>> active_transitions;
    // Background::Kind::Image: the decoded wallpaper (straight alpha RGBA view). Null -> color_a fill.
    ID3D11ShaderResourceView* background_image = nullptr;
    SizeI background_image_size;
};

struct RenderOptions {
    bool cursor = true;
    bool annotations = true;
    bool keystrokes = true;
    bool background = true;          // false: clear to transparent black
    bool deterministic_text = true;  // grayscale text AA (goldens independent of ClearType settings)
    bool gpu_timing = false;         // GpuTimer per pass; results (a few frames old) in RenderTiming::gpu_*
};

struct RenderTiming {
    // CPU time of each part of the last RenderFrame call (ms).
    double background_ms = 0, layers_ms = 0, cursor_ms = 0, annotations_ms = 0, total_ms = 0;
    // GPU time of the newest finished frame (ms), when RenderOptions::gpu_timing; -1 until one resolved.
    double gpu_background_ms = -1, gpu_layers_ms = -1, gpu_overlay_ms = -1, gpu_total_ms = -1;
};

} // namespace ct::render
