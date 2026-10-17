// ct/render/layer_math.h, cursor smoothing and atlas packing (pure logic).
#include <ct/render/cursor_renderer.h>
#include <ct/render/layer_math.h>
#include <ct/test/ct_test.h>

#include "render_fixture.h"

#include <cmath>

using namespace ct;
using render::Mat3;
using render::PointF;
namespace lm = render::layer_math;

CT_TEST(render_mat3_inverse_roundtrip) {
    const Mat3 m = Mat3::Translate(37.f, -12.f) * Mat3::Rotate(23.f) * Mat3::Scale(1.7f, 1.7f) * Mat3::Translate(-5.f, 9.f);
    const Mat3 inv = m.Inverse();
    for (float x = -100; x <= 100; x += 50)
        for (float y = -100; y <= 100; y += 50) {
            const PointF p = inv.Apply(m.Apply(PointF{x, y}));
            CT_CHECK_NEAR(p.x, x, 1e-3);
            CT_CHECK_NEAR(p.y, y, 1e-3);
        }
    CT_CHECK_NEAR(m.ScaleFactor(), 1.7, 1e-5);
    // Rotate(90) turns +x into +y (clockwise on a y-down screen).
    const PointF r = Mat3::Rotate(90).Apply(PointF{1, 0});
    CT_CHECK_NEAR(r.x, 0, 1e-6);
    CT_CHECK_NEAR(r.y, 1, 1e-6);
}

CT_TEST(render_fit_contain_and_stage) {
    project::Canvas c;
    c.width = 1920;
    c.height = 1080;
    c.padding_px = 96;
    const RectF stage = lm::StageRect(c);
    CT_CHECK_NEAR(stage.x, 96, 1e-6);
    CT_CHECK_NEAR(stage.w, 1728, 1e-6);
    CT_CHECK_NEAR(stage.h, 888, 1e-6);
    const RectF f = lm::FitContain(SizeI{1920, 1080}, stage);   // limited by height
    CT_CHECK_NEAR(f.h, 888, 1e-4);
    CT_CHECK_NEAR(f.w, 888.f * 16.f / 9.f, 1e-3);
    CT_CHECK_NEAR(f.x + f.w * 0.5f, 960, 1e-3);
    CT_CHECK_NEAR(f.y, 96, 1e-4);
    const RectF tall = lm::FitContain(SizeI{1080, 1920}, RectF{0, 0, 1920, 1080});
    CT_CHECK_NEAR(tall.h, 1080, 1e-4);
    CT_CHECK_NEAR(tall.x, (1920 - 1080.f * 1080.f / 1920.f) * 0.5f, 1e-3);
}

CT_TEST(render_clip_transform_offsets_scale_rotation) {
    project::Canvas cv;
    project::Clip clip;
    clip.transform.x = 0.1f;
    clip.transform.y = -0.05f;
    clip.transform.scale = 0.5f;
    const RectF base{100, 100, 400, 200};
    const Mat3 m = lm::ClipTransform(clip, 0, base, cv);
    const PointF c = m.Apply(PointF{300, 200});   // centre moves by the canvas-fraction offset
    CT_CHECK_NEAR(c.x, 300 + 192, 1e-3);
    CT_CHECK_NEAR(c.y, 200 - 54, 1e-3);
    const PointF tl = m.Apply(PointF{100, 100});   // scaled about the centre
    CT_CHECK_NEAR(tl.x, 492 - 100, 1e-3);
    CT_CHECK_NEAR(tl.y, 146 - 50, 1e-3);
    // Keyframes override the static value without allocating.
    clip.start_us = 1000;
    clip.keyframes["transform.scale"].Set(0, 1.f, project::Ease::Linear);
    clip.keyframes["transform.scale"].Set(1000, 2.f, project::Ease::Linear);
    CT_CHECK_NEAR(lm::PropAt(clip, "transform.scale", 1500, 9.f), 1.5, 1e-6);
    CT_CHECK_NEAR(lm::PropAt(clip, "transform.opacity", 1500, 0.25f), 0.25, 1e-6);
}

CT_TEST(render_camera_transform_zoom_and_clamp) {
    project::Canvas cv;
    project::Camera cam;
    cam.keys.push_back(project::CameraKey{0, 2.f, 0.1f, 0.5f, project::Ease::Linear, false});
    const project::Camera::View v = cam.At(0);
    CT_CHECK_NEAR(v.cx, 0.25, 1e-6);   // clamped so the window stays inside the canvas
    const Mat3 m = lm::CameraTransform(v, cv);
    const PointF left = m.Apply(PointF{0, 540});
    CT_CHECK_NEAR(left.x, 0, 1e-3);   // the canvas' left edge stays on the left edge
    const PointF centre = m.Apply(PointF{0.25f * 1920, 540});
    CT_CHECK_NEAR(centre.x, 960, 1e-3);
    CT_CHECK_NEAR(m.ScaleFactor(), 2, 1e-6);
    const Mat3 id = lm::CameraTransform(project::Camera::View{1, 0.5f, 0.5f}, cv);
    const PointF q = id.Apply(PointF{123, 456});
    CT_CHECK_NEAR(q.x, 123, 1e-3);
    CT_CHECK_NEAR(q.y, 456, 1e-3);
}

CT_TEST(render_pip_rect_corners) {
    project::Canvas cv;
    project::Pip pip;
    pip.size = 0.25f;
    pip.margin_px = 32;
    pip.corner = project::Pip::Corner::BR;
    pip.mask = project::Pip::Mask::Rounded;
    RectF r = lm::PipRect(pip, SizeI{1280, 720}, cv);
    CT_CHECK_NEAR(r.w, 480, 1e-3);
    CT_CHECK_NEAR(r.h, 270, 1e-3);
    CT_CHECK_NEAR(r.x + r.w, 1920 - 32, 1e-3);
    CT_CHECK_NEAR(r.y + r.h, 1080 - 32, 1e-3);
    pip.mask = project::Pip::Mask::Circle;
    pip.corner = project::Pip::Corner::TL;
    r = lm::PipRect(pip, SizeI{1280, 720}, cv);
    CT_CHECK_NEAR(r.w, r.h, 1e-3);
    CT_CHECK_NEAR(r.x, 32, 1e-3);
    CT_CHECK_NEAR(r.y, 32, 1e-3);
}

CT_TEST(render_compositor_video_to_canvas_maps_corners) {
    project::Project p = render_test::MakeProject(SizeI{1920, 1080});
    render::Compositor comp;   // geometry queries need no device
    const project::Clip& c = p.clips[0];
    const RectF r = comp.LayerRectOnCanvas(p, c, 0, SizeI{1280, 720});
    const Mat3 m = comp.VideoToCanvas(p, c, 0, SizeI{1280, 720});
    const PointF tl = m.Apply(PointF{0, 0}), br = m.Apply(PointF{1280, 720});
    CT_CHECK_NEAR(tl.x, r.x, 1e-2);
    CT_CHECK_NEAR(tl.y, r.y, 1e-2);
    CT_CHECK_NEAR(br.x, r.x + r.w, 1e-2);
    CT_CHECK_NEAR(br.y, r.y + r.h, 1e-2);
    CT_CHECK_NEAR(r.y, 96, 1e-3);
}

CT_TEST(render_transition_progress) {
    project::Project p = render_test::MakeProject(SizeI{640, 360});
    p.clips[0].out_us = 2 * kUsPerSecond;
    project::Clip b = p.clips[0];
    b.id = "c9";
    b.start_us = 2 * kUsPerSecond;
    p.clips.push_back(b);
    project::Transition tr;
    tr.track = "t1";
    tr.a = "c1";
    tr.b = "c9";
    tr.duration_us = 500'000;
    p.transitions.push_back(tr);
    float u = -1;
    CT_CHECK(render::Compositor::TransitionProgress(p, p.transitions[0], 2'000'000, u));
    CT_CHECK_NEAR(u, 0.5, 1e-6);
    CT_CHECK(!render::Compositor::TransitionProgress(p, p.transitions[0], 1'700'000, u));
    std::vector<std::pair<const project::Clip*, const project::Transition*>> active;
    render::Compositor::ActiveTransitions(p, 1'800'000, active);
    CT_REQUIRE(active.size() == 1);
    CT_CHECK(active[0].first == p.FindClip("c9"));
    CT_CHECK_EQ(render::Compositor::ClipMediaTime(p.clips[0], 2'100'000), 2 * kUsPerSecond - 1);
}

CT_TEST(render_smoothed_position_constant_velocity_stays_on_line) {
    events::EventHeader h;
    h.region_w = 2000;
    h.region_h = 1000;
    events::EventWriter w;
    w.OpenMemory(h);
    for (int k = 0; k <= 240; ++k)   // constant velocity: 6 px right, 3 px down per 1/120 s
        w.CursorMove(static_cast<TimeUs>(k) * 8'333, 6 * k, 3 * k, 0, true);
    events::EventLog log;
    Err err;
    CT_REQUIRE(log.LoadFromMemory(w.Bytes(), &err));
    for (TimeUs t = 300'000; t <= 1'700'000; t += 77'777) {
        const events::CursorState lin = log.CursorAt(t);
        for (float s : {0.f, 0.35f, 1.f}) {
            const PointF p = render::CursorRenderer::SmoothedPosition(log, t, s);
            CT_CHECK_NEAR(p.x, lin.x, 0.01);
            CT_CHECK_NEAR(p.y, lin.y, 0.01);
            CT_CHECK_NEAR(p.y, p.x * 0.5f, 0.01);   // on the line y = x / 2
        }
    }
    // smoothing 0 equals the linear interpolation exactly.
    const PointF z = render::CursorRenderer::SmoothedPosition(log, 1'234'567, 0.f);
    const events::CursorState c = log.CursorAt(1'234'567);
    CT_CHECK_EQ(z.x, c.x);
    CT_CHECK_EQ(z.y, c.y);
}

CT_TEST(render_smoothed_position_snaps_to_click) {
    events::EventLog log;
    CT_REQUIRE(render_test::MakeEvents(log));
    const PointF at = render::CursorRenderer::SmoothedPosition(log, 1'000'000, 1.f);
    CT_CHECK_NEAR(at.x, 640, 1e-3);
    CT_CHECK_NEAR(at.y, 360, 1e-3);
    // A step: the smoothed path rounds it off (lags before, leads after) but never overshoots.
    const PointF mid = render::CursorRenderer::SmoothedPosition(log, 450'000, 1.f);
    CT_CHECK(mid.x > 300 && mid.x < 640);
}

CT_TEST(render_shelf_packer) {
    render::ShelfPacker p(64, 64, 2);
    int x = 0, y = 0;
    CT_CHECK(p.Insert(20, 10, x, y));
    CT_CHECK_EQ(x, 2);
    CT_CHECK_EQ(y, 2);
    CT_CHECK(p.Insert(20, 16, x, y));
    CT_CHECK_EQ(x, 26);
    CT_CHECK_EQ(y, 2);
    CT_CHECK(p.Insert(20, 8, x, y));   // 50 + 24 > 64: next shelf below the tallest (16 + 4)
    CT_CHECK_EQ(x, 2);
    CT_CHECK_EQ(y, 22);
    CT_CHECK(!p.Insert(70, 4, x, y));  // wider than the atlas
    CT_CHECK(p.Insert(60, 20, x, y));
    CT_CHECK_EQ(y, 34);
    CT_CHECK(!p.Insert(60, 20, x, y)); // full
    p.Reset();
    CT_CHECK(p.Insert(60, 60, x, y));
    // 2048 atlas holds 1,000 32x32 shapes (36x36 cells: 56 per shelf, 56 shelves).
    render::ShelfPacker big(2048, 2048, 2);
    int n = 0;
    while (n < 4000 && big.Insert(32, 32, x, y)) ++n;
    CT_CHECK_EQ(n, 56 * 56);
}
