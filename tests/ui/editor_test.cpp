// ct_ui tests for the P09 second half: preview gizmos (clip transform, annotation, camera key), timecode parsing,
// command-line parsing, path classification, linked audio clips, the review regressions (group move back towards
// the origin, marker self-snap, analytic trim clamp, autosave on every drag frame, camera key/section ID scopes,
// marker label coalescing), and the synthetic .ctrec / 3-layer preview project used by the 9.7 measurements.
#include "headless.h"

#include "../project/bundle_util.h"

#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/project/autosave.h>
#include <ct/project/edit_ops.h>
#include <ct/project/importer.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>
#include <ct/ui/dialogs.h>
#include <ct/ui/editor_app.h>
#include <ct/ui/editor_commands.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/panels/preview_panel.h>
#include <ct/ui/panels/properties_panel.h>
#include <ct/ui/panels/timeline_widget.h>

#include <windows.h>

#include <cmath>
#include <string>

using namespace ct;
using namespace ct::ui;
using namespace ct::project;
using ct::ui::testing::Headless;

namespace {

Project TwoClipProject() {
    Project p;
    p.id = "editor-test";
    p.name = "Editor test";
    Media m;
    m.id = "m1";
    m.kind = Media::Kind::Video;
    m.path = "C:/media/a.mp4";
    m.duration_us = 60'000'000;
    m.width = 1920;
    m.height = 1080;
    m.fps_num = 60;
    m.audio_streams = {"default"};
    p.media.push_back(m);
    Track t;
    t.id = "t1";
    t.kind = Track::Kind::Video;
    t.name = "Video";
    p.tracks.push_back(t);
    for (int i = 0; i < 2; ++i) {
        Clip c;
        c.id = i == 0 ? "c1" : "c2";
        c.track = "t1";
        c.media = "m1";
        c.start_us = 1'000'000 + i * 4'000'000;   // c1 [1,5), c2 [5,9): adjacent
        c.in_us = i * 4'000'000;
        c.out_us = c.in_us + 4'000'000;
        p.clips.push_back(c);
    }
    p.RecomputeNextId();
    return p;
}

void Set(EditorState& s, Project p) {
    s.SetProject(std::move(p));
    s.pps = 100;
    s.scroll_us = 0;
}

void TFrame(Headless& h, TimelineWidget& w, EditorState& s) {
    h.Frame([&] { w.Draw(s, ImVec2(1600, 400)); });
}

void PFrame(Headless& h, PreviewPanel& pv, EditorState& s) {
    h.Frame([&] { pv.Draw(s); });
}

// Press, move through the given x offsets (one frame each), release.
template <class FrameFn>
void DragPath(Headless& h, FrameFn frame, float x0, float y0, std::initializer_list<float> xs, float dy = 0) {
    h.Mouse(x0, y0);
    frame();
    h.Button(0, true);
    frame();
    for (float dx : xs) {
        h.Mouse(x0 + dx, y0 + dy);
        frame();
    }
    h.Button(0, false);
    frame();
    frame();
}

std::string TempDir(const char* name) {
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    std::string d = std::string(buf) + "ct_ui_" + name + "_" + std::to_string(GetCurrentProcessId());
    fs::CreateDirectories(d);
    return d;
}

} // namespace

// --- review regressions --------------------------------------------------------------------------------------------

CT_TEST(group_move_back_towards_origin_keeps_following) {
    Headless h;
    EditorState s;
    Set(s, TwoClipProject());
    s.snap = false;
    s.selection.Set("c1");
    s.selection.Add("c2");
    TimelineWidget w;
    TFrame(h, w, s);
    ImVec2 a, b;
    CT_REQUIRE(w.ClipRect(s, "c1", &a, &b));
    // +500 px (+5 s), then back to +300 px (+3 s) in the same drag: the group must follow both ways.
    DragPath(h, [&] { TFrame(h, w, s); }, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, {100, 300, 500, 400, 300});
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(4'000'000));
    CT_CHECK_EQ(s.project.FindClip("c2")->start_us, TimeUs(8'000'000));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
}

CT_TEST(marker_drag_does_not_snap_to_itself) {
    Headless h;
    EditorState s;
    Project p = TwoClipProject();
    Marker m;
    m.t_us = 3'000'000;
    p.markers.push_back(m);
    Set(s, std::move(p));
    TimelineWidget w;
    TFrame(h, w, s);
    const float x = w.View().X(3'000'000);
    // +5 px = +50 ms, inside the 8 px snap tolerance of its own old position.
    DragPath(h, [&] { TFrame(h, w, s); }, x, w.RulerY() + 8, {2, 4, 5});
    CT_REQUIRE(s.project.markers.size() == 1);
    CT_CHECK_EQ(s.project.markers[0].t_us, TimeUs(3'050'000));
}

CT_TEST(trim_clamps_at_media_start_and_neighbour) {
    Headless h;
    EditorState s;
    Set(s, TwoClipProject());
    s.snap = false;
    TimelineWidget w;
    TFrame(h, w, s);
    ImVec2 a, b;
    // c2 starts at media 4 s right after c1: trimming its start left is blocked by c1 (stays at 5 s).
    CT_REQUIRE(w.ClipRect(s, "c2", &a, &b));
    DragPath(h, [&] { TFrame(h, w, s); }, a.x + 3, (a.y + b.y) * 0.5f, {-50, -150});
    CT_CHECK_EQ(s.project.FindClip("c2")->start_us, TimeUs(5'000'000));
    // c1 (media in 0) trimmed left by 0.5 s: clamps at its media start, so it stays at 1 s.
    CT_REQUIRE(w.ClipRect(s, "c1", &a, &b));
    DragPath(h, [&] { TFrame(h, w, s); }, a.x + 3, (a.y + b.y) * 0.5f, {-20, -50});
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(1'000'000));
    // c2's end dragged right by 30 s: clamps at the media end (60 s of media, in 4 s -> 56 s of clip).
    CT_REQUIRE(w.ClipRect(s, "c2", &a, &b));
    DragPath(h, [&] { TFrame(h, w, s); }, b.x - 3, (a.y + b.y) * 0.5f, {500, 6000});
    CT_CHECK_EQ(s.project.FindClip("c2")->End(), TimeUs(5'000'000 + 56'000'000));
}

CT_TEST(autosave_counts_every_frame_of_a_drag) {
    const std::string dir = TempDir("autosave");
    Autosave::SetDirectory(dir);
    Headless h;
    EditorState s;
    Set(s, TwoClipProject());
    s.snap = false;
    s.autosave.SetSynchronous(true);
    s.autosave.Init(s.project.id, 0);   // time-based saves off: only the "every 20 edits" rule
    TimelineWidget w;
    TFrame(h, w, s);
    ImVec2 a, b;
    CT_REQUIRE(w.ClipRect(s, "c2", &a, &b));
    DragPath(h, [&] { TFrame(h, w, s); }, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f,
             {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220});
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    s.autosave.Tick(s.project);
    CT_CHECK_EQ(s.autosave.Saves(), uint64_t(1));   // 22 commits of one drag reached the 20-edit rule
    s.autosave.Clear();
}

CT_TEST(camera_key_and_camera_section_have_separate_ids) {
    Headless h(600, 900);
    EditorState s;
    Project p = TwoClipProject();
    p.camera.mode = Camera::Mode::Auto;
    CameraKey k;
    k.t_us = 2'000'000;
    k.zoom = 2;
    p.camera.keys.push_back(k);
    Set(s, std::move(p));
    s.selection.camera_key = 2'000'000;
    PropertiesPanel panel;
    bool conflict = false;
    // Hover every row: ImGui flags an ID conflict when a hovered item's ID was submitted twice.
    for (float y = 0; y < 900 && !conflict; y += 5) {
        h.Mouse(400, y);
        h.Frame([&] { panel.Draw(s); });
        conflict |= h.IdConflict();
    }
    CT_CHECK(!conflict);
}

CT_TEST(marker_label_typing_is_one_undo_entry) {
    Headless h(600, 600);
    EditorState s;
    Project p = TwoClipProject();
    Marker m;
    m.t_us = 3'000'000;
    m.label = "a";
    p.markers.push_back(m);
    Set(s, std::move(p));
    s.selection.marker = 3'000'000;
    PropertiesPanel panel;
    auto frame = [&] { h.Frame([&] { panel.Draw(s); }); };
    // Find the label field: click down the field column until a text field takes keyboard input.
    bool found = false;
    for (float y = 30; y < 300 && !found; y += 4) {   // below the section header (a click there collapses it)
        h.Mouse(400, y);
        frame();
        h.Button(0, true);
        frame();
        h.Button(0, false);
        frame();
        frame();
        found = ImGui::GetIO().WantTextInput;
    }
    CT_REQUIRE(found);
    for (char c : std::string("bcd")) {
        ImGui::GetIO().AddInputCharacter(static_cast<unsigned>(c));
        frame();
    }
    h.Key(ImGuiKey_Enter, true);
    frame();
    h.Key(ImGuiKey_Enter, false);
    frame();
    // Click away to deactivate.
    h.Mouse(5, 590);
    frame();
    h.Button(0, true);
    frame();
    h.Button(0, false);
    frame();
    frame();
    CT_CHECK_EQ(s.project.markers[0].label, std::string("bcd"));   // the field selects its text on focus
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    CT_CHECK(!s.DragActive());
}

// --- preview gizmos -------------------------------------------------------------------------------------------------

CT_TEST(preview_move_gizmo_changes_transform_with_one_undo_entry) {
    Headless h(1600, 900);
    EditorState s;
    Set(s, TwoClipProject());
    s.SetPlayhead(2'000'000);
    s.selection.Set("c1");
    PreviewPanel pv;
    PFrame(h, pv, s);
    ImVec2 q[4];
    CT_REQUIRE(pv.ClipGizmoCorners(s, q));
    const ImVec2 c((q[0].x + q[3].x) * 0.5f, (q[0].y + q[3].y) * 0.5f);
    const float scale = pv.Scale();
    CT_REQUIRE(scale > 0);
    DragPath(h, [&] { PFrame(h, pv, s); }, c.x, c.y, {20, 60, 100});
    const Clip* cl = s.project.FindClip("c1");
    CT_CHECK_NEAR(cl->transform.x, 100.0f / scale / 1920.0f, 1e-3);
    CT_CHECK_NEAR(cl->transform.y, 0.0f, 1e-4);
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string("Move layer"));
    CT_CHECK(!s.DragActive());
}

CT_TEST(preview_scale_and_rotate_gizmos) {
    Headless h(1600, 900);
    EditorState s;
    Set(s, TwoClipProject());
    s.SetPlayhead(2'000'000);
    s.selection.Set("c1");
    PreviewPanel pv;
    pv.fit_mode = 1;   // 50 %: room around the canvas for the handles
    PFrame(h, pv, s);
    ImVec2 q[4];
    CT_REQUIRE(pv.ClipGizmoCorners(s, q));
    const ImVec2 c((q[0].x + q[3].x) * 0.5f, (q[0].y + q[3].y) * 0.5f);
    // Bottom-right corner handle dragged outwards along the diagonal by half the half-diagonal: scale 1.5.
    const ImVec2 d(q[3].x - c.x, q[3].y - c.y);
    DragPath(h, [&] { PFrame(h, pv, s); }, q[3].x, q[3].y, {d.x * 0.25f, d.x * 0.5f}, d.y * 0.5f);
    CT_CHECK_NEAR(s.project.FindClip("c1")->transform.scale, 1.5f, 0.02);
    CT_CHECK(s.Undo());   // back to scale 1 so the rotation handle is inside the panel
    PFrame(h, pv, s);
    // Rotation handle: 24 px above the top-edge midpoint; drag it a quarter turn clockwise around the centre.
    CT_REQUIRE(pv.ClipGizmoCorners(s, q));
    const ImVec2 c2((q[0].x + q[3].x) * 0.5f, (q[0].y + q[3].y) * 0.5f);
    const ImVec2 top((q[0].x + q[1].x) * 0.5f, (q[0].y + q[1].y) * 0.5f - 24);
    const float r = c2.y - top.y;
    h.Mouse(top.x, top.y);
    PFrame(h, pv, s);
    h.Button(0, true);
    PFrame(h, pv, s);
    for (int i = 1; i <= 6; ++i) {
        const float a = -1.5707963f + 1.5707963f * i / 6;
        h.Mouse(c2.x + r * std::cos(a), c2.y + r * std::sin(a));
        PFrame(h, pv, s);
    }
    h.Button(0, false);
    PFrame(h, pv, s);
    CT_CHECK_NEAR(s.project.FindClip("c1")->transform.rotation_deg, 90.0f, 1.0);
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
}

CT_TEST(preview_annotation_move_resize_and_camera_key_pan) {
    Headless h(1600, 900);
    EditorState s;
    Project p = TwoClipProject();
    Track at;
    at.id = "t2";
    at.kind = Track::Kind::Annotations;
    at.name = "Callouts";
    p.tracks.push_back(at);
    Clip ac = MakeAnnotationClip(3, "t2", 0);   // rectangle, 4 s
    ac.id = "a1";
    ac.annotation->rect = RectF{0.4f, 0.4f, 0.2f, 0.2f};
    p.clips.push_back(ac);
    CameraKey k;
    k.t_us = 1'000'000;
    k.zoom = 2;
    k.cx = 0.5f;
    k.cy = 0.5f;
    p.camera.keys.push_back(k);
    p.camera.mode = Camera::Mode::Manual;
    p.RecomputeNextId();
    Set(s, std::move(p));
    s.SetPlayhead(500'000);   // before the key: zoom 2 holds; select the annotation first (camera on)
    s.selection.Set("a1");
    PreviewPanel pv;
    PFrame(h, pv, s);
    // Annotation body (screen centre of the canvas under 2x zoom = canvas centre): move +40 px.
    const ImVec2 centre = pv.CanvasToScreen(960, 540);
    DragPath(h, [&] { PFrame(h, pv, s); }, centre.x, centre.y, {20, 40});
    const float dx_norm = 40.0f / pv.Scale() / 2.0f / 1920.0f;   // zoom 2: half the canvas distance
    CT_CHECK_NEAR(s.project.FindClip("a1")->annotation->rect.x, 0.4f + dx_norm, 2e-3);
    CT_CHECK_NEAR(s.project.FindClip("a1")->annotation->rect.w, 0.2f, 1e-5);
    // Camera key selected: the preview shows the whole canvas and the key's window; drag the window body +96 px.
    s.selection.Clear();
    s.selection.camera_key = 1'000'000;
    PFrame(h, pv, s);
    const ImVec2 kc = pv.CanvasToScreen(960, 540);
    DragPath(h, [&] { PFrame(h, pv, s); }, kc.x, kc.y, {48, 96});
    CT_REQUIRE(s.project.camera.keys.size() == 1);
    CT_CHECK_NEAR(s.project.camera.keys[0].cx, 0.5f + 96.0f / pv.Scale() / 1920.0f, 2e-3);
    CT_CHECK(!s.project.camera.keys[0].generated);
}

// --- parsing / helpers ------------------------------------------------------------------------------------------------

CT_TEST(timecode_parsing) {
    TimeUs t = 0;
    CT_CHECK(ParseTimecode("00:01:12.03", 60, 1, &t));
    CT_CHECK_EQ(t, TimeUs(72'000'000 + FrameTimeUs(3, 60, 1)));
    CT_CHECK(ParseTimecode("1:05", 30, 1, &t));
    CT_CHECK_EQ(t, TimeUs(65'000'000));
    CT_CHECK(ParseTimecode("72.5", 60, 1, &t));
    CT_CHECK_EQ(t, TimeUs(72'500'000));
    CT_CHECK(ParseTimecode(" 3s", 60, 1, &t));
    CT_CHECK_EQ(t, TimeUs(3'000'000));
    CT_CHECK(!ParseTimecode("abc", 60, 1, &t));
    CT_CHECK(!ParseTimecode("1:75", 60, 1, &t));
    CT_CHECK(!ParseTimecode("", 60, 1, &t));
}

CT_TEST(editor_command_line) {
    EditorLaunch l;
    CT_CHECK(ParseEditorLaunch(L"super4vid.exe --editor \"C:\\x\\a b.ctproj\"", &l));
    CT_REQUIRE(l.paths.size() == 1);
    CT_CHECK_EQ(l.paths[0], std::string("C:\\x\\a b.ctproj"));
    EditorLaunch bare;
    CT_CHECK(ParseEditorLaunch(L"super4vid.exe D:\\rec\\one.ctrec", &bare));
    CT_CHECK_EQ(bare.paths.size(), size_t(1));
    EditorLaunch none;
    CT_CHECK(!ParseEditorLaunch(L"super4vid.exe --tray", &none));
    EditorLaunch demo;
    CT_CHECK(!ParseEditorLaunch(L"super4vid.exe --editor-demo --project p.ctproj", &demo));
    EditorLaunch shot;
    CT_CHECK(ParseEditorLaunch(L"super4vid.exe --editor --screenshot o.png --frames 9 --bench-scrub 12", &shot));
    CT_CHECK_EQ(shot.screenshot, std::string("o.png"));
    CT_CHECK_EQ(shot.frames, 9);
    CT_CHECK_EQ(shot.bench, std::string("--bench-scrub"));
    CT_CHECK_EQ(shot.bench_count, 12);
    CT_CHECK(shot.paths.empty());
}

CT_TEST(path_classification) {
    const std::string dir = TempDir("classify");
    const std::string bundle = dir + "\\take.ctrec";
    fs::CreateDirectories(bundle);
    fs::WriteAtomic(bundle + "\\meta.json", "{}");
    fs::WriteAtomic(dir + "\\p.ctproj", "{}");
    CT_CHECK(dialogs::Classify(bundle) == dialogs::PathKind::Recording);
    CT_CHECK(dialogs::Classify(bundle + "\\") == dialogs::PathKind::Recording);
    CT_CHECK(dialogs::Classify(bundle + "\\meta.json") == dialogs::PathKind::Recording);
    CT_CHECK(dialogs::Classify(dir + "\\p.ctproj") == dialogs::PathKind::Project);
    CT_CHECK(dialogs::Classify("C:\\x\\clip.MP4") == dialogs::PathKind::Media);
    CT_CHECK(dialogs::Classify(dir) == dialogs::PathKind::Unknown);
    CT_CHECK_EQ(dialogs::FileStem("C:\\a\\b.c.ctproj"), std::string("b.c"));
    CT_CHECK_EQ(dialogs::Extension("C:\\a.b\\c"), std::string(""));
}

CT_TEST(linked_audio_clip_for_video_with_sound) {
    Project p = TwoClipProject();
    CT_REQUIRE(cmd::AddLinkedAudioClip(p, *p.FindClip("c1")));
    // A new audio track got a clip with the same timing and media range.
    const Track* audio = nullptr;
    for (const Track& t : p.tracks)
        if (t.kind == Track::Kind::Audio) audio = &t;
    CT_REQUIRE(audio != nullptr);
    const Clip* a = nullptr;
    for (const Clip& c : p.clips)
        if (c.track == audio->id) a = &c;
    CT_REQUIRE(a != nullptr);
    CT_CHECK_EQ(a->start_us, TimeUs(1'000'000));
    CT_CHECK_EQ(a->out_us, TimeUs(4'000'000));
    // The second one reuses the audio track (room after c1's audio).
    CT_CHECK(cmd::AddLinkedAudioClip(p, *p.FindClip("c2")));
    int audio_tracks = 0;
    for (const Track& t : p.tracks) audio_tracks += t.kind == Track::Kind::Audio ? 1 : 0;
    CT_CHECK_EQ(audio_tracks, 1);
    std::string why;
    CT_CHECK(p.Validate(&why));
    // Audio-less media: nothing added.
    p.FindMedia("m1")->audio_streams.clear();
    const size_t n = p.clips.size();
    CT_CHECK(!cmd::AddLinkedAudioClip(p, *p.FindClip("c1")));
    CT_CHECK_EQ(p.clips.size(), n);
}

// --- 9.7 fixtures: synthetic recording bundle and the 3-layer preview project (written to ../out/testmedia) --------

CT_TEST(editor_fixtures_synthetic_bundle_and_preview_project) {
    std::string pattern = "data/pattern_1080p60_5s.mp4";
    std::string pattern4k = "data/pattern_4k60_3s.mp4";
    std::string probe;
    if (!fs::ReadAll(pattern, probe)) CT_SKIP("tests/data/pattern_1080p60_5s.mp4 missing");
    const std::string out = "..\\out\\testmedia";
    fs::CreateDirectories(out);
    Json meta = project::testutil::DocsMetaSample();
    CT_REQUIRE(!meta.IsNull());
    meta.Set("duration_us", Json(int64_t(5'000'000)));
    meta["camera"].Set("width", Json(1920));
    meta["camera"].Set("height", Json(1080));
    meta["camera"].Set("fps_num", Json(60));
    meta["audio"].Set("system", Json(nullptr));
    meta.Set("pauses", Json::Array());
    const std::vector<project::testutil::Click> clicks = {{900'000, 1400, 300}, {2'600'000, 400, 800}, {4'000'000, 1500, 900}};
    const std::string events = project::testutil::MakeEventsFile(1920, 1080, clicks, 5'000'000);
    const std::string bundle = project::testutil::MakeBundle(out, "editor_demo", meta, events);
    CT_REQUIRE(CopyFileA(pattern.c_str(), (bundle + "\\screen.mp4").c_str(), FALSE) != 0);
    CT_REQUIRE(CopyFileA(pattern.c_str(), (bundle + "\\cam.mp4").c_str(), FALSE) != 0);

    // The preview budget project: screen recording (cursor + events) + camera PIP + a third video layer + 2 annotations.
    Project p;
    p.id = Uuid4();
    p.name = "preview_bench";
    Err err;
    CT_REQUIRE(project::ImportRecording(p, bundle, &err));
    std::string b4k;
    const bool have4k = fs::ReadAll(pattern4k, b4k);
    Media m;
    m.kind = Media::Kind::Video;
    char full[MAX_PATH];
    GetFullPathNameA(have4k ? pattern4k.c_str() : pattern.c_str(), MAX_PATH, full, nullptr);
    m.path = full;
    m.duration_us = have4k ? 3'000'000 : 5'000'000;
    m.width = have4k ? 3840 : 1920;
    m.height = have4k ? 2160 : 1080;
    m.fps_num = 60;
    Id mid, tid, cid;
    CT_REQUIRE(ops::AddMedia(p, m, &mid));
    CT_REQUIRE(ops::AddTrack(p, Track::Kind::Video, "B-roll", &tid, -1));
    Clip c;
    c.track = tid;
    c.media = mid;
    c.start_us = 0;
    c.out_us = m.duration_us;
    c.transform.scale = 0.3f;
    c.transform.x = -0.3f;
    c.transform.y = -0.25f;
    CT_REQUIRE(ops::AddClip(p, c, &cid, &err));
    Id ann_track;
    for (const Track& t : p.tracks)
        if (t.kind == Track::Kind::Annotations) ann_track = t.id;
    if (ann_track.empty()) CT_REQUIRE(ops::AddTrack(p, Track::Kind::Annotations, "Callouts", &ann_track, -1));
    Clip a1 = MakeAnnotationClip(1, ann_track, 0);   // callout
    a1.out_us = 2'400'000;
    a1.annotation->rect = RectF{0.62f, 0.12f, 0.24f, 0.1f};
    Clip a2 = MakeAnnotationClip(3, ann_track, 2'500'000);   // rectangle
    a2.out_us = 2'400'000;
    a2.annotation->rect = RectF{0.1f, 0.6f, 0.3f, 0.2f};
    CT_REQUIRE(ops::AddClip(p, a1, nullptr, &err));
    CT_REQUIRE(ops::AddClip(p, a2, nullptr, &err));
    std::string why;
    CT_CHECK(p.Validate(&why));
    char proj[MAX_PATH];
    GetFullPathNameA((out + "\\preview_bench.ctproj").c_str(), MAX_PATH, proj, nullptr);
    CT_REQUIRE(project::Save(p, proj, &err));
    std::printf("  fixtures: %s, %s\n", bundle.c_str(), proj);
}
