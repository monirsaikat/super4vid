#pragma once
// ct/render/text_renderer.h
// Annotations and keystroke badges rasterized with Direct2D/DirectWrite into cached BGRA premultiplied textures on the
// shared D3D11 device. A layer is re-rendered only when its cache key (every field that changes pixels, the canvas
// size and the raster scale) changes. EndFrame (called by Compositor::RenderFrame once no layer is in use) evicts layers
// not used for kIdleFrames frames and then enforces the byte cap (least recently used first); lookups are O(1).
// Thread model: the render thread with D3DDevice::Lock held. Never call while one of the layer textures is bound as a
// D3D shader resource (the compositor rasterizes before its D3D passes and unbinds SRVs first).

#include <ct/project/types.h>
#include <ct/render/render_context.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ct::render {

class TextRenderer {
public:
    TextRenderer() = default;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    HRESULT Init(RenderContext&);
    void Shutdown();

    struct Layer {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        SizeI size;          // texture pixels (canvas_rect size * scale, rounded up)
        RectF canvas_rect;   // where the texture goes, in canvas pixels before the camera (badges: anchored at 0,0)
    };

    // Rasterizes (or returns the cached raster of) an annotation of type Text, Callout, Arrow, Rect, Ellipse.
    // scale > 1 renders more pixels per canvas pixel (camera zoom) so text stays crisp. out stays valid until the next
    // EndFrame/Trim/Shutdown. Highlight/Blur/Keystrokes are drawn by the compositor and return E_INVALIDARG here.
    HRESULT Annotation(const project::Annotation&, SizeI canvas, float scale, const Layer*& out);
    // A badge for the given strokes (oldest first, e.g. {"Ctrl+C", "Ctrl+V"}): one pill with the strokes separated by
    // spaces, or for Keycap one key cap per key (strokes split at '+'). canvas_rect.x/y are 0; the caller positions it.
    // font_px 0 = 3.2 % of the canvas height.
    HRESULT KeystrokeBadge(const std::vector<std::string>& keys, const project::KeystrokeStyle&, SizeI canvas, float scale,
                           const Layer*& out, float font_px = 0);
    // Evicts least recently used layers until at most max_bytes remain.
    void Trim(size_t max_bytes = kMaxBytes);
    // End of a frame: evicts layers unused for more than max_idle_frames frames, then Trim(max_bytes). Invalidates
    // every Layer pointer handed out for evicted entries.
    void EndFrame(uint32_t max_idle_frames = kIdleFrames, size_t max_bytes = kMaxBytes);
    static constexpr uint32_t kIdleFrames = 60;
    static constexpr size_t kMaxBytes = size_t{64} << 20;

    void SetDeterministic(bool grayscale_aa) { m_grayscale = grayscale_aa; }
    size_t CachedLayers() const { return m_entries.size(); }
    size_t CachedBytes() const { return m_bytes; }
    uint64_t Rasterizations() const { return m_rasterizations; }   // cache misses since Init

    // Family name that will be used for `requested` (falls back to "Segoe UI" when the family is not installed; logs
    // once per name).
    const wchar_t* ResolveFont(const std::string& requested);

private:
    struct Entry {
        uint64_t key = 0;
        Layer layer;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        uint64_t last_use = 0;     // m_tick at the last lookup (LRU order)
        uint64_t last_frame = 0;   // m_frame at the last lookup
    };
    static size_t Bytes(const Entry& e) {
        return static_cast<size_t>(e.layer.size.w) * static_cast<size_t>(e.layer.size.h) * 4;
    }
    Entry* Find(uint64_t key);
    HRESULT NewEntry(uint64_t key, SizeI size, Entry*& out);
    void Drop(uint64_t key);
    HRESULT BeginDraw(Entry&);
    HRESULT EndDraw();
    HRESULT MakeFormat(const std::string& font, float size_px, int weight, IDWriteTextFormat** out);
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> m_params;   // fixed text rendering params (deterministic)

    RenderContext* m_rc = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<Entry>> m_entries;   // by key; stable addresses: Layers survive inserts
    size_t m_bytes = 0;      // texture bytes of m_entries
    uint64_t m_tick = 0;
    uint64_t m_frame = 0;    // EndFrame count
    uint64_t m_rasterizations = 0;
    bool m_grayscale = true;
    struct FontName {
        std::string requested;
        std::wstring resolved;
    };
    std::vector<FontName> m_fonts;
    std::wstring m_text;   // scratch (UTF-16 of the current text)
};

} // namespace ct::render
