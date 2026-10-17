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

// Finding 2: blur/highlight regions under an animated zoom request temp sizes that change every frame; the pool used to
// keep one texture per distinct size forever.
CT_TEST_DEVICE(render_temp_pool_bounded_when_region_sizes_change) {
    Fixture fx;
    if (!fx.Init(kCanvas)) CT_SKIP("no D3D11 device");
    Nv12Frame screen;
    CT_REQUIRE(render_test::MakeNv12(fx.dev, SizeI{1280, 720}, Pattern::Screen, screen));
    project::Project p = render_test::MakeProject(kCanvas);
    project::Clip& b = render_test::AddAnnotation(p, project::Annotation::Type::Blur, 0, 5'000'000);
    b.annotation->rect = RectF{0.2f, 0.25f, 0.2f, 0.2f};
    b.annotation->blur_px = 40;   // wide: uses the downsampled levels
    project::Clip& px = render_test::AddAnnotation(p, project::Annotation::Type::Blur, 0, 5'000'000);
    px.annotation->rect = RectF{0.55f, 0.2f, 0.2f, 0.3f};
    px.annotation->blur_px = 10;
    px.annotation->pixelate = true;
    project::Clip& h = render_test::AddAnnotation(p, project::Annotation::Type::Highlight, 0, 5'000'000);
    h.annotation->rect = RectF{0.3f, 0.6f, 0.3f, 0.1f};
    h.annotation->fill = ColorF{1.f, 0.9f, 0.2f, 1};
    const size_t nb = p.clips.size() - 3, np = nb + 1, nh = nb + 2;
    render::FrameInputs in;
    in.layers.push_back(Layer(screen, &p.clips[0], 1'000'000));
    size_t max_count = 0, max_bytes = 0;
    for (int i = 0; i < 500; ++i) {
        const float u = static_cast<float>(i) / 500.f;
        p.camera.keys.clear();
        p.camera.keys.push_back(project::CameraKey{0, 1.f + 1.5f * u, 0.45f, 0.45f, project::Ease::Linear, false});
        p.clips[nb].annotation->rect.w = 0.1f + 0.25f * u;
        p.clips[np].annotation->rect.h = 0.1f + 0.3f * u;
        p.clips[nh].annotation->rect.w = 0.05f + 0.4f * u;
        in.layers[0].clip = &p.clips[0];
        CT_REQUIRE(SUCCEEDED(fx.RenderOnly(p, 1'000'000, in)));
        max_count = std::max(max_count, fx.rc.TempCount());
        max_bytes = std::max(max_bytes, fx.rc.TempBytes());
    }
    std::printf("  500 frames: max %zu temps, %zu bytes\n", max_count, max_bytes);
    CT_CHECK(max_count <= 48);
    CT_CHECK(max_bytes <= size_t{48} << 20);
    // Once the animation stops, the textures of the old sizes are freed.
    for (int i = 0; i < static_cast<int>(render::RenderContext::kTempIdleFrames) + 2; ++i)
        CT_REQUIRE(SUCCEEDED(fx.RenderOnly(p, 1'000'000, in)));
    const size_t still = fx.rc.TempCount();
    CT_CHECK(still <= 12);
    // A still frame reuses its temps: no new textures, identical count.
    CT_REQUIRE(SUCCEEDED(fx.RenderOnly(p, 1'000'000, in)));
    CT_CHECK_EQ(fx.rc.TempCount(), still);
}

CT_TEST(render_temp_texture_size_is_bucketed) {
    using RC = render::RenderContext;
    const SizeI a = RC::TempTextureSize(SizeI{1, 1});
    CT_CHECK(a.w == 64 && a.h == 64);
    const SizeI b = RC::TempTextureSize(SizeI{640, 360});
    CT_CHECK(b.w == 640 && b.h == 384);
    const SizeI c = RC::TempTextureSize(SizeI{65, 129});
    CT_CHECK(c.w == 128 && c.h == 192);
    const SizeI d = RC::TempTextureSize(SizeI{16384, 16383});
    CT_CHECK(d.w == 16384 && d.h == 16384);
}
