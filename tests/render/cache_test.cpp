// Bounded GPU caches (text rasters, temp render targets), the wallpaper blur cache key and the keystroke badge fade.
// Regression tests for review findings: caches that grew with every animated frame, a fade-in that dipped mid-display
// and a cache keyed on a raw pointer.
#include <ct/events/ctev.h>
#include <ct/events/event_log.h>
#include <ct/render/compositor.h>
#include <ct/test/ct_test.h>

#include "render_fixture.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace ct;
using render_test::Fixture;
using render_test::Layer;
using render_test::Nv12Frame;
using render_test::Pattern;

namespace {

constexpr SizeI kCanvas{640, 360};

} // namespace

// Finding 1: an annotation whose rect moves every frame used to leave one texture per frame in the text cache.
CT_TEST_DEVICE(render_text_cache_bounded_when_annotation_animates) {
    Fixture fx;
    if (!fx.Init(kCanvas)) CT_SKIP("no D3D11 device");
    project::Project p = render_test::MakeProject(kCanvas);
    p.clips.clear();
    project::Clip& c = render_test::AddAnnotation(p, project::Annotation::Type::Rect, 0, 5'000'000);
    c.annotation->text = "Moving";
    c.annotation->size_px = 18;
    c.annotation->rect = RectF{0.1f, 0.2f, 0.25f, 0.12f};
    render::TextRenderer& text = fx.comp.Text();
    const uint64_t raster0 = text.Rasterizations();
    size_t max_layers = 0, max_bytes = 0;
    for (int i = 0; i < 1000; ++i) {
        p.clips.back().annotation->rect.x = 0.1f + static_cast<float>(i) * 0.0005f;   // a new cache key every frame
        CT_REQUIRE(SUCCEEDED(fx.RenderOnly(p, 1'000'000, {})));
        max_layers = std::max(max_layers, text.CachedLayers());
        max_bytes = std::max(max_bytes, text.CachedBytes());
    }
    std::printf("  1000 frames: max %zu cached layers, %zu bytes\n", max_layers, max_bytes);
    CT_CHECK_EQ(text.Rasterizations() - raster0, 1000ull);   // every frame was a miss ...
    CT_CHECK(max_layers <= render::TextRenderer::kIdleFrames + 2);   // ... but idle rasters are evicted
    CT_CHECK(max_bytes <= render::TextRenderer::kMaxBytes);
    // A still frame keeps its raster (no re-rasterization) and the rest ages out.
    for (int i = 0; i < static_cast<int>(render::TextRenderer::kIdleFrames) + 2; ++i)
        CT_REQUIRE(SUCCEEDED(fx.RenderOnly(p, 1'000'000, {})));
    CT_CHECK_EQ(text.CachedLayers(), size_t{1});
    CT_CHECK_EQ(text.Rasterizations() - raster0, 1000ull);
    // The byte cap evicts least recently used layers.
    text.EndFrame(render::TextRenderer::kIdleFrames, 0);
    CT_CHECK_EQ(text.CachedLayers(), size_t{0});
    CT_CHECK_EQ(text.CachedBytes(), size_t{0});
}
