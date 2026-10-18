// ct_ui tests for P09 task 9.5's Auto-Zoom regeneration: EditorState::EditAmend (the regenerated keys join the
// parameter edit's undo entry only while nothing else happened), and the Properties panel's debounced live
// regeneration driven through a real slider drag (one undo entry restores parameters and keys together).
#include "headless.h"

#include <ct/events/ctev.h>
#include <ct/events/event_log.h>
#include <ct/project/auto_zoom.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/panels/properties_panel.h>
#include <ct/ui/strings.h>

#include <imgui_internal.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::ui;
using namespace ct::project;
using ct::ui::testing::Headless;

namespace {

events::EventLog ClickLog() {
    events::EventHeader hd;
    hd.region_w = 1920;
    hd.region_h = 1080;
    events::EventWriter w;
    w.OpenMemory(hd);
    w.MouseButton(5'000'000, events::Button::Left, true, 480, 270);
    w.MouseButton(12'000'000, events::Button::Left, true, 1440, 810);
    events::EventLog log;
    log.LoadFromMemory(w.Bytes(), nullptr);
    return log;
}

Project RecordingProject() {
    Project p;
    p.id = "regen-ui";
    Media m;
    m.id = "rec";
    m.kind = Media::Kind::Recording;
    m.path = "C:/r/take.ctrec";
    m.duration_us = 20'000'000;
    m.width = 1920;
    m.height = 1080;
    m.fps_num = 60;
    m.has_events = true;
    p.media.push_back(m);
    Track t;
    t.id = "t1";
    t.kind = Track::Kind::Video;
    t.name = "Screen";
    p.tracks.push_back(t);
    Clip c;
    c.id = "c1";
    c.track = "t1";
    c.media = "rec";
    c.source = Clip::Source::Screen;
    c.out_us = 20'000'000;
    p.clips.push_back(c);
    p.RecomputeNextId();
    p.camera.mode = Camera::Mode::Auto;
    p.camera.auto_params.zoom = 3.0f;
    p.camera.auto_params.follow = false;
    return p;
}

float MaxGeneratedZoom(const Project& p) {
    float z = 0;
    for (const CameraKey& k : p.camera.keys)
        if (k.generated) z = std::max(z, k.zoom);
    return z;
}

} // namespace

CT_TEST(edit_amend_joins_the_parameter_edit_only_when_nothing_else_happened) {
    EditorState s;
    s.SetProject(RecordingProject());
    const std::string s0 = ToJson(s.project);
    CameraKey k;
    k.t_us = 1'000'000;
    k.generated = true;

    // Parameter edit, then the amended regeneration: one entry.
    CT_REQUIRE(s.Edit(str::kUndoCamera, [](Project& p) {
        p.camera.auto_params.zoom = 2.5f;
        return true;
    }));
    const uint64_t rev = s.revision;
    CT_REQUIRE(s.EditAmend(str::kUndoRegenerate, rev, [&](Project& p) {
        p.camera.keys.push_back(k);
        return true;
    }));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string(str::kUndoCamera));
    CT_CHECK(s.revision > rev);   // caches / preview still see a change
    CT_CHECK(s.IsDirty());
    CT_REQUIRE(s.Undo());
    CT_CHECK_EQ(ToJson(s.project), s0);

    // Another edit in between: the regeneration is its own entry.
    CT_REQUIRE(s.Edit(str::kUndoCamera, [](Project& p) {
        p.camera.auto_params.zoom = 2.5f;
        return true;
    }));
    const uint64_t rev2 = s.revision;
    CT_REQUIRE(s.Edit(str::kAddMarker, [](Project& p) {
        Marker m;
        m.t_us = 3'000'000;
        p.markers.push_back(m);
        return true;
    }));
    CT_REQUIRE(s.EditAmend(str::kUndoRegenerate, rev2, [&](Project& p) {
        p.camera.keys.push_back(k);
        return true;
    }));
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(3));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string(str::kUndoRegenerate));

    // After an undo the stale revision no longer matches either.
    CT_REQUIRE(s.Undo());
    const uint64_t rev3 = s.revision - 1;
    CT_REQUIRE(s.EditAmend(str::kUndoRegenerate, rev3, [&](Project& p) {
        p.camera.keys.push_back(k);
        return true;
    }));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string(str::kUndoRegenerate));
    // A failing fn changes nothing.
    const size_t entries = s.undo.UndoEntries();
    CT_CHECK(!s.EditAmend(str::kUndoRegenerate, s.revision, [](Project&) { return false; }));
    CT_CHECK_EQ(s.undo.UndoEntries(), entries);
}

CT_TEST(properties_live_regeneration_after_zoom_drag_is_one_undo_entry) {
    const events::EventLog log = ClickLog();
    const AutoZoom::LogLookup lookup = [&](const Media& m) -> const events::EventLog* { return m.id == "rec" ? &log : nullptr; };
    Project start = RecordingProject();
    CT_REQUIRE(AutoZoom::Regenerate(start, lookup));
    CT_REQUIRE(start.camera.keys.size() == 8);
    const std::string s0 = ToJson(start);

    Headless h(600, 1600);
    EditorState s;
    s.SetProject(std::move(start));
    PropertiesPanel panel;
    int calls = 0;
    uint64_t got_rev = 0;
    panel.on_regenerate = [](EditorState&) { return false; };   // the button path is not under test
    panel.on_live_regenerate = [&](EditorState& st, uint64_t rev) {
        ++calls;
        got_rev = rev;
        return st.EditAmend(str::kUndoRegenerate, rev, [&](Project& p) {
            bool changed = false;
            return AutoZoom::Regenerate(p, lookup, &changed) && changed;
        });
    };
    ImGuiID zoom_id = 0;
    auto frame = [&] {
        h.Frame([&] {
            panel.Draw(s);
            ImGui::PushID(str::kZoom);   // PropFloat: PushID(label) + DragFloat("##v")
            zoom_id = ImGui::GetID("##v");
            ImGui::PopID();
        });
    };
    // Find the Auto-Zoom "Zoom" slider by hovering down the field column (hovering never edits).
    float y_zoom = -1;
    for (float y = 0; y < 1600 && y_zoom < 0; y += 3) {
        h.Mouse(400, y);
        frame();
        if (GImGui->HoveredId != 0 && GImGui->HoveredId == zoom_id) y_zoom = y;
    }
    CT_REQUIRE(y_zoom >= 0);
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(0));

    // Drag the slider right by 50 px (0.01 per px): one coalesced "Edit camera" entry.
    h.Button(0, true);
    frame();
    for (int i = 1; i <= 5; ++i) {
        h.Mouse(400.0f + 10.0f * i, y_zoom);
        frame();
    }
    h.Button(0, false);
    frame();
    const float new_zoom = s.project.camera.auto_params.zoom;
    CT_CHECK(new_zoom > 3.2f);
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    CT_CHECK_EQ(calls, 0);   // debounced: not before 300 ms after the last change
    CT_CHECK(panel.WantsRedraw());
    const uint64_t rev_after_drag = s.revision;

    // 400 ms later the keys are regenerated into the same undo entry.
    ImGui::GetIO().DeltaTime = 0.1f;
    for (int i = 0; i < 4; ++i) frame();
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    CT_CHECK_EQ(calls, 1);
    CT_CHECK_EQ(got_rev, rev_after_drag);
    CT_CHECK(!panel.WantsRedraw());
    CT_CHECK_NEAR(MaxGeneratedZoom(s.project), new_zoom, 1e-5f);
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(1));
    CT_CHECK_EQ(std::string(s.undo.UndoLabel()), std::string(str::kUndoCamera));
    const std::string s1 = ToJson(s.project);

    // One undo restores parameters and keys; redo brings both back.
    CT_REQUIRE(s.Undo());
    CT_CHECK_EQ(ToJson(s.project), s0);
    CT_CHECK_NEAR(MaxGeneratedZoom(s.project), 3.0f, 1e-6f);
    CT_REQUIRE(s.Redo());
    CT_CHECK_EQ(ToJson(s.project), s1);
    CT_CHECK_EQ(calls, 1);   // undo/redo do not re-trigger the regeneration
}
