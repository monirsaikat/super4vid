#pragma once
// Shared by the ct_render tests: a device + RenderContext + Compositor with a canvas-sized target, synthetic NV12
// "video" frames, a synthetic project and a synthetic event log (cursor path, one click, a shortcut).

#include <ct/events/ctev.h>
#include <ct/events/event_log.h>
#include <ct/platform/d3d_device.h>
#include <ct/project/project.h>
#include <ct/render/compositor.h>
#include <ct/render/render_context.h>

#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ct::render_test {

enum class Pattern { Screen, Camera, Slides };

struct Nv12Frame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y, uv;
    SizeI size;
};

// CPU pattern (BGRA) -> NV12 with the capture module's CPU reference (BT.709 limited) -> immutable NV12 texture.
bool MakeNv12(platform::D3DDevice&, SizeI size, Pattern, Nv12Frame& out);
void MakePatternBgra(SizeI size, Pattern, std::vector<uint8_t>& bgra);

class Fixture {
public:
    // Device (debug layer when requested and available), context, compositor, a canvas-sized BGRA target.
    bool Init(SizeI canvas, bool debug_layer = false);
    ~Fixture();
    // Renders and reads back RGBA8 (holding the device lock around RenderFrame).
    bool Render(const project::Project&, TimeUs t, const render::FrameInputs&, std::vector<uint8_t>& rgba,
                const render::RenderOptions& = {});
    // Renders only (no readback); for benches.
    HRESULT RenderOnly(const project::Project&, TimeUs t, const render::FrameInputs&, const render::RenderOptions& = {});
    // Number of debug-layer messages stored so far (0 when the debug layer is off).
    uint64_t DebugMessages();
    void PrintDebugMessages();

    platform::D3DDevice dev;
    render::RenderContext rc;
    render::Compositor comp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    SizeI size;
    bool ready = false;
};

// Canvas w x h, gradient background, padding/radius/shadow scaled to the canvas, tracks Screen (t1), Camera (t2),
// Callouts (t3, annotations); media m1 (recording 1280x720, 10 s); clip c1 (screen, 0..10 s) on t1.
project::Project MakeProject(SizeI canvas);
project::Clip& AddAnnotation(project::Project&, project::Annotation::Type, TimeUs start, TimeUs end);

// Event log for a 1280x720 region: cursor from (300,200) to (640,360) between 0 and 0.9 s, still afterwards; left
// click at 1.0 s (up at 1.08 s) at (640,360); Ctrl+Shift+S at 1.2 s. Uses the built-in arrow as shape.
bool MakeEvents(events::EventLog& log);

render::VideoLayerInput Layer(const Nv12Frame&, const project::Clip*, TimeUs media_t, const events::EventLog* ev = nullptr);

} // namespace ct::render_test
