// ct_ui tests: timeline geometry, ruler ticks, shortcut table, theme tokens, EditorState::Edit/undo coalescing, the
// timeline interaction state machine driven by synthetic ImGui IO, snapping, panels/shell smoke tests, and the
// --bench timeline draw of tests/data/projects/1000_clips.ctproj.
#include "headless.h"

#include <ct/core/fs.h>
#include <ct/core/time.h>
#include <ct/project/edit_ops.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>
#include <ct/ui/editor_commands.h>
#include <ct/ui/editor_shell.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/fonts.h>
#include <ct/ui/icons.h>
#include <ct/ui/panels/media_panel.h>
#include <ct/ui/panels/properties_panel.h>
#include <ct/ui/panels/timeline_widget.h>
#include <ct/ui/shortcuts.h>
#include <ct/ui/theme.h>
#include <ct/ui/timeline_math.h>
#include <ct/ui/widgets.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace ct;
using namespace ct::ui;
using namespace ct::project;
using ct::ui::testing::Headless;

namespace {

constexpr const char* k1000Path = "data/projects/1000_clips.ctproj";

Media MakeMedia(const char* id, Media::Kind kind, TimeUs dur, const char* path) {
    Media m;
    m.id = id;
    m.kind = kind;
    m.path = path;
    m.duration_us = dur;
    if (kind != Media::Kind::Audio) {
        m.width = 1920;
        m.height = 1080;
        m.fps_num = 60;
        m.fps_den = 1;
    }
    return m;
}

// One video track with one 4 s clip at 1 s (media 60 s), canvas 60 fps.
Project OneClipProject() {
    Project p;
    p.id = "ui-test";
    p.name = "UI test";
    Media m = MakeMedia("m1", Media::Kind::Video, 60'000'000, "C:/media/a.mp4");
    p.media.push_back(m);
    Track t;
    t.id = "t1";
    t.kind = Track::Kind::Video;
    t.name = "Video";
    p.tracks.push_back(t);
    Clip c;
    c.id = "c1";
    c.track = "t1";
    c.media = "m1";
    c.start_us = 1'000'000;
    c.in_us = 0;
    c.out_us = 4'000'000;
    p.clips.push_back(c);
    p.RecomputeNextId();
    return p;
}

void SetUp(EditorState& s, Project p) {
    s.SetProject(std::move(p));
    s.pps = 100;
    s.scroll_us = 0;
}

// Draws the widget full-size for one frame.
void DrawFrame(Headless& h, TimelineWidget& w, EditorState& s) {
    h.Frame([&] { w.Draw(s, ImVec2(1600, 400)); });
}

// Press at (x0,y0), move by (dx,dy) in `steps` frames, release.
void Drag(Headless& h, TimelineWidget& w, EditorState& s, float x0, float y0, float dx, float dy, int steps = 5) {
    h.Mouse(x0, y0);
    DrawFrame(h, w, s);
    h.Button(0, true);
    DrawFrame(h, w, s);
    for (int i = 1; i <= steps; ++i) {
        h.Mouse(x0 + dx * i / steps, y0 + dy * i / steps);
        DrawFrame(h, w, s);
    }
    h.Button(0, false);
    DrawFrame(h, w, s);
    DrawFrame(h, w, s);
}

// Deterministic 1000-clip project for the draw bench (4 tracks x 250 clips, markers, camera keys).
Project Make1000Clips() {
    Project p;
    p.id = "1000-clips-bench";
    p.name = "1000 clips";
    Media rec = MakeMedia("m1", Media::Kind::Recording, 600'000'000, "C:/rec/long.ctrec");
    rec.audio_streams = {"mic", "system"};
    rec.has_events = true;
    p.media.push_back(rec);
    p.media.push_back(MakeMedia("m2", Media::Kind::Video, 600'000'000, "C:/media/broll.mp4"));
    const struct { const char* id; Track::Kind kind; const char* name; } tracks[] = {
        {"t1", Track::Kind::Video, "Screen"}, {"t2", Track::Kind::Video, "B-roll"},
        {"t3", Track::Kind::Audio, "Mic"}, {"t4", Track::Kind::Annotations, "Callouts"}};
    for (const auto& td : tracks) {
        Track t;
        t.id = td.id;
        t.kind = td.kind;
        t.name = td.name;
        p.tracks.push_back(t);
    }
    uint32_t seed = 12345;
    int n = 0;
    for (int ti = 0; ti < 4; ++ti) {
        TimeUs at = 0;
        for (int i = 0; i < 250; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const TimeUs dur = 2'000'000 + static_cast<TimeUs>((seed >> 8) % 2'000) * 1000;
            Clip c;
            char id[16];
            std::snprintf(id, sizeof(id), "c%d", ++n);
            c.id = id;
            c.track = tracks[ti].id;
            c.start_us = at;
            if (ti == 3) {
                c.out_us = dur;
                Annotation a;
                a.text = "Callout";
                c.annotation = a;
            } else {
                c.media = ti == 1 ? "m2" : "m1";
                c.source = ti == 0 ? Clip::Source::Screen : ti == 2 ? Clip::Source::Mic : Clip::Source::Default;
                c.in_us = (static_cast<TimeUs>(i) * 2'000'000) % 500'000'000;
                c.out_us = c.in_us + dur;
            }
            p.clips.push_back(c);
            at += dur + 500'000;
        }
    }
    for (int i = 0; i < 40; ++i) {
        Marker m;
        m.t_us = static_cast<TimeUs>(i) * 25'000'000 + 1'000'000;
        char l[16];
        std::snprintf(l, sizeof(l), "M%d", i + 1);
        m.label = l;
        p.markers.push_back(m);
    }
    for (int i = 0; i < 60; ++i) {
        CameraKey k;
        k.t_us = static_cast<TimeUs>(i) * 15'000'000;
        k.zoom = (i % 4 == 1 || i % 4 == 2) ? 2.0f : 1.0f;
        k.generated = true;
        p.camera.keys.push_back(k);
    }
    p.RecomputeNextId();
    return p;
}

} // namespace

// --- geometry ------------------------------------------------------------------------------------------------------

CT_TEST(timeline_x_t_round_trip) {
    TimelineView v;
    v.origin_x = 160;
    v.width = 1400;
    for (double pps : {2.0, 37.5, 100.0, 777.7, 2000.0}) {
        v.pps = pps;
        for (TimeUs scroll : {TimeUs(0), TimeUs(5'000'000), TimeUs(3'600'000'000)}) {
            v.scroll_us = scroll;
            for (float x = 160; x <= 1560; x += 97.3f) {
                const TimeUs t = v.T(x);
                CT_CHECK_NEAR(v.X(t), x, 0.02);
            }
            const TimeUs tol = static_cast<TimeUs>(1e6 / pps * 0.01) + 2;
            for (TimeUs t = scroll; t < scroll + 10'000'000; t += 1'234'567) CT_CHECK_NEAR(v.T(v.X(t)), t, tol);
        }
    }
    v.pps = 100;
    v.scroll_us = 0;
    CT_CHECK_EQ(v.X(1'000'000), 260.0f);
    CT_CHECK_EQ(v.T(260.0f), TimeUs(1'000'000));
    CT_CHECK_EQ(v.VisibleEnd(), TimeUs(14'000'000));
}

CT_TEST(timeline_zoom_keeps_anchor_fixed) {
    TimelineView v;
    v.origin_x = 160;
    v.width = 1400;
    v.pps = 100;
    v.scroll_us = 2'000'000;
    const TimeUs anchor = 7'500'000;
    const float x_before = v.X(anchor);
    for (double np : {150.0, 1000.0, 20.0, 2.0}) {
        TimelineView w = v;
        w.scroll_us = ZoomScroll(v.scroll_us, anchor, v.pps, np);
        w.pps = np;
        CT_CHECK_NEAR(w.X(anchor), x_before, 0.01);
    }
    CT_CHECK_EQ(ClampPps(1.0), kMinPps);
    CT_CHECK_EQ(ClampPps(5000.0), kMaxPps);
    CT_CHECK_NEAR(FitPps(100'000'000, 1000), 9.6, 1e-9);
}

CT_TEST(timeline_ruler_tick_selection) {
    struct Case { double pps; TimeUs major; bool frames; };
    const Case cases[] = {
        {2000, 100'000, false},   // a frame is 33 px; 100 ms = 200 px
        {5000, 16'666, true},     // a frame is 83 px
        {800, 100'000, false},    // 100 ms = 80 px exactly
        {700, 500'000, false},
        {100, 1'000'000, false},
        {50, 5'000'000, false},
        {10, 10'000'000, false},
        {3, 30'000'000, false},
        {2, 60'000'000, false},
        {0.1, 300'000'000, false},   // nothing reaches 80 px: the largest unit
    };
    for (const Case& c : cases) {
        const TickUnit u = PickTickUnit(c.pps, 60, 1, 80);
        CT_CHECK_EQ(u.major_us, c.major);
        CT_CHECK_EQ(u.frames, c.frames);
        CT_CHECK_EQ(u.minor_div, c.frames ? 0 : 5);
    }
    char b[32];
    CT_CHECK_EQ(std::string(FormatTick(b, sizeof(b), 65'000'000, PickTickUnit(100, 60, 1), 60, 1)), std::string("1:05"));
    CT_CHECK_EQ(std::string(FormatTick(b, sizeof(b), 65'500'000, PickTickUnit(700, 60, 1), 60, 1)), std::string("1:05.50"));
    CT_CHECK_EQ(std::string(FormatTick(b, sizeof(b), 1'050'000, PickTickUnit(5000, 60, 1), 60, 1)), std::string("0:01:03f"));
    CT_CHECK_EQ(std::string(FormatTimecode(b, sizeof(b), 72'050'000, 60, 1)), std::string("00:01:12.03"));
    CT_CHECK_EQ(std::string(FormatDuration(b, sizeof(b), 185'000'000)), std::string("3:05.00"));
}

// --- shortcuts / theme / widgets -------------------------------------------------------------------------------------

CT_TEST(shortcut_table_parses_without_duplicate_chords) {
    size_t n = 0;
    const ShortcutDef* t = Shortcuts::Table(&n);
    CT_REQUIRE(n > 40);
    std::set<ImGuiKeyChord> seen;
    std::set<int> actions;
    for (size_t i = 0; i < n; ++i) {
        ImGuiKeyChord c = 0;
        if (!Shortcuts::Parse(t[i].keys, &c)) CT_FAIL(std::string("unparsable: ") + t[i].keys);
        CT_CHECK_EQ(Shortcuts::Chord(i), c);
        if (!seen.insert(c).second) CT_FAIL(std::string("duplicate chord: ") + t[i].keys);
        actions.insert(static_cast<int>(t[i].action));
    }
    // Every action has a key.
    for (int a = static_cast<int>(Action::TogglePlay); a < static_cast<int>(Action::Count); ++a)
        if (!actions.count(a)) CT_FAIL("action without a shortcut: " + std::to_string(a));
    ImGuiKeyChord c = 0;
    CT_CHECK(Shortcuts::Parse("Ctrl+Shift+Z", &c) && c == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z));
    CT_CHECK(Shortcuts::Parse("?", &c) && c == (ImGuiMod_Shift | ImGuiKey_Slash));
    CT_CHECK(Shortcuts::Parse("+", &c) && c == ImGuiKey_Equal);
    CT_CHECK(Shortcuts::Parse("Ctrl++", &c) && c == (ImGuiMod_Ctrl | ImGuiKey_Equal));
    CT_CHECK(Shortcuts::Parse("Keypad+", &c) && c == ImGuiKey_KeypadAdd);
    CT_CHECK(Shortcuts::Parse("Shift+,", &c) && c == (ImGuiMod_Shift | ImGuiKey_Comma));
    CT_CHECK(Shortcuts::Parse("F11", &c) && c == ImGuiKey_F11);
    CT_CHECK(Shortcuts::Parse("Alt+I", &c) && c == (ImGuiMod_Alt | ImGuiKey_I));
    CT_CHECK(!Shortcuts::Parse("Hyper+X", &c));
    CT_CHECK(!Shortcuts::Parse("Ctrl+", &c));
    CT_CHECK(!Shortcuts::Parse("F25", &c));
    CT_CHECK(!Shortcuts::Parse("", &c));
    CT_CHECK_EQ(std::string(Shortcuts::KeysFor(Action::Undo)), std::string("Ctrl+Z"));
}

CT_TEST(shortcut_poll_respects_text_input) {
    Headless h;
    h.Frame([] {});
    h.Key(ImGuiMod_Ctrl, true);
    h.Key(ImGuiKey_Z, true);
    Action got[8];
    int n = 0;
    h.Frame([&] { n = Shortcuts::Poll(got, 8); });
    CT_REQUIRE(n == 1);
    CT_CHECK(got[0] == Action::Undo);
    h.Key(ImGuiKey_Z, false);
    h.Key(ImGuiMod_Ctrl, false);
    h.Frame([] {});
    h.Key(ImGuiKey_S, true);
    h.Frame([&] { n = Shortcuts::Poll(got, 8); });
    CT_REQUIRE(n == 1);
    CT_CHECK(got[0] == Action::Split);
    h.Key(ImGuiKey_S, false);
    h.Frame([] {});
}

CT_TEST(theme_text_tokens_meet_contrast) {
    for (ImU32 bg : {col::bg0, col::bg1, col::bg2, col::bg3}) {
        CT_CHECK(Theme::Contrast(col::text, bg) >= 4.5f);
        CT_CHECK(Theme::Contrast(col::text_dim, bg) >= 4.5f);
    }
    CT_CHECK_EQ(Rgb(0x4F8CFF), IM_COL32(0x4F, 0x8C, 0xFF, 0xFF));
    CT_CHECK_EQ(col::accent_soft >> 24, 46u);
    ImGuiStyle s;
    Theme::Apply(s);
    CT_CHECK_EQ(s.FrameRounding, 6.0f);
    CT_CHECK_EQ(s.WindowPadding.x, 12.0f);
    CT_CHECK(ImGui::ColorConvertFloat4ToU32(s.Colors[ImGuiCol_WindowBg]) == col::bg1);
    CT_CHECK(ImGui::ColorConvertFloat4ToU32(s.Colors[ImGuiCol_CheckMark]) == col::accent);
    CT_CHECK_EQ(MeterColor(-30), col::success);
    CT_CHECK_EQ(MeterColor(-10), col::warn);
    CT_CHECK_EQ(MeterColor(-3), col::danger);
}

CT_TEST(icons_present_in_icon_font) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    int checked = 0;
    for (const char* file : {"SegoeIcons.ttf", "segmdl2.ttf"}) {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "%s\\Fonts\\%s", dir, file);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        Headless h;
        ImFont* f = ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 16);
        CT_REQUIRE(f != nullptr);
        h.Frame([] {});
        for (const char* icon : kAllIcons) {
            unsigned int cp = 0;
            ImTextCharFromUtf8(&cp, icon, nullptr);
            if (!f->IsGlyphInFont(static_cast<ImWchar>(cp))) {
                char msg[64];
                std::snprintf(msg, sizeof(msg), "U+%04X missing in %s", cp, file);
                CT_FAIL(msg);
            }
        }
        ++checked;
    }
    if (!checked) CT_SKIP("no Segoe icon font installed");
}

// --- EditorState ------------------------------------------------------------------------------------------------------

CT_TEST(selection_behaviours) {
    Selection s;
    CT_CHECK(s.Empty());
    s.Set("c1");
    CT_CHECK(s.Has("c1"));
    s.Toggle("c2");
    CT_CHECK(s.Has("c2") && s.clips.size() == 2);
    s.Toggle("c1");
    CT_CHECK(!s.Has("c1") && s.clips.size() == 1);
    s.marker = 5;
    s.Set("c3");
    CT_CHECK(!s.marker && s.clips.size() == 1 && s.Has("c3"));
    s.Add("c3");
    CT_CHECK_EQ(s.clips.size(), size_t(1));
    s.Clear();
    CT_CHECK(s.Empty());
}

CT_TEST(edit_wrapper_undo_and_coalescing) {
    EditorState s;
    SetUp(s, OneClipProject());
    const std::string base = ToJson(s.project);
    const uint64_t rev = s.revision;
    Err err;
    // A failing op records nothing.
    CT_CHECK(!s.Edit("Move", [&](Project& p) { return ops::MoveClip(p, "c1", -5, "", &err); }));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(0));
    CT_CHECK_EQ(s.revision, rev);
    CT_CHECK(!s.IsDirty());
    // A failing multi-op EditCopy leaves the project untouched.
    CT_CHECK(!s.EditCopy("Multi", [&](Project& p) {
        ops::MoveClip(p, "c1", 9'000'000, "", &err);
        return false;
    }));
    CT_CHECK_EQ(ToJson(s.project), base);
    // One entry per successful edit.
    CT_CHECK(s.Edit("Move", [&](Project& p) { return ops::MoveClip(p, "c1", 2'000'000, "", &err); }));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    CT_CHECK(s.IsDirty());
    // A drag: many edits, one entry with the drag's label.
    s.BeginDrag("Drag clip");
    for (int i = 1; i <= 20; ++i)
        CT_CHECK(s.Edit("Move", [&](Project& p) { return ops::MoveClip(p, "c1", 2'000'000 + i * 10'000, "", &err); }));
    s.EndDrag();
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(2));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string("Drag clip"));
    CT_CHECK(s.Undo());
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(2'000'000));
    CT_CHECK(s.Undo());
    CT_CHECK_EQ(ToJson(s.project), base);
    CT_CHECK(!s.IsDirty());
    CT_CHECK(s.Redo());
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(2'000'000));
}

CT_TEST(commands_split_delete_paste_markers) {
    EditorState s;
    SetUp(s, OneClipProject());
    s.SetPlayhead(3'000'000);
    CT_CHECK(cmd::SplitAtPlayhead(s));
    CT_CHECK_EQ(s.project.clips.size(), size_t(2));
    s.selection.Set("c1");
    cmd::CopySelection(s);
    s.SetPlayhead(10'000'000);
    CT_CHECK(cmd::PasteAtPlayhead(s));
    CT_CHECK_EQ(s.project.clips.size(), size_t(3));
    CT_CHECK_EQ(s.project.FindClip(s.selection.clips[0])->start_us, TimeUs(10'000'000));
    CT_CHECK(cmd::DeleteSelection(s, false));
    CT_CHECK_EQ(s.project.clips.size(), size_t(2));
    CT_CHECK(cmd::AddMarkerAtPlayhead(s));
    CT_CHECK(!cmd::AddMarkerAtPlayhead(s));   // one marker per time
    CT_CHECK(cmd::RenameMarker(s, 10'000'000, "Intro"));
    CT_CHECK_EQ(s.project.markers[0].label, std::string("Intro"));
    CT_CHECK(cmd::AddCameraKeyAtPlayhead(s));
    CT_CHECK_EQ(s.project.camera.keys.size(), size_t(1));
    CT_CHECK(!s.project.camera.keys[0].generated);
    CT_CHECK(cmd::RemoveNearestCameraKey(s));
    s.selection.Set("c1");
    CT_CHECK(!cmd::NudgeSelection(s, 6));   // the right half of the split is directly after c1
    CT_CHECK(cmd::NudgeSelection(s, -6));
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(1'000'000) - 6 * s.FrameUs());
    const size_t entries = s.undo.UndoEntries();
    while (s.Undo()) {}
    CT_CHECK(entries >= 7);
    CT_CHECK_EQ(s.project.clips.size(), size_t(1));
}

// --- timeline interaction -------------------------------------------------------------------------------------------

CT_TEST(timeline_drag_moves_clip_with_one_undo_entry) {
    Headless h;
    EditorState s;
    SetUp(s, OneClipProject());
    TimelineWidget w;
    DrawFrame(h, w, s);
    ImVec2 a, b;
    CT_REQUIRE(w.ClipRect(s, "c1", &a, &b));
    CT_CHECK_NEAR(a.x, 260, 0.01);   // header 160 + 1 s at 100 px/s
    CT_CHECK_NEAR(b.x, 660, 0.01);
    const float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f;
    Drag(h, w, s, cx, cy, 50, 0);
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(1'500'000));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string("Move clip"));
    CT_CHECK(s.selection.Has("c1"));
    CT_CHECK(w.CurrentMode() == TimelineWidget::Mode::Idle);
    CT_CHECK(!s.DragActive());
    CT_CHECK(s.Undo());
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(1'000'000));
    // A click without movement selects but does not edit.
    s.selection.Clear();
    Drag(h, w, s, cx, cy, 0, 0, 1);
    CT_CHECK(s.selection.Has("c1"));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(0));
}

CT_TEST(timeline_drag_snaps_to_marker) {
    Headless h;
    EditorState s;
    Project p = OneClipProject();
    Marker m;
    m.t_us = 3'000'000;
    p.markers.push_back(m);
    SetUp(s, std::move(p));
    TimelineWidget w;
    DrawFrame(h, w, s);
    ImVec2 a, b;
    CT_REQUIRE(w.ClipRect(s, "c1", &a, &b));
    // +195 px = +1.95 s -> start 2.95 s, 5 px from the marker: snaps to exactly 3 s.
    Drag(h, w, s, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 195, 0);
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(3'000'000));
    // Snapping off: lands where the mouse says.
    CT_CHECK(s.Undo());
    s.snap = false;
    Drag(h, w, s, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 195, 0);
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(2'950'000));
}

CT_TEST(timeline_trim_and_duplicate_drags) {
    Headless h;
    EditorState s;
    SetUp(s, OneClipProject());
    TimelineWidget w;
    DrawFrame(h, w, s);
    ImVec2 a, b;
    CT_REQUIRE(w.ClipRect(s, "c1", &a, &b));
    const float cy = (a.y + b.y) * 0.5f;
    // Right edge -100 px: TrimOut to 4 s.
    Drag(h, w, s, b.x - 3, cy, -100, 0);
    CT_CHECK_EQ(s.project.FindClip("c1")->End(), TimeUs(4'000'000));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string("Trim clip"));
    // Left edge +50 px: TrimIn to 1.5 s, end fixed.
    Drag(h, w, s, a.x + 3, cy, 50, 0);
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(1'500'000));
    CT_CHECK_EQ(s.project.FindClip("c1")->End(), TimeUs(4'000'000));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(2));
    // Ctrl+drag duplicates: the source stays, a copy lands 6 s later, one undo entry.
    CT_REQUIRE(w.ClipRect(s, "c1", &a, &b));
    h.Key(ImGuiMod_Ctrl, true);
    Drag(h, w, s, (a.x + b.x) * 0.5f, cy, 600, 0);
    h.Key(ImGuiMod_Ctrl, false);
    DrawFrame(h, w, s);
    CT_CHECK_EQ(s.project.clips.size(), size_t(2));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(3));
    CT_CHECK_EQ(s.project.FindClip("c1")->start_us, TimeUs(1'500'000));
    CT_REQUIRE(!s.selection.clips.empty());
    const Clip* copy = s.project.FindClip(s.selection.clips[0]);
    CT_REQUIRE(copy && copy->id != "c1");
    CT_CHECK_EQ(copy->start_us, TimeUs(7'500'000));
}

CT_TEST(timeline_scrub_rubber_band_and_header_toggles) {
    Headless h;
    EditorState s;
    Project p = OneClipProject();
    Clip c2 = p.clips[0];
    c2.id = "c2";
    c2.start_us = 6'000'000;
    p.clips.push_back(c2);
    SetUp(s, std::move(p));
    TimelineWidget w;
    DrawFrame(h, w, s);
    // Ruler: press at 2 s, drag to 3 s.
    const float ry = w.RulerY() + 10;
    Drag(h, w, s, 360, ry, 100, 0);
    CT_CHECK_EQ(s.Playhead(), TimeUs(3'000'000));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(0));
    // Rubber band from empty space before c1 across both clips.
    ImVec2 a, b;
    CT_REQUIRE(w.ClipRect(s, "c2", &a, &b));
    Drag(h, w, s, 200, b.y + 6, 1000, -(b.y - a.y));
    CT_CHECK(s.selection.Has("c1") && s.selection.Has("c2"));
    // Header lock toggle (rightmost button of the track header).
    const auto& lanes = w.Lanes();
    const TimelineWidget::Lane* lane = nullptr;
    for (const auto& l : lanes)
        if (l.kind == TimelineWidget::Lane::Kind::Track) lane = &l;
    CT_REQUIRE(lane);
    Drag(h, w, s, 160 - 8 - 11, lane->y + lane->h * 0.5f, 0, 0, 1);
    CT_CHECK(s.project.tracks[0].locked);
    // Locked clips do not move.
    Drag(h, w, s, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 100, 0);
    CT_CHECK_EQ(s.project.FindClip("c2")->start_us, TimeUs(6'000'000));
}

CT_TEST(timeline_wheel_zoom_and_scroll) {
    Headless h;
    EditorState s;
    SetUp(s, OneClipProject());
    TimelineWidget w;
    h.Mouse(660, 120);
    DrawFrame(h, w, s);
    const TimeUs anchor = w.View().T(660);
    h.Key(ImGuiMod_Ctrl, true);
    ImGui::GetIO().AddMouseWheelEvent(0, 2);
    DrawFrame(h, w, s);
    h.Key(ImGuiMod_Ctrl, false);
    DrawFrame(h, w, s);
    CT_CHECK_NEAR(s.pps, 144.0, 1e-6);
    CT_CHECK_NEAR(w.View().X(anchor), 660, 0.5);
    ImGui::GetIO().AddMouseWheelEvent(0, -1);
    const TimeUs before = s.scroll_us;
    DrawFrame(h, w, s);
    CT_CHECK(s.scroll_us > before);
}

// --- panels / shell smoke -------------------------------------------------------------------------------------------

CT_TEST(panels_and_shell_draw_without_id_conflicts) {
    Project p;
    Err err;
    CT_REQUIRE(Load("data/projects/full.ctproj", p, &err));
    Headless h(1600, 900);
    {
        EditorState s;
        s.SetProject(p);
        PropertiesPanel props;
        MediaPanel media;
        // Nothing selected, each clip alone, everything selected.
        std::vector<std::vector<Id>> selections = {{}};
        for (const Clip& c : s.project.clips) selections.push_back({c.id});
        std::vector<Id> all;
        for (const Clip& c : s.project.clips) all.push_back(c.id);
        selections.push_back(all);
        for (const auto& sel : selections) {
            s.selection.Clear();
            s.selection.clips = sel;
            for (int i = 0; i < 2; ++i)
                h.Frame([&] {
                    ImGui::BeginChild("props", ImVec2(320, 0));
                    props.Draw(s);
                    ImGui::EndChild();
                    ImGui::SameLine();
                    ImGui::BeginChild("media", ImVec2(260, 0));
                    media.Draw(s);
                    ImGui::EndChild();
                });
            CT_CHECK(!h.IdConflict());
        }
        CT_CHECK_EQ(s.undo.UndoEntries(), size_t(0));   // drawing never edits
    }
    {
        EditorShell shell;
        shell.State().SetProject(p);
        for (int i = 0; i < 4; ++i) h.Frame([&] { shell.Frame(); }, false);
        CT_CHECK(!h.IdConflict());
        CT_CHECK(shell.Timeline().ClipsDrawn() > 0);
    }
}

// --- 1000 clips fixture + bench ---------------------------------------------------------------------------------------

CT_TEST(fixture_1000_clips) {
    if (ct::test::UpdateGolden()) {
        Err err;
        CT_REQUIRE(Save(Make1000Clips(), k1000Path, &err));
    }
    Project p;
    Err err;
    CT_REQUIRE(Load(k1000Path, p, &err));
    CT_CHECK_EQ(p.clips.size(), size_t(1000));
    std::string why;
    CT_CHECK(p.Validate(&why));
    CT_CHECK_EQ(ToJson(p), ToJson(Make1000Clips()));
}

CT_BENCH(bench_timeline_draw_1000_clips, 200) {
    Project p;
    Err err;
    if (!Load(k1000Path, p, &err)) {
        CT_FAIL("cannot load " + std::string(k1000Path));
        return;
    }
    Headless h(1600, 300);
    EditorState s;
    s.SetProject(std::move(p));
    TimelineWidget w;
    struct Zoom { const char* name; double pps; TimeUs scroll; bool select_all; };
    const TimeUs dur = s.project.Duration();
    const Zoom zooms[] = {{"fit", FitPps(dur, 1440), 0, false},
                          {"fit+all", FitPps(dur, 1440), 0, true},
                          {"10 px/s", 10, 300'000'000, false},
                          {"100 px/s", 100, 300'000'000, false}};
    for (const Zoom& z : zooms) {
        s.pps = z.pps;
        s.scroll_us = z.scroll;
        if (z.select_all) cmd::SelectAll(s);
        else s.selection.Clear();
        double total_ms = 0, worst_ms = 0;
        int drawn = 0;
        for (int64_t i = 0; i < CT_BENCH_N + 5; ++i) {
            double ms = 0;
            h.Frame([&] {
                Stopwatch sw;
                sw.Start();
                w.Draw(s, ImVec2(1600, 300));
                ms = sw.ElapsedMs();
            });
            if (i < 5) continue;   // warm-up (cache build, glyphs)
            total_ms += ms;
            worst_ms = ms > worst_ms ? ms : worst_ms;
            drawn = w.ClipsDrawn();
        }
        const double avg = total_ms / static_cast<double>(CT_BENCH_N);
        std::printf("  timeline draw, 1000 clips, %-9s: avg %.3f ms, worst %.3f ms, %d clips drawn\n", z.name, avg, worst_ms, drawn);
#if defined(NDEBUG)
        CT_CHECK(avg < 1.0);
#endif
    }
}
