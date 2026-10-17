// Performance: 1080p, 3 video layers (screen with shadow, a second video, a webcam PIP) + cursor + 2 annotations
// (callout text, blur region), 300 frames. Reports GPU (timestamp queries) and CPU (RenderFrame call) averages and,
// in Debug, the heap allocations made after warm-up (CRT alloc hook). Also a Debug-layer cleanliness test.
#include <ct/render/compositor.h>
#include <ct/test/ct_test.h>

#include "render_fixture.h"

#include <crtdbg.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace ct;
using render_test::Fixture;
using render_test::Layer;
using render_test::Nv12Frame;
using render_test::Pattern;

namespace {

#ifdef _DEBUG
long g_allocs = 0;
int AllocHook(int type, void*, size_t, int, long, const unsigned char*, int) {
    if (type == _HOOK_ALLOC || type == _HOOK_REALLOC) ++g_allocs;
    return TRUE;
}
#endif

struct Scene {
    project::Project p;
    Nv12Frame screen, video, camera;
    events::EventLog events;
    render::FrameInputs in;
};

bool BuildScene(Fixture& fx, Scene& s) {
    s.p = render_test::MakeProject(SizeI{1920, 1080});
    project::Media m2 = s.p.media[0];
    m2.id = "m2";
    m2.kind = project::Media::Kind::Video;
    s.p.media.push_back(m2);
    project::Track t4;
    t4.id = "t4";
    t4.kind = project::Track::Kind::Video;
    t4.name = "B-roll";
    s.p.tracks.insert(s.p.tracks.begin() + 1, t4);
    project::Clip v = s.p.clips[0];
    v.id = "c4";
    v.track = "t4";
    v.media = "m2";
    v.source = project::Clip::Source::Default;
    v.transform.scale = 0.35f;
    v.transform.x = -0.28f;
    v.transform.y = -0.25f;
    v.corner_radius_px = 16;
    v.shadow = project::Shadow{};
    s.p.clips.push_back(v);
    project::Clip cam = s.p.clips[0];
    cam.id = "c2";
    cam.track = "t2";
    cam.source = project::Clip::Source::Camera;
    project::Pip pip;
    pip.mask = project::Pip::Mask::Circle;
    cam.pip = pip;
    s.p.clips.push_back(cam);
    project::Clip& a = render_test::AddAnnotation(s.p, project::Annotation::Type::Callout, 0, 10'000'000);
    a.annotation->text = "Click here";
    a.annotation->rect = RectF{0.6f, 0.2f, 0.25f, 0.1f};
    a.annotation->pointer = project::PointF{0.55f, 0.35f};
    project::Clip& b = render_test::AddAnnotation(s.p, project::Annotation::Type::Blur, 0, 10'000'000);
    b.annotation->rect = RectF{0.15f, 0.55f, 0.25f, 0.2f};
    if (!render_test::MakeNv12(fx.dev, SizeI{1920, 1080}, Pattern::Screen, s.screen)) return false;
    if (!render_test::MakeNv12(fx.dev, SizeI{1920, 1080}, Pattern::Slides, s.video)) return false;
    if (!render_test::MakeNv12(fx.dev, SizeI{1280, 720}, Pattern::Camera, s.camera)) return false;
    if (!render_test::MakeEvents(s.events)) return false;
    s.in.layers.push_back(Layer(s.screen, s.p.FindClip("c1"), 0, &s.events));
    s.in.layers.push_back(Layer(s.video, s.p.FindClip("c4"), 0));
    s.in.layers.push_back(Layer(s.camera, s.p.FindClip("c2"), 0));
    s.in.layers.reserve(8);
    return true;
}

void SetTime(Scene& s, TimeUs t) {
    for (auto& l : s.in.layers) l.media_t_us = t;
}

} // namespace

CT_BENCH(compositor_1080p_3layers_cursor_2annotations, 300) {
    Fixture fx;
    if (!fx.Init(SizeI{1920, 1080}, false)) {
        std::printf("  no device\n");
        return;
    }
    Scene s;
    if (!BuildScene(fx, s)) {
        std::printf("  scene setup failed\n");
        return;
    }
    render::RenderOptions opt;
    opt.gpu_timing = true;
    // Warm-up: text rasters, shadow masks, temps, cursor atlas.
    for (int i = 0; i < 30; ++i) {
        SetTime(s, 900'000 + i * 16'667);
        fx.RenderOnly(s.p, 900'000 + i * 16'667, s.in, opt);
    }
    std::vector<double> cpu, gpu;
    cpu.reserve(static_cast<size_t>(CT_BENCH_N));
    gpu.reserve(static_cast<size_t>(CT_BENCH_N));
#ifdef _DEBUG
    g_allocs = 0;
    _CRT_ALLOC_HOOK old = _CrtSetAllocHook(AllocHook);
#endif
    // The cursor moves and clicks during the run (900 ms .. 6 s), so cursor and ripple passes run every frame.
    for (int64_t i = 0; i < CT_BENCH_N; ++i) {
        const TimeUs t = 900'000 + static_cast<TimeUs>(i) * 16'667;
        SetTime(s, t);
        fx.RenderOnly(s.p, t, s.in, opt);
        const render::RenderTiming& rt = fx.comp.LastTiming();
        cpu.push_back(rt.total_ms);
        if (rt.gpu_total_ms >= 0) gpu.push_back(rt.gpu_total_ms);
        if ((i & 15) == 15) {   // keep the GPU at most a few frames behind, like a presenting preview
            platform::D3DDevice::Lock lock(fx.dev);
            fx.dev.Context()->Flush();
        }
    }
#ifdef _DEBUG
    _CrtSetAllocHook(old);
#endif
    auto avg = [](const std::vector<double>& v) {
        double sum = 0;
        for (double x : v) sum += x;
        return v.empty() ? -1.0 : sum / static_cast<double>(v.size());
    };
    auto p95 = [](std::vector<double> v) {
        if (v.empty()) return -1.0;
        std::sort(v.begin(), v.end());
        return v[v.size() * 95 / 100];
    };
    std::printf("  frames %lld: CPU avg %.3f ms (p95 %.3f), GPU avg %.3f ms (p95 %.3f, %zu samples)\n",
                static_cast<long long>(CT_BENCH_N), avg(cpu), p95(cpu), avg(gpu), p95(gpu), gpu.size());
#ifdef _DEBUG
    std::printf("  heap allocations after warm-up: %ld\n", g_allocs);
#endif
}

// Debug builds: a full frame (all passes) under the D3D11 debug layer produces no messages, and RenderFrame allocates
// nothing after warm-up.
CT_TEST_DEVICE(render_debug_layer_clean_and_no_allocs) {
    Fixture fx;
#ifdef _DEBUG
    if (!fx.Init(SizeI{1920, 1080}, true)) CT_SKIP("no D3D11 device");
    if (!fx.dev.DebugLayerActive()) CT_SKIP("D3D11 debug layer not installed");
#else
    if (!fx.Init(SizeI{1920, 1080}, false)) CT_SKIP("no D3D11 device");
#endif
    Scene s;
    CT_REQUIRE(BuildScene(fx, s));
    render::RenderOptions opt;
    opt.gpu_timing = true;
    for (int i = 0; i < 20; ++i) {
        SetTime(s, 900'000 + i * 16'667);
        CT_CHECK_HR(fx.RenderOnly(s.p, 900'000 + i * 16'667, s.in, opt));
    }
#ifdef _DEBUG
    g_allocs = 0;
    _CRT_ALLOC_HOOK old = _CrtSetAllocHook(AllocHook);
    for (int i = 0; i < 30; ++i) {
        const TimeUs t = 1'300'000 + i * 16'667;
        SetTime(s, t);
        fx.RenderOnly(s.p, t, s.in, opt);
    }
    _CrtSetAllocHook(old);
    CT_CHECK_EQ(g_allocs, 0L);
#endif
    std::vector<uint8_t> px;
    CT_REQUIRE(fx.Render(s.p, 1'000'000, s.in, px));
    const uint64_t msgs = fx.DebugMessages();
    if (msgs) fx.PrintDebugMessages();
    CT_CHECK_EQ(msgs, 0ull);
}
