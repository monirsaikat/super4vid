#include "render_fixture.h"

#include <ct/capture/color_convert.h>
#include <ct/render/cursor_renderer.h>
#include <ct/render/readback.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::render_test {

namespace {

uint8_t Clamp8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.f, 255.f) + 0.5f); }

void Put(std::vector<uint8_t>& px, int w, int x, int y, float r, float g, float b) {
    uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
    p[0] = Clamp8(b);
    p[1] = Clamp8(g);
    p[2] = Clamp8(r);
    p[3] = 255;
}

} // namespace

void MakePatternBgra(SizeI s, Pattern pat, std::vector<uint8_t>& px) {
    px.assign(static_cast<size_t>(s.w) * s.h * 4, 0);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            const float fx = static_cast<float>(x) / s.w, fy = static_cast<float>(y) / s.h;
            float r = 0, g = 0, b = 0;
            switch (pat) {
            case Pattern::Screen: {
                // A light "desktop app": dark title bar, sidebar, content with a hue strip, cards and a grid.
                r = g = b = 236;
                if (fy < 0.07f) r = g = b = 40;
                else if (fx < 0.18f) { r = 214; g = 220; b = 230; }
                else if (fy > 0.12f && fy < 0.22f) {   // hue strip
                    const float h = (fx - 0.18f) / 0.82f * 6.f;
                    const float k = h - std::floor(h / 2.f) * 2.f;
                    const float c = 1.f - std::fabs(k - 1.f);
                    const int seg = static_cast<int>(h);
                    const float rgb[6][3] = {{1, c, 0}, {c, 1, 0}, {0, 1, c}, {0, c, 1}, {c, 0, 1}, {1, 0, c}};
                    const int si = std::clamp(seg, 0, 5);
                    r = rgb[si][0] * 230;
                    g = rgb[si][1] * 230;
                    b = rgb[si][2] * 230;
                } else if (fy > 0.3f && fy < 0.7f && fx > 0.25f && fx < 0.55f) { r = 79; g = 140; b = 255; }
                else if (fy > 0.3f && fy < 0.7f && fx > 0.6f && fx < 0.9f) { r = 255; g = 176; b = 32; }
                if ((x % 64) == 0 || (y % 64) == 0) { r *= 0.85f; g *= 0.85f; b *= 0.85f; }
                break;
            }
            case Pattern::Camera: {
                const float dx = fx - 0.5f, dy = fy - 0.45f;
                r = 200 - 120 * fy;
                g = 140 - 60 * fy;
                b = 90 + 80 * fx;
                if (dx * dx * 1.8f + dy * dy < 0.04f) { r = 60; g = 45; b = 40; }   // "head"
                if (fy > 0.75f && std::fabs(dx) < 0.3f) { r = 50; g = 70; b = 120; }  // "shoulders"
                break;
            }
            case Pattern::Slides: {
                r = 20 + 40 * fx;
                g = 40 + 60 * fy;
                b = 120 + 100 * fx;
                if (((x + y) / 40) % 2 == 0) { r += 30; g += 30; b += 30; }
                if (fy > 0.4f && fy < 0.6f && fx > 0.2f && fx < 0.8f) { r = g = b = 245; }
                break;
            }
            }
            Put(px, s.w, x, y, r, g, b);
        }
}

bool MakeNv12(platform::D3DDevice& dev, SizeI s, Pattern pat, Nv12Frame& out) {
    std::vector<uint8_t> bgra;
    MakePatternBgra(s, pat, bgra);
    std::vector<uint8_t> nv12(static_cast<size_t>(s.w) * s.h * 3 / 2);
    capture::RgbToNv12Ref(bgra.data(), s.w, s.h, nv12.data(), nv12.data() + static_cast<size_t>(s.w) * s.h);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(s.w);
    td.Height = static_cast<UINT>(s.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{nv12.data(), static_cast<UINT>(s.w), 0};   // UV plane follows the Y plane
    out = {};
    if (FAILED(dev.Device()->CreateTexture2D(&td, &init, out.tex.GetAddressOf()))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(dev.Device()->CreateShaderResourceView(out.tex.Get(), &sd, out.y.GetAddressOf()))) return false;
    sd.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(dev.Device()->CreateShaderResourceView(out.tex.Get(), &sd, out.uv.GetAddressOf()))) return false;
    out.size = s;
    return true;
}

bool Fixture::Init(SizeI canvas, bool debug_layer) {
    size = canvas;
    if (FAILED(dev.Init(debug_layer))) return false;
    if (FAILED(rc.Init(dev))) return false;
    if (FAILED(comp.Init(rc))) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(canvas.w);
    td.Height = static_cast<UINT>(canvas.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev.Device()->CreateTexture2D(&td, nullptr, target.GetAddressOf()))) return false;
    if (FAILED(dev.Device()->CreateRenderTargetView(target.Get(), nullptr, rtv.GetAddressOf()))) return false;
    ready = true;
    return true;
}

Fixture::~Fixture() {
    comp.Shutdown();
    rc.Shutdown();
    rtv.Reset();
    target.Reset();
    dev.Shutdown();
}

HRESULT Fixture::RenderOnly(const project::Project& p, TimeUs t, const render::FrameInputs& in, const render::RenderOptions& o) {
    platform::D3DDevice::Lock lock(dev);
    return comp.RenderFrame(p, t, in, rtv.Get(), o);
}

bool Fixture::Render(const project::Project& p, TimeUs t, const render::FrameInputs& in, std::vector<uint8_t>& rgba,
                     const render::RenderOptions& o) {
    if (FAILED(RenderOnly(p, t, in, o))) return false;
    SizeI s;
    return SUCCEEDED(render::Readback::ToRgba8(dev, target.Get(), rgba, s)) && s.w == size.w && s.h == size.h;
}

uint64_t Fixture::DebugMessages() {
    ComPtr<ID3D11InfoQueue> q;
    if (!dev.Device() || FAILED(dev.Device()->QueryInterface(IID_PPV_ARGS(q.GetAddressOf())))) return 0;
    return q->GetNumStoredMessages();
}

void Fixture::PrintDebugMessages() {
    ComPtr<ID3D11InfoQueue> q;
    if (!dev.Device() || FAILED(dev.Device()->QueryInterface(IID_PPV_ARGS(q.GetAddressOf())))) return;
    for (UINT64 i = 0; i < q->GetNumStoredMessages(); ++i) {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::vector<uint8_t> buf(len);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (SUCCEEDED(q->GetMessage(i, m, &len))) std::printf("  d3d11: %.*s\n", static_cast<int>(m->DescriptionByteLength), m->pDescription);
    }
}

project::Project MakeProject(SizeI canvas) {
    project::Project p;
    p.id = "test";
    p.name = "render test";
    p.canvas.width = canvas.w;
    p.canvas.height = canvas.h;
    const float k = static_cast<float>(canvas.h) / 1080.f;
    p.canvas.padding_px = static_cast<int>(std::lround(96 * k));
    p.canvas.corner_radius_px = static_cast<int>(std::lround(36 * k));
    p.canvas.shadow.blur_px = 48 * k;
    p.canvas.shadow.offset_y_px = 24 * k;
    p.canvas.shadow.opacity = 0.45f;
    p.canvas.background.kind = project::Background::Kind::Gradient;
    p.canvas.background.color_a = ColorF{0.31f, 0.20f, 0.55f, 1};
    p.canvas.background.color_b = ColorF{0.06f, 0.30f, 0.45f, 1};
    p.canvas.background.angle_deg = 135;
    p.camera.mode = project::Camera::Mode::Manual;

    project::Media m;
    m.id = "m1";
    m.kind = project::Media::Kind::Recording;
    m.duration_us = 10 * kUsPerSecond;
    m.width = 1280;
    m.height = 720;
    m.fps_num = 60;
    m.has_events = true;
    m.has_camera = true;
    p.media.push_back(m);

    const char* names[3] = {"Screen", "Camera", "Callouts"};
    const project::Track::Kind kinds[3] = {project::Track::Kind::Video, project::Track::Kind::Video, project::Track::Kind::Annotations};
    for (int i = 0; i < 3; ++i) {
        project::Track t;
        t.id = "t" + std::to_string(i + 1);
        t.kind = kinds[i];
        t.name = names[i];
        p.tracks.push_back(t);
    }
    project::Clip c;
    c.id = "c1";
    c.track = "t1";
    c.media = "m1";
    c.source = project::Clip::Source::Screen;
    c.start_us = 0;
    c.in_us = 0;
    c.out_us = 10 * kUsPerSecond;
    p.clips.push_back(c);
    p.RecomputeNextId();
    return p;
}

project::Clip& AddAnnotation(project::Project& p, project::Annotation::Type type, TimeUs start, TimeUs end) {
    project::Clip c;
    c.id = p.NewId("c");
    c.track = "t3";
    c.start_us = start;
    c.in_us = 0;
    c.out_us = end - start;
    project::Annotation a;
    a.type = type;
    c.annotation = a;
    p.clips.push_back(c);
    return p.clips.back();
}

bool MakeEvents(events::EventLog& log) {
    events::EventHeader h;
    h.qpc_frequency = 10'000'000;
    h.region_w = 1280;
    h.region_h = 720;
    h.flags = events::kFlagKeystrokes;
    events::EventWriter w;
    w.OpenMemory(h);
    std::vector<uint8_t> arrow;
    int aw = 0, ah = 0, hx = 0, hy = 0;
    render::CursorRenderer::MakeArrowShape(arrow, aw, ah, hx, hy);
    const uint32_t shape = w.CursorShape(0, 0, static_cast<uint16_t>(aw), static_cast<uint16_t>(ah), static_cast<int16_t>(hx),
                                         static_cast<int16_t>(hy), arrow.data());
    for (TimeUs t = 0; t <= 900'000; t += 8'333) {
        const float u = static_cast<float>(t) / 900'000.f;
        const float e = u * u * (3 - 2 * u);
        w.CursorMove(t, static_cast<int32_t>(std::lround(300 + 340 * e)), static_cast<int32_t>(std::lround(200 + 160 * e)), shape, true);
    }
    w.CursorMove(900'000, 640, 360, shape, true);
    w.MouseButton(1'000'000, events::Button::Left, true, 640, 360);
    w.MouseButton(1'080'000, events::Button::Left, false, 640, 360);
    auto key = [&](TimeUs t, uint16_t vk, bool down, uint8_t mods, const char* label) {
        events::KeyPayload k;
        k.vk = vk;
        k.down = down ? 1 : 0;
        k.modifiers = mods;
        events::CopyLabel(k.label, sizeof(k.label), label);
        w.Key(t, k);
    };
    key(1'150'000, 0x11, true, 0, "Ctrl");
    key(1'170'000, 0x10, true, events::kModCtrl, "Shift");
    key(1'200'000, 0x53, true, events::kModCtrl | events::kModShift, "S");
    key(1'260'000, 0x53, false, events::kModCtrl | events::kModShift, "S");
    key(1'280'000, 0x10, false, events::kModCtrl, "Shift");
    key(1'290'000, 0x11, false, 0, "Ctrl");
    Err err;
    return log.LoadFromMemory(w.Bytes(), &err);
}

render::VideoLayerInput Layer(const Nv12Frame& f, const project::Clip* clip, TimeUs media_t, const events::EventLog* ev) {
    render::VideoLayerInput l;
    l.y_srv = f.y.Get();
    l.uv_srv = f.uv.Get();
    l.video_size = f.size;
    l.media_t_us = media_t;
    l.clip = clip;
    l.events = ev;
    return l;
}

} // namespace ct::render_test
