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

// A solid BGRA texture and its SRV.
bool MakeSolid(platform::D3DDevice& dev, SizeI size, uint32_t bgra, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& out) {
    std::vector<uint32_t> px(static_cast<size_t>(size.w) * static_cast<size_t>(size.h), bgra);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(size.w);
    td.Height = static_cast<UINT>(size.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), static_cast<UINT>(size.w * 4), 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev.Device()->CreateTexture2D(&td, &init, tex.GetAddressOf()))) return false;
    return SUCCEEDED(dev.Device()->CreateShaderResourceView(tex.Get(), nullptr, out.ReleaseAndGetAddressOf()));
}

events::KeyPayload KeyDown(uint16_t vk, const char* label, bool down) {
    events::KeyPayload k;
    k.vk = vk;
    k.down = down ? 1 : 0;
    k.modifiers = events::kModCtrl;
    events::CopyLabel(k.label, sizeof(k.label), label);
    return k;
}

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

// Finding 3: the badge faded in from the oldest stroke it shows; when that stroke expired, or a 4th stroke pushed it
// out, the opacity dropped while the badge stayed up. It now fades in from the start of the continuous visible run.
CT_TEST(render_keystroke_badge_alpha_does_not_dip) {
    events::EventHeader hdr;
    hdr.qpc_frequency = 10'000'000;
    hdr.region_w = 1280;
    hdr.region_h = 720;
    hdr.flags = events::kFlagKeystrokes;
    events::EventWriter w;
    w.OpenMemory(hdr);
    const TimeUs downs[] = {1'000'000, 1'020'000, 1'040'000, 1'060'000,   // 4 quick strokes (the 4th pushes the 1st out)
                            2'530'000,                                    // shortly before 1.06 s expires (hold 1.5 s)
                            5'000'000};                                   // a new run after a gap
    const char* labels[] = {"A", "B", "C", "D", "E", "F"};
    for (int i = 0; i < 6; ++i) {
        const uint16_t vk = static_cast<uint16_t>('A' + i);
        w.Key(downs[i], KeyDown(vk, labels[i], true));
        w.Key(downs[i] + 10'000, KeyDown(vk, labels[i], false));
    }
    events::EventLog log;
    Err err;
    CT_REQUIRE(log.LoadFromMemory(w.Bytes(), &err));
    CT_REQUIRE(log.Strokes().size() == 6);
    constexpr TimeUs kHold = 1'500'000;
    using render::Compositor;
    CT_CHECK_EQ(Compositor::KeystrokeBadgeAlpha(log, 999'999, kHold), 0.f);
    CT_CHECK_NEAR(Compositor::KeystrokeBadgeAlpha(log, 1'060'000, kHold), 0.5f, 1e-4);
    // Fade-in over the first 120 ms, never decreasing although the 4th stroke pushes the 1st out of the badge at 1.06 s.
    float prev = 0.f;
    int dips = 0;
    for (TimeUs t = 1'000'000; t <= 1'120'000; t += 1'000) {
        const float a = Compositor::KeystrokeBadgeAlpha(log, t, kHold);
        if (a + 1e-6f < prev) ++dips;
        prev = a;
    }
    CT_CHECK_EQ(dips, 0);
    // Fully opaque until the fade-out toward 1.06 + 1.5 s starts (2.36 s; the badge cannot know that E follows), and
    // again from E (2.53 s, while D still shows) until E fades out (4.03 s - 200 ms): no dip when A..D expire.
    int not_opaque = 0;
    for (TimeUs t = 1'120'000; t <= 3'830'000; t += 1'000) {
        if (t > 2'360'000 && t < 2'530'000) continue;
        if (Compositor::KeystrokeBadgeAlpha(log, t, kHold) != 1.f) ++not_opaque;
    }
    CT_CHECK_EQ(not_opaque, 0);
    CT_CHECK_NEAR(Compositor::KeystrokeBadgeAlpha(log, 3'930'000, kHold), 0.5f, 1e-4);   // fade-out
    CT_CHECK_EQ(Compositor::KeystrokeBadgeAlpha(log, 4'030'000, kHold), 0.f);
    // After the gap the next stroke fades in from its own time.
    CT_CHECK_NEAR(Compositor::KeystrokeBadgeAlpha(log, 5'060'000, kHold), 0.5f, 1e-4);
}

// Finding 4: the blurred wallpaper was cached by the SRV's address; a new wallpaper allocated at the freed address of
// the old one showed the old blur.
CT_TEST_DEVICE(render_wallpaper_blur_follows_a_new_image) {
    Fixture fx;
    if (!fx.Init(kCanvas)) CT_SKIP("no D3D11 device");
    project::Project p = render_test::MakeProject(kCanvas);
    p.clips.clear();
    p.canvas.background.kind = project::Background::Kind::Image;
    p.canvas.background.blur = 6.f;
    const SizeI img{96, 64};
    const size_t centre = (static_cast<size_t>(kCanvas.h / 2) * kCanvas.w + kCanvas.w / 2) * 4;   // RGBA
    std::vector<uint8_t> px;
    for (int round = 0; round < 8; ++round) {
        const bool red = round % 2 == 0;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        CT_REQUIRE(MakeSolid(fx.dev, img, red ? 0xFFFF0000u : 0xFF0000FFu, srv));
        render::FrameInputs in;
        in.background_image = srv.Get();
        in.background_image_size = img;
        CT_REQUIRE(fx.Render(p, 0, in, px));
        if (red) CT_CHECK(px[centre] > 240 && px[centre + 2] < 15);
        else CT_CHECK(px[centre + 2] > 240 && px[centre] < 15);
        // srv (and its texture) is released here: the next image may get the same address.
    }
}
