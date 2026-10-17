#pragma once
// ct/render/text_renderer.h
// Annotations and keystroke badges rasterized with Direct2D/DirectWrite into cached BGRA premultiplied textures on the
// shared D3D11 device. A layer is re-rendered only when its cache key (every field that changes pixels, the canvas
// size and the raster scale) changes; Trim evicts least recently used layers.
// Thread model: the render thread with D3DDevice::Lock held. Never call while one of the layer textures is bound as a
// D3D shader resource (the compositor rasterizes before its D3D passes and unbinds SRVs first).

#include <ct/project/types.h>
#include <ct/render/render_context.h>

#include <cstdint>
#include <memory>
#include <string>
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
    // Trim/Shutdown. Highlight/Blur/Keystrokes are drawn by the compositor and return E_INVALIDARG here.
    HRESULT Annotation(const project::Annotation&, SizeI canvas, float scale, const Layer*& out);
    // A badge for the given strokes (oldest first, e.g. {"Ctrl+C", "Ctrl+V"}): one pill with the strokes separated by
    // spaces, or for Keycap one key cap per key (strokes split at '+'). canvas_rect.x/y are 0; the caller positions it.
    // font_px 0 = 3.2 % of the canvas height.
    HRESULT KeystrokeBadge(const std::vector<std::string>& keys, const project::KeystrokeStyle&, SizeI canvas, float scale,
                           const Layer*& out, float font_px = 0);
    void Trim(size_t max_bytes = 64 << 20);

    void SetDeterministic(bool grayscale_aa) { m_grayscale = grayscale_aa; }
    size_t CachedLayers() const { return m_entries.size(); }
    size_t CachedBytes() const;
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
        uint64_t last_use = 0;
    };
    Entry* Find(uint64_t key);
    HRESULT NewEntry(uint64_t key, SizeI size, Entry*& out);
    HRESULT BeginDraw(Entry&);
    HRESULT EndDraw();
    HRESULT MakeFormat(const std::string& font, float size_px, int weight, IDWriteTextFormat** out);
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> m_params;   // fixed text rendering params (deterministic)

    RenderContext* m_rc = nullptr;
    std::vector<std::unique_ptr<Entry>> m_entries;   // stable addresses: Layer pointers survive inserts
    uint64_t m_tick = 0;
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
