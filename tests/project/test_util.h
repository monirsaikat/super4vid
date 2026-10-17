#pragma once
// Helpers shared by the ct_project tests: a seeded sample project built directly from the model types.

#include <ct/project/project.h>

#include <string>

namespace ct::project::testutil {

inline Media MakeMedia(const char* id, Media::Kind kind, TimeUs duration_us, const char* path) {
    Media m;
    m.id = id;
    m.kind = kind;
    m.path = path;
    m.duration_us = duration_us;
    if (kind != Media::Kind::Audio) {
        m.width = 1920;
        m.height = 1080;
        m.fps_num = kind == Media::Kind::Image ? 0 : 60;
        m.fps_den = 1;
    }
    return m;
}

inline Track MakeTrack(const char* id, Track::Kind kind, const char* name) {
    Track t;
    t.id = id;
    t.kind = kind;
    t.name = name;
    return t;
}

inline Clip MakeClip(const char* id, const char* track, const char* media, Clip::Source source, TimeUs start_us,
                     TimeUs in_us, TimeUs out_us) {
    Clip c;
    c.id = id;
    c.track = track;
    c.media = media;
    c.source = source;
    c.start_us = start_us;
    c.in_us = in_us;
    c.out_us = out_us;
    return c;
}

// m1 recording 60 s (mic, system, camera, events), m2 video 20 s, m3 audio 30 s, m4 image.
// t1 Screen, t2 Camera, t3 Mic, t4 System, t5 Music (audio), t6 Callouts (annotations).
inline Project MakeSampleProject() {
    Project p;
    p.id = "7c1d4e6a-0000-4000-8000-000000000001";
    p.name = "Sample";
    Media rec = MakeMedia("m1", Media::Kind::Recording, 60'000'000, "C:/rec/2026-10-17 12-03-44.ctrec");
    rec.audio_streams = {"mic", "system"};
    rec.has_events = true;
    rec.has_camera = true;
    p.media.push_back(rec);
    p.media.push_back(MakeMedia("m2", Media::Kind::Video, 20'000'000, "C:/media/clip.mp4"));
    Media audio = MakeMedia("m3", Media::Kind::Audio, 30'000'000, "C:/media/music.m4a");
    audio.audio_streams = {"default"};
    p.media.push_back(audio);
    p.media.push_back(MakeMedia("m4", Media::Kind::Image, 0, "C:/media/logo.png"));
    p.tracks.push_back(MakeTrack("t1", Track::Kind::Video, "Screen"));
    p.tracks.push_back(MakeTrack("t2", Track::Kind::Video, "Camera"));
    p.tracks.push_back(MakeTrack("t3", Track::Kind::Audio, "Mic"));
    p.tracks.push_back(MakeTrack("t4", Track::Kind::Audio, "System"));
    p.tracks.push_back(MakeTrack("t5", Track::Kind::Audio, "Music"));
    p.tracks.push_back(MakeTrack("t6", Track::Kind::Annotations, "Callouts"));
    p.clips.push_back(MakeClip("c1", "t1", "m1", Clip::Source::Screen, 0, 0, 30'000'000));
    p.clips.push_back(MakeClip("c2", "t1", "m1", Clip::Source::Screen, 30'000'000, 30'000'000, 60'000'000));
    Clip cam = MakeClip("c3", "t2", "m1", Clip::Source::Camera, 0, 0, 60'000'000);
    cam.pip = Pip{};
    p.clips.push_back(cam);
    p.clips.push_back(MakeClip("c4", "t3", "m1", Clip::Source::Mic, 0, 0, 60'000'000));
    p.clips.push_back(MakeClip("c5", "t5", "m3", Clip::Source::Default, 5'000'000, 0, 30'000'000));
    Clip note = MakeClip("c6", "t6", "", Clip::Source::Default, 5'000'000, 0, 4'000'000);
    Annotation a;
    a.type = Annotation::Type::Callout;
    a.text = "Click here";
    note.annotation = a;
    p.clips.push_back(note);
    Clip logo = MakeClip("c7", "t2", "m4", Clip::Source::Default, 60'000'000, 0, 5'000'000);
    p.clips.push_back(logo);
    p.markers.push_back(Marker{12'000'000, "intro ends"});
    p.RecomputeNextId();
    return p;
}

} // namespace ct::project::testutil
