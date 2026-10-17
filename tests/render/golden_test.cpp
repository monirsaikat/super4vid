// Compositor golden images (tests/data/golden/*.png, 640x360) and determinism. Synthetic inputs only: NV12 frames made
// from CPU patterns, the built-in arrow cursor, an in-memory event log.
#include <ct/render/compositor.h>
#include <ct/render/effects.h>
#include <ct/test/ct_test.h>

#include "render_fixture.h"

#include <cstring>
#include <memory>

using namespace ct;
using render_test::Fixture;
using render_test::Layer;
using render_test::Nv12Frame;
using render_test::Pattern;

namespace {

constexpr SizeI kCanvas{640, 360};

struct Shared {
    Fixture fx;
    Nv12Frame screen, camera, slides;
    events::EventLog events;
    bool ok = false;
};

Shared* Get() {
    static std::unique_ptr<Shared> s;
    if (!s) {
        s = std::make_unique<Shared>();
        s->ok = s->fx.Init(kCanvas, false) && render_test::MakeNv12(s->fx.dev, SizeI{1280, 720}, Pattern::Screen, s->screen) &&
                render_test::MakeNv12(s->fx.dev, SizeI{640, 480}, Pattern::Camera, s->camera) &&
                render_test::MakeNv12(s->fx.dev, SizeI{1280, 720}, Pattern::Slides, s->slides) && render_test::MakeEvents(s->events);
    }
    return s->ok ? s.get() : nullptr;
}

bool Render(Shared* s, const project::Project& p, TimeUs t, const render::FrameInputs& in, std::vector<uint8_t>& px) {
    return s->fx.Render(p, t, in, px);
}

} // namespace

#define CT_SHARED_OR_SKIP(s)                                         \
    Shared* s = Get();                                               \
    if (!s) CT_SKIP("no D3D11 device / compositor init failed")

CT_TEST_GOLDEN(bg_gradient) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    p.clips.clear();
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 0, {}, px));
    // Corners: color_a at the top-left, color_b at the bottom-right (135 degrees).
    CT_CHECK_NEAR(px[0], 0.31 * 255, 3);
    const size_t br = (static_cast<size_t>(kCanvas.h - 1) * kCanvas.w + kCanvas.w - 1) * 4;
    CT_CHECK_NEAR(px[br + 2], 0.45 * 255, 3);
    CT_GOLDEN_IMAGE(px.data(), kCanvas.w, kCanvas.h);
}

CT_TEST_GOLDEN(layer_rounded_shadow) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'000'000));
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 1'000'000, in, px));
    CT_GOLDEN_IMAGE(px.data(), kCanvas.w, kCanvas.h);
}

CT_TEST_GOLDEN(camera_zoom_2x) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    p.camera.keys.push_back(project::CameraKey{0, 2.f, 0.35f, 0.4f, project::Ease::InOut, false});
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'000'000));
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 1'000'000, in, px));
    CT_GOLDEN_IMAGE(px.data(), kCanvas.w, kCanvas.h);
}

CT_TEST_GOLDEN(pip_circle) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    project::Clip cam = p.clips[0];
    cam.id = "c2";
    cam.track = "t2";
    cam.source = project::Clip::Source::Camera;
    project::Pip pip;
    pip.corner = project::Pip::Corner::BR;
    pip.size = 0.24f;
    pip.margin_px = 16;
    pip.mask = project::Pip::Mask::Circle;
    pip.mirror = true;
    cam.pip = pip;
    p.clips.push_back(cam);
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'000'000));
    in.layers.push_back(Layer(s->camera, &p.clips[1], 1'000'000));
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 1'000'000, in, px));
    CT_GOLDEN_IMAGE(px.data(), kCanvas.w, kCanvas.h);
}

// Click at 1.0 s; 150 ms later the ripple is a third of the way out.
CT_TEST_GOLDEN(cursor_ripple_t150ms) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    p.clips[0].keystrokes.enabled = false;
    p.clips[0].cursor.scale = 3.f;   // large enough to judge the sprite at 640x360
    p.clips[0].cursor.click_color = ColorF{1.f, 0.3f, 0.31f, 1};   // visible on the blue card under the click
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'150'000, &s->events));
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 1'150'000, in, px));
    CT_GOLDEN_IMAGE(px.data(), kCanvas.w, kCanvas.h);
}

CT_TEST_GOLDEN(annotation_callout) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    project::Clip& c = render_test::AddAnnotation(p, project::Annotation::Type::Callout, 0, 5'000'000);
    c.annotation->text = "Click here";
    c.annotation->size_px = 22;
    c.annotation->rect = RectF{0.56f, 0.16f, 0.28f, 0.13f};
    c.annotation->pointer = project::PointF{0.47f, 0.42f};
    project::Clip& arrow = render_test::AddAnnotation(p, project::Annotation::Type::Arrow, 0, 5'000'000);
    arrow.annotation->rect = RectF{0.2f, 0.75f, 0.15f, -0.2f};
    arrow.annotation->fill = ColorF{1.f, 0.69f, 0.13f, 1};
    arrow.annotation->arrow_head_px = 16;
    project::Clip& rect = render_test::AddAnnotation(p, project::Annotation::Type::Rect, 0, 5'000'000);
    rect.annotation->rect = RectF{0.62f, 0.55f, 0.2f, 0.18f};
    rect.annotation->fill = ColorF{0, 0, 0, 0};
    rect.annotation->stroke = ColorF{1.f, 0.3f, 0.31f, 1};
    rect.annotation->stroke_px = 3;
    rect.annotation->radius_px = 6;
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'000'000));
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 1'000'000, in, px));
    CT_GOLDEN_IMAGE_TOL(px.data(), kCanvas.w, kCanvas.h, 255, test::kGoldenTextMinPsnr);
}

CT_TEST_GOLDEN(annotation_blur) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    project::Clip& b = render_test::AddAnnotation(p, project::Annotation::Type::Blur, 0, 5'000'000);
    b.annotation->rect = RectF{0.2f, 0.28f, 0.3f, 0.32f};
    b.annotation->blur_px = 14;
    b.annotation->radius_px = 10;
    project::Clip& px_clip = render_test::AddAnnotation(p, project::Annotation::Type::Blur, 0, 5'000'000);
    px_clip.annotation->rect = RectF{0.6f, 0.15f, 0.3f, 0.45f};
    px_clip.annotation->blur_px = 12;
    px_clip.annotation->pixelate = true;
    px_clip.annotation->radius_px = 0;
    project::Clip& h = render_test::AddAnnotation(p, project::Annotation::Type::Highlight, 0, 5'000'000);
    h.annotation->rect = RectF{0.35f, 0.12f, 0.4f, 0.08f};
    h.annotation->fill = ColorF{1.f, 0.9f, 0.2f, 1};
    h.annotation->radius_px = 4;
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'000'000));
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 1'000'000, in, px));
    CT_GOLDEN_IMAGE(px.data(), kCanvas.w, kCanvas.h);
}

// Ctrl+Shift+S at 1.2 s, shown at 1.5 s (pill, bottom centre).
CT_TEST_GOLDEN(keystroke_badge) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    p.clips[0].cursor.visible = false;
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'500'000, &s->events));
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, 1'500'000, in, px));
    CT_GOLDEN_IMAGE_TOL(px.data(), kCanvas.w, kCanvas.h, 255, test::kGoldenTextMinPsnr);
}

// c1 (screen) -> c9 (slides) with a 500 ms crossfade around the cut at 2 s: u = 0.5 exactly at the cut.
CT_TEST_GOLDEN(transition_crossfade_u05) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    p.clips[0].out_us = 2'000'000;
    project::Clip b = p.clips[0];
    b.id = "c9";
    b.start_us = 2'000'000;
    b.in_us = 2'000'000;
    b.out_us = 4'000'000;
    p.clips.push_back(b);
    project::Transition tr;
    tr.track = "t1";
    tr.a = "c1";
    tr.b = "c9";
    tr.kind = project::Transition::Kind::Crossfade;
    tr.duration_us = 500'000;
    p.transitions.push_back(tr);
    const TimeUs t = 2'000'000;
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], render::Compositor::ClipMediaTime(p.clips[0], t)));
    in.layers.push_back(Layer(s->slides, &p.clips[1], render::Compositor::ClipMediaTime(p.clips[1], t)));
    render::Compositor::ActiveTransitions(p, t, in.active_transitions);
    CT_REQUIRE(in.active_transitions.size() == 1);
    std::vector<uint8_t> px;
    CT_REQUIRE(Render(s, p, t, in, px));
    CT_GOLDEN_IMAGE(px.data(), kCanvas.w, kCanvas.h);
}

CT_TEST_DEVICE(render_dip_and_slide_transitions_differ) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    p.clips[0].out_us = 2'000'000;
    project::Clip b = p.clips[0];
    b.id = "c9";
    b.start_us = 2'000'000;
    b.out_us = 4'000'000;
    p.clips.push_back(b);
    project::Transition tr;
    tr.track = "t1";
    tr.a = "c1";
    tr.b = "c9";
    tr.duration_us = 500'000;
    p.transitions.push_back(tr);
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'999'999));
    in.layers.push_back(Layer(s->slides, &p.clips[1], 2'000'000));
    render::Compositor::ActiveTransitions(p, 2'000'000, in.active_transitions);
    std::vector<uint8_t> dip, slide;
    p.transitions[0].kind = project::Transition::Kind::Dip;
    CT_REQUIRE(Render(s, p, 2'000'000, in, dip));
    // Dip to black at u = 0.5: the centre pixel is the dip colour.
    const size_t c = (static_cast<size_t>(kCanvas.h / 2) * kCanvas.w + kCanvas.w / 2) * 4;
    CT_CHECK(dip[c] < 3 && dip[c + 1] < 3 && dip[c + 2] < 3);
    p.transitions[0].kind = project::Transition::Kind::Slide;
    CT_REQUIRE(Render(s, p, 2'000'000, in, slide));
    CT_CHECK(std::memcmp(dip.data(), slide.data(), dip.size()) != 0);
}

// Same inputs -> identical bytes, 10 renders of a frame with every pass active.
CT_TEST_DEVICE(render_deterministic_10_frames) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(kCanvas);
    project::Clip cam = p.clips[0];
    cam.id = "c2";
    cam.track = "t2";
    cam.source = project::Clip::Source::Camera;
    cam.pip = project::Pip{};
    p.clips.push_back(cam);
    project::Clip& a = render_test::AddAnnotation(p, project::Annotation::Type::Callout, 0, 5'000'000);
    a.annotation->text = "Determinism";
    a.annotation->size_px = 20;
    a.annotation->rect = RectF{0.1f, 0.1f, 0.3f, 0.12f};
    project::Clip& bl = render_test::AddAnnotation(p, project::Annotation::Type::Blur, 0, 5'000'000);
    bl.annotation->rect = RectF{0.5f, 0.3f, 0.2f, 0.2f};
    p.camera.keys.push_back(project::CameraKey{0, 1.f, 0.5f, 0.5f, project::Ease::InOut, false});
    p.camera.keys.push_back(project::CameraKey{2'000'000, 1.6f, 0.4f, 0.5f, project::Ease::InOut, false});
    render::FrameInputs in;
    in.layers.push_back(Layer(s->screen, &p.clips[0], 1'250'000, &s->events));
    in.layers.push_back(Layer(s->camera, &p.clips[1], 1'250'000));
    std::vector<uint8_t> first, px;
    CT_REQUIRE(Render(s, p, 1'250'000, in, first));
    for (int i = 0; i < 9; ++i) {
        CT_REQUIRE(Render(s, p, 1'250'000, in, px));
        CT_CHECK(px == first);
    }
    // The shadow mask cache: the same frame again computes no new mask.
    const uint64_t before = render::effects::ShadowMaskRenders(s->fx.rc);
    CT_REQUIRE(Render(s, p, 1'250'000, in, px));
    CT_CHECK_EQ(render::effects::ShadowMaskRenders(s->fx.rc), before);
    // Text layers are cached as well.
    const uint64_t raster = s->fx.comp.Text().Rasterizations();
    CT_REQUIRE(Render(s, p, 1'250'000, in, px));
    CT_CHECK_EQ(s->fx.comp.Text().Rasterizations(), raster);
}

CT_TEST_DEVICE(render_target_size_must_match_canvas) {
    CT_SHARED_OR_SKIP(s);
    project::Project p = render_test::MakeProject(SizeI{1280, 720});
    platform::D3DDevice::Lock lock(s->fx.dev);
    CT_CHECK_EQ(s->fx.comp.RenderFrame(p, 0, {}, s->fx.rtv.Get()), E_INVALIDARG);
}
