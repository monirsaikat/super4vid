#pragma once
// ct/render/cursor_renderer.h
// The re-rendered cursor of screen recordings: smoothed position from the event log, shape sprites from a texture
// atlas (2048x2048 BGRA, shelf packing, at most 2 atlases), click ripple/highlight, spotlight and drop shadow.
// Thread model: the render thread with D3DDevice::Lock held; SmoothedPosition and ShelfPacker are pure.

#include <ct/events/event_log.h>
#include <ct/project/types.h>
#include <ct/render/layer_math.h>
#include <ct/render/render_context.h>

#include <cstdint>
#include <vector>

namespace ct::render {

// Shelf packer for the shape atlas: rows ("shelves") of the tallest item so far, items placed left to right.
class ShelfPacker {
public:
    explicit ShelfPacker(int width = 2048, int height = 2048, int padding = 2);
    void Reset();
    // False when the item does not fit (the atlas is full).
    bool Insert(int w, int h, int& x, int& y);
    int Width() const { return m_w; }
    int Height() const { return m_h; }

private:
    int m_w, m_h, m_pad;
    int m_shelf_y = 0, m_shelf_h = 0, m_x = 0;
};

class CursorRenderer {
public:
    static constexpr int kAtlasSize = 2048;
    static constexpr int kMaxAtlases = 2;
    static constexpr TimeUs kClickEffectUs = 450'000;   // ripple length
    static constexpr TimeUs kIdleFadeUs = 300'000;      // fade out after hide_when_idle

    CursorRenderer() = default;
    ~CursorRenderer();
    CursorRenderer(const CursorRenderer&) = delete;
    CursorRenderer& operator=(const CursorRenderer&) = delete;

    HRESULT Init(RenderContext&);
    void Shutdown();
    // Builds/refreshes the shape atlas for this log (no-op when the log and its shape count are unchanged).
    void Prepare(const events::EventLog&);
    // Draws spotlight, click effects, shadow and sprite into target. video_to_canvas maps event (region) pixels to target
    // pixels; effective_scale = style.scale * layer scale (sprite pixels per shape pixel).
    void Draw(const project::CursorStyle&, const events::EventLog&, TimeUs media_t, const Mat3& video_to_canvas,
              float effective_scale, ID3D11RenderTargetView* target, SizeI target_size);

    // Zero-lag Gaussian over +-(smoothing * 120 ms): the cursor path (linear between samples, events::CursorAt) is
    // sampled every 1/120 s around t and weighted with sigma = window / 2. smoothing 0 = linear interpolation. Within
    // 60 ms of a mouse-down the result is blended toward the click position (weight 1 at the down). Pure.
    static PointF SmoothedPosition(const events::EventLog&, TimeUs t, float smoothing);

    // The built-in arrow (white with a black outline, premultiplied BGRA, hotspot at the tip), used for shape id 0 and
    // shapes missing from the log. Tests write it into synthetic event logs.
    static void MakeArrowShape(std::vector<uint8_t>& bgra, int& w, int& h, int& hot_x, int& hot_y);

    // Diagnostics for tests.
    bool HasShape(uint32_t shape_id) const;
    int AtlasCount() const { return m_atlas_count; }
    uint64_t AtlasUploads() const { return m_uploads; }

private:
    struct Slot {
        uint32_t id = 0;
        int atlas = 0;
        int x = 0, y = 0, w = 0, h = 0;
        int hot_x = 0, hot_y = 0;
    };
    const Slot* FindSlot(uint32_t id) const;
    HRESULT EnsureAtlas(int index);

    RenderContext* m_rc = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_atlas[kMaxAtlases];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_atlas_srv[kMaxAtlases];
    int m_atlas_count = 0;
    ShelfPacker m_packers[kMaxAtlases];
    std::vector<Slot> m_slots;   // sorted by id
    const events::EventLog* m_prepared_log = nullptr;
    size_t m_prepared_shapes = 0;
    uint64_t m_uploads = 0;
    std::vector<events::ButtonEvent> m_downs;   // scratch, reused every frame
};

} // namespace ct::render
