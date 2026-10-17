#pragma once
// tests/common/test_project.h
// Synthetic recording bundles and projects built from the tests/data pattern media (TimelinePlayer and export
// tests, export_demo): a .ctrec folder with meta.json, screen.mp4 (a copy of the pattern, optionally cam.mp4) and a
// synthetic events.ctev (cursor path, clicks, shortcuts) for a region of the pattern's size.

#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/events/ctev.h>
#include <ct/platform/win.h>
#include <ct/project/project.h>
#include <ct/render/cursor_renderer.h>

#include <cmath>
#include <string>
#include <vector>

namespace ct::test {

// Cursor sweeps on a Lissajous path over the whole duration (120 Hz), a left click every 2 s, Ctrl+S every 5 s.
inline bool WriteSyntheticEvents(const std::string& path, int w, int h, TimeUs duration_us) {
    events::EventHeader hd;
    hd.qpc_frequency = 10'000'000;
    hd.region_w = w;
    hd.region_h = h;
    hd.flags = events::kFlagKeystrokes;
    events::EventWriter wr;
    Err err;
    if (!wr.Open(path, hd, &err)) return false;
    std::vector<uint8_t> arrow;
    int aw = 0, ah = 0, hx = 0, hy = 0;
    render::CursorRenderer::MakeArrowShape(arrow, aw, ah, hx, hy);
    const uint32_t shape = wr.CursorShape(0, 0, static_cast<uint16_t>(aw), static_cast<uint16_t>(ah), static_cast<int16_t>(hx),
                                          static_cast<int16_t>(hy), arrow.data());
    TimeUs next_click = 1'000'000, next_key = 2'500'000;
    for (TimeUs t = 0; t <= duration_us; t += 8'333) {
        const double s = static_cast<double>(t) / 1e6;
        const int x = static_cast<int>(w * (0.5 + 0.4 * std::sin(s * 0.9)));
        const int y = static_cast<int>(h * (0.5 + 0.35 * std::sin(s * 1.3 + 0.5)));
        wr.CursorMove(t, x, y, shape, true);
        if (t >= next_click) {
            wr.MouseButton(t, events::Button::Left, true, x, y);
            wr.MouseButton(t + 80'000, events::Button::Left, false, x, y);
            next_click += 2'000'000;
        }
        if (t >= next_key) {
            events::KeyPayload k;
            k.vk = 0x53;
            k.down = 1;
            k.modifiers = events::kModCtrl;
            events::CopyLabel(k.label, sizeof(k.label), "S");
            wr.Key(t, k);
            k.down = 0;
            wr.Key(t + 60'000, k);
            next_key += 5'000'000;
        }
    }
    wr.Close();
    return true;
}

// Creates <dir> (deleted first) as a recording bundle whose screen.mp4 (and cam.mp4 when camera) are copies of
// `pattern` (w x h, duration_us; audio stream = "mic").
inline bool MakeBundle(const std::string& dir, const std::string& pattern, int w, int h, TimeUs duration_us, bool camera,
                       bool events = true) {
    fs::DeleteTree(dir);
    if (!fs::CreateDirectories(dir)) return false;
    const std::wstring src = fs::ToWinPath(fs::Absolute(pattern));
    if (!CopyFileW(src.c_str(), fs::ToWinPath(PathJoin(dir, "screen.mp4")).c_str(), FALSE)) return false;
    if (camera && !CopyFileW(src.c_str(), fs::ToWinPath(PathJoin(dir, "cam.mp4")).c_str(), FALSE)) return false;
    std::string meta = Format(
        "{\"format\":\"ctrec\",\"version\":1,\"duration_us\":%lld,"
        "\"screen\":{\"file\":\"screen.mp4\",\"width\":%d,\"height\":%d,\"fps_num\":60,\"fps_den\":1,"
        "\"region\":{\"x\":0,\"y\":0,\"w\":%d,\"h\":%d}},",
        static_cast<long long>(duration_us), w, h, w, h);
    meta += camera ? Format("\"camera\":{\"file\":\"cam.mp4\",\"width\":%d,\"height\":%d},", w, h) : std::string("\"camera\":null,");
    meta += "\"audio\":{\"mic\":{\"stream_index\":1},\"system\":null},";
    meta += events ? "\"events\":{\"file\":\"events.ctev\"},\"pauses\":[]}" : "\"events\":null,\"pauses\":[]}";
    if (!fs::WriteAtomic(PathJoin(dir, "meta.json"), meta)) return false;
    if (events && !WriteSyntheticEvents(PathJoin(dir, "events.ctev"), w, h, duration_us)) return false;
    return true;
}

// A 1920x1080@60 project: background gradient, tracks t1 Screen (video), t2 Camera (video), t3 Overlay (video),
// t4 Callouts (annotations), t5 Mic (audio). Media m1 = the bundle (recording). No clips.
inline project::Project MakeBaseProject(const std::string& bundle, int w, int h, TimeUs media_duration_us) {
    project::Project p;
    p.id = "test-project";
    p.name = "export test";
    p.canvas.width = 1920;
    p.canvas.height = 1080;
    p.canvas.fps_num = 60;
    p.canvas.fps_den = 1;
    p.camera.mode = project::Camera::Mode::Manual;
    project::Media m;
    m.id = "m1";
    m.kind = project::Media::Kind::Recording;
    m.path = fs::Absolute(bundle);
    m.duration_us = media_duration_us;
    m.width = w;
    m.height = h;
    m.fps_num = 60;
    m.audio_streams = {"mic"};
    m.has_events = true;
    m.has_camera = true;
    p.media.push_back(m);
    const char* names[5] = {"Screen", "Camera", "Overlay", "Callouts", "Mic"};
    const project::Track::Kind kinds[5] = {project::Track::Kind::Video, project::Track::Kind::Video, project::Track::Kind::Video,
                                           project::Track::Kind::Annotations, project::Track::Kind::Audio};
    for (int i = 0; i < 5; ++i) {
        project::Track t;
        t.id = "t" + std::to_string(i + 1);
        t.kind = kinds[i];
        t.name = names[i];
        p.tracks.push_back(t);
    }
    p.next_id = 100;
    return p;
}

inline project::Clip MakeClip(const char* id, const char* track, const char* media, project::Clip::Source src, TimeUs start,
                              TimeUs in, TimeUs out) {
    project::Clip c;
    c.id = id;
    c.track = track;
    c.media = media;
    c.source = src;
    c.start_us = start;
    c.in_us = in;
    c.out_us = out;
    return c;
}

// 3 video layers (screen with cursor + keystrokes, camera PIP, a scaled overlay) looped from the pattern, two callout
// annotations, the mic audio; duration_us long.
inline project::Project MakeExportProject(const std::string& bundle, int w, int h, TimeUs media_duration_us, TimeUs duration_us) {
    project::Project p = MakeBaseProject(bundle, w, h, media_duration_us);
    int n = 0;
    for (TimeUs t = 0; t < duration_us; t += media_duration_us, ++n) {
        const TimeUs len = std::min(media_duration_us, duration_us - t);
        const std::string k = std::to_string(n);
        project::Clip s = MakeClip(("s" + k).c_str(), "t1", "m1", project::Clip::Source::Screen, t, 0, len);
        p.clips.push_back(s);
        project::Clip c = MakeClip(("k" + k).c_str(), "t2", "m1", project::Clip::Source::Camera, t, 0, len);
        c.pip = project::Pip{};
        p.clips.push_back(c);
        project::Clip o = MakeClip(("o" + k).c_str(), "t3", "m1", project::Clip::Source::Screen, t, 0, len);
        o.transform.scale = 0.3f;
        o.transform.x = -0.3f;
        o.transform.y = -0.3f;
        o.transform.opacity = 0.9f;
        p.clips.push_back(o);
        project::Clip a = MakeClip(("a" + k).c_str(), "t5", "m1", project::Clip::Source::Mic, t, 0, len);
        p.clips.push_back(a);
    }
    for (int i = 0; i < 2; ++i) {
        project::Clip c;
        c.id = "n" + std::to_string(i);
        c.track = "t4";
        c.start_us = i == 0 ? 0 : duration_us / 2;
        c.in_us = 0;
        c.out_us = duration_us / 2;
        project::Annotation a;
        a.type = i == 0 ? project::Annotation::Type::Callout : project::Annotation::Type::Text;
        a.text = i == 0 ? "Click here" : "Export test";
        a.rect = i == 0 ? RectF{0.6f, 0.2f, 0.25f, 0.1f} : RectF{0.1f, 0.8f, 0.3f, 0.08f};
        c.annotation = a;
        p.clips.push_back(c);
    }
    return p;
}

} // namespace ct::test
