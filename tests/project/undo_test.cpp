// ct/project/undo.h and autosave.h: snapshot undo/redo, coalescing, caps, dirty flag, autosave and crash recovery.
#include "test_util.h"

#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/time.h>
#include <ct/project/autosave.h>
#include <ct/project/edit_ops.h>
#include <ct/project/serialize.h>
#include <ct/project/undo.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

using namespace ct;
using namespace ct::project;
using namespace ct::project::testutil;

CT_TEST(undo_redo_restores_exact_json) {
    Project p = MakeSampleProject();
    UndoStack undo;
    CT_CHECK(!undo.CanUndo());
    CT_CHECK_EQ(std::string(undo.UndoLabel()), std::string());
    const std::string s0 = ToJson(p);
    Err err;
    undo.Push(p, "Move clip");
    CT_REQUIRE(ops::MoveClip(p, "c5", 40'000'000, "", &err));
    const std::string s1 = ToJson(p);
    undo.Push(p, "Split");
    CT_REQUIRE(ops::Split(p, "c1", 10'000'000, nullptr, nullptr, &err));
    const std::string s2 = ToJson(p);
    CT_CHECK_EQ(undo.UndoEntries(), size_t(2));
    CT_CHECK_EQ(std::string(undo.UndoLabel()), std::string("Split"));
    CT_REQUIRE(undo.Undo(p));
    CT_CHECK_EQ(ToJson(p), s1);
    CT_CHECK_EQ(std::string(undo.RedoLabel()), std::string("Split"));
    CT_REQUIRE(undo.Undo(p));
    CT_CHECK_EQ(ToJson(p), s0);
    CT_CHECK(!undo.Undo(p));
    CT_REQUIRE(undo.Redo(p));
    CT_CHECK_EQ(ToJson(p), s1);
    CT_REQUIRE(undo.Redo(p));
    CT_CHECK_EQ(ToJson(p), s2);
    CT_CHECK(!undo.Redo(p));
    // A new edit clears the redo stack.
    CT_REQUIRE(undo.Undo(p));
    undo.Push(p, "Trim");
    CT_CHECK(!undo.CanRedo());
}

CT_TEST(undo_keeps_path_ui_and_missing_flags) {
    Project p = MakeSampleProject();
    p.path = "C:\\x\\p.ctproj";
    p.FindMedia("m2")->missing = true;
    UndoStack undo;
    Err err;
    undo.Push(p, "Move");
    CT_REQUIRE(ops::MoveClip(p, "c5", 40'000'000, "", &err));
    p.ui.playhead_us = 777;
    p.ui.selected = {"c5"};
    CT_REQUIRE(undo.Undo(p));
    CT_CHECK_EQ(p.path, std::string("C:\\x\\p.ctproj"));
    CT_CHECK_EQ(p.ui.playhead_us, TimeUs(777));
    CT_CHECK_EQ(p.ui.selected.size(), size_t(1));
    CT_CHECK(p.FindMedia("m2")->missing);
    CT_CHECK_EQ(p.FindClip("c5")->start_us, TimeUs(5'000'000));
}

CT_TEST(undo_coalescing_one_entry_per_drag) {
    Project p = MakeSampleProject();
    const std::string start = ToJson(p);
    UndoStack undo;
    Err err;
    undo.BeginCoalesce("drag:c5");
    for (int i = 1; i <= 30; ++i) {
        undo.Push(p, "Move clip");
        CT_REQUIRE(ops::MoveClip(p, "c5", 5'000'000 + i * 100'000, "", &err));
    }
    undo.EndCoalesce();
    CT_CHECK_EQ(undo.UndoEntries(), size_t(1));
    // A second drag is a second entry; the same key twice in a row is still one group.
    undo.BeginCoalesce("drag:c5");
    undo.BeginCoalesce("drag:c5");
    for (int i = 0; i < 5; ++i) {
        undo.Push(p, "Move clip");
        CT_REQUIRE(ops::MoveClip(p, "c5", 20'000'000 + i * 100'000, "", &err));
    }
    undo.BeginCoalesce("trim:c1");   // a different key starts a new group
    undo.Push(p, "Trim");
    CT_REQUIRE(ops::TrimOut(p, "c1", 20'000'000, &err));
    undo.EndCoalesce();
    CT_CHECK_EQ(undo.UndoEntries(), size_t(3));
    CT_REQUIRE(undo.Undo(p));
    CT_REQUIRE(undo.Undo(p));
    CT_REQUIRE(undo.Undo(p));
    CT_CHECK_EQ(ToJson(p), start);
}

CT_TEST(undo_caps_entries_and_bytes) {
    Project p = MakeSampleProject();
    UndoStack undo(5);
    Err err;
    for (int i = 0; i < 12; ++i) {
        undo.Push(p, "Move");
        CT_REQUIRE(ops::MoveClip(p, "c5", 5'000'000 + i * 1000, "", &err));
    }
    CT_CHECK_EQ(undo.Entries(), size_t(5));
    int n = 0;
    while (undo.Undo(p)) ++n;
    CT_CHECK_EQ(n, 5);
    CT_CHECK_EQ(p.FindClip("c5")->start_us, TimeUs(5'000'000 + 6 * 1000));   // entries 0..6 were dropped

    const size_t one = ToJson(p).size();
    UndoStack small(200, one * 3 + one / 2);
    for (int i = 0; i < 10; ++i) small.Push(p, "x");
    CT_CHECK_EQ(small.Entries(), size_t(3));
    CT_CHECK(small.Bytes() <= one * 3 + one / 2);
    small.Clear();
    CT_CHECK_EQ(small.Entries(), size_t(0));
    CT_CHECK_EQ(small.Bytes(), size_t(0));
}

// Review fix: Undo() adds the redo snapshot's bytes; the byte cap must hold even when only redo entries are left.
CT_TEST(undo_redo_bytes_respect_cap) {
    Project p = MakeSampleProject();
    const size_t one = ToJson(p).size();
    UndoStack undo(200, one + one / 2);
    undo.Push(p, "grow");                 // small snapshot
    p.name = std::string(one, 'n');       // the current document is now ~2x larger
    CT_REQUIRE(undo.Undo(p));             // redo entry = the large document, alone over the cap
    CT_CHECK(undo.Bytes() <= one + one / 2);
    CT_CHECK_EQ(undo.Entries(), size_t(0));
    CT_CHECK(!undo.CanRedo());
    // Within the cap the redo entry is kept.
    UndoStack roomy(200, one * 4);
    Project q = MakeSampleProject();
    roomy.Push(q, "grow");
    q.name = std::string(one, 'n');
    CT_REQUIRE(roomy.Undo(q));
    CT_CHECK(roomy.CanRedo());
    CT_CHECK(roomy.Bytes() <= one * 4);
}

CT_TEST(undo_dirty_flag_tracks_save_point) {
    Project p = MakeSampleProject();
    UndoStack undo;
    Err err;
    CT_CHECK(!undo.IsDirty());
    undo.Push(p, "a");
    ops::MoveClip(p, "c5", 6'000'000, "", &err);
    CT_CHECK(undo.IsDirty());
    undo.MarkSaved();
    CT_CHECK(!undo.IsDirty());
    undo.Push(p, "b");
    ops::MoveClip(p, "c5", 7'000'000, "", &err);
    CT_CHECK(undo.IsDirty());
    undo.Undo(p);
    CT_CHECK(!undo.IsDirty());   // back at the saved state
    undo.Undo(p);
    CT_CHECK(undo.IsDirty());
    undo.Redo(p);
    CT_CHECK(!undo.IsDirty());
    // Coalesced pushes after the first still make the document dirty.
    undo.MarkSaved();
    undo.BeginCoalesce("k");
    undo.Push(p, "c");
    undo.MarkSaved();
    undo.Push(p, "c");
    CT_CHECK(undo.IsDirty());
    undo.EndCoalesce();
}

namespace {

std::string MakeAutosaveDir() {
    const std::string dir = fs::TempPath("");
    fs::CreateDirectories(dir);
    Autosave::SetDirectory(dir);
    return dir;
}

} // namespace

CT_TEST(autosave_interval_and_edit_count) {
    const std::string dir = MakeAutosaveDir();
    Project p = MakeSampleProject();
    p.path = "D:\\work\\demo.ctproj";
    Autosave a;
    a.SetSynchronous(true);
    a.Init(p.id, 60);
    const int64_t t0 = Clock::NowUs();
    a.Tick(p, t0 + 120 * kUsPerSecond);
    CT_CHECK_EQ(a.Saves(), uint64_t(0));   // not dirty
    a.NotifyChanged();
    a.Tick(p, t0 + 30 * kUsPerSecond);
    CT_CHECK_EQ(a.Saves(), uint64_t(0));   // interval not reached
    a.Tick(p, t0 + 61 * kUsPerSecond);
    CT_CHECK_EQ(a.Saves(), uint64_t(1));
    std::string text;
    CT_REQUIRE(fs::ReadAll(a.AutosavePath(), text));
    CT_CHECK_EQ(text, ToJson(p));
    CT_CHECK(fs::Exists(a.LockPath()));
    CT_CHECK(!a.Dirty());
    // Every 20th edit saves regardless of the interval.
    for (int i = 0; i < Autosave::kEditsPerSave - 1; ++i) {
        a.NotifyChanged();
        a.Tick(p, t0 + 62 * kUsPerSecond);
    }
    CT_CHECK_EQ(a.Saves(), uint64_t(1));
    a.NotifyChanged();
    a.Tick(p, t0 + 62 * kUsPerSecond);
    CT_CHECK_EQ(a.Saves(), uint64_t(2));
    // Our own live project is never offered for recovery.
    CT_CHECK(Autosave::FindRecoverable().empty());
    a.Clear();
    CT_CHECK(!fs::Exists(a.AutosavePath()));
    CT_CHECK(!fs::Exists(a.LockPath()));
    fs::DeleteTree(dir);
}

CT_TEST(autosave_recovery_after_crash) {
    const std::string dir = MakeAutosaveDir();
    Project p = MakeSampleProject();
    p.path = "D:\\work\\crashy.ctproj";
    p.name = "Crashy";
    std::string autosave_path;
    {
        auto a = std::make_unique<Autosave>();
        a->Init(p.id, 1);
        a->NotifyChanged();
        CT_REQUIRE(a->SaveNow(p));
        autosave_path = a->AutosavePath();
        // The process "dies": the lock handle goes away, the files stay.
    }
    const std::vector<Autosave::Recoverable> found = Autosave::FindRecoverable();
    CT_REQUIRE(found.size() == 1);
    CT_CHECK(IEquals(found[0].autosave_path, autosave_path));
    CT_CHECK_EQ(found[0].original_path, p.path);
    CT_CHECK_EQ(found[0].name, std::string("Crashy"));
    CT_CHECK(found[0].mtime > 0);
    Project r;
    Err err;
    CT_REQUIRE(Load(found[0].autosave_path, r, &err));
    CT_CHECK_EQ(r.clips.size(), p.clips.size());
    CT_CHECK(Autosave::Discard(found[0]));
    CT_CHECK(Autosave::FindRecoverable().empty());
    fs::DeleteTree(dir);
}

CT_TEST(autosave_async_writes_land) {
    const std::string dir = MakeAutosaveDir();
    Project p = MakeSampleProject();
    Autosave a;
    a.Init(p.id, 1);
    Err err;
    for (int i = 0; i < 5; ++i) {
        ops::MoveClip(p, "c5", 5'000'000 + i * 1000, "", &err);
        a.NotifyChanged();
        a.Tick(p, Clock::NowUs() + (i + 2) * 2 * kUsPerSecond);
    }
    // Wait for the pool (bounded): the newest snapshot must be the one on disk.
    const std::string want = ToJson(p);
    std::string text;
    for (int spin = 0; spin < 500; ++spin) {
        if (fs::ReadAll(a.AutosavePath(), text) && text == want) break;
        Event e;
        e.Wait(10);
        a.Tick(p, Clock::NowUs() + (spin + 20) * 2 * kUsPerSecond);   // the UI timer keeps ticking (retries failed writes)
    }
    if (text != want)
        std::printf("  saves=%llu exists=%d size=%zu want=%zu path=%s\n", (unsigned long long)a.Saves(),
                    (int)fs::Exists(a.AutosavePath()), text.size(), want.size(), a.AutosavePath().c_str());
    CT_CHECK(text == want);
    a.Clear();
    fs::DeleteTree(dir);
}

// Review fix: a slow background write (the hook stalls the writer 300 ms inside the IO section) must not block the
// UI thread's Tick(), neither the "not due" path nor one that queues the next snapshot.
CT_TEST(autosave_slow_write_does_not_block_tick) {
    const std::string dir = MakeAutosaveDir();
    Project p = MakeSampleProject();
    Autosave a;
    a.Init(p.id, 1);
    struct Ctx {
        Event started{true, false};
    } ctx;
    a.SetTestWriteHook(
        [](void* c) {
            static_cast<Ctx*>(c)->started.Set();
            Event pause;
            pause.Wait(300);
        },
        &ctx);
    a.NotifyChanged();
    a.Tick(p, Clock::NowUs() + 2 * kUsPerSecond);
    CT_REQUIRE(ctx.started.Wait(5000));
    double worst_ms = 0.0;
    const int64_t t_due = Clock::NowUs() + 10 * kUsPerSecond;
    for (int i = 0; i < 20; ++i) {
        if (i == 10) a.NotifyChanged();   // this Tick queues another write while the first is still in flight
        const int64_t t0 = Clock::NowUs();
        a.Tick(p, i < 10 ? t_due - 9 * kUsPerSecond : t_due);
        worst_ms = std::max(worst_ms, double(Clock::NowUs() - t0) / 1000.0);
    }
    std::printf("    worst Tick during a 300 ms write: %.3f ms\n", worst_ms);
    CT_CHECK(worst_ms < 20.0);   // ~0.1 ms normally; blocked it was ~300 ms
    a.SetTestWriteHook(nullptr, nullptr);
    for (int spin = 0; spin < 300 && a.Saves() < 2; ++spin) {
        Event e;
        e.Wait(10);
    }
    CT_CHECK_EQ(a.Saves(), uint64_t(2));
    std::string text;
    CT_CHECK(fs::ReadAll(a.AutosavePath(), text) && text == ToJson(p));
    a.Clear();
    fs::DeleteTree(dir);
}

// P09 9.5: live Auto-Zoom regeneration amends the parameter edit's entry, so one undo reverts both.
CT_TEST(undo_amend_joins_newest_entry) {
    Project p = MakeSampleProject();
    UndoStack undo;
    CT_CHECK(!undo.Amend());   // nothing to amend
    const std::string s0 = ToJson(p);
    undo.Push(p, "Edit camera");
    p.camera.auto_params.zoom = 3.0f;
    undo.MarkSaved();          // a save between the parameter edit and the regeneration
    CT_CHECK(!undo.IsDirty());
    CameraKey k;
    k.t_us = 5'000'000;
    k.zoom = 3.0f;
    k.generated = true;
    p.camera.keys.push_back(k);
    CT_REQUIRE(undo.Amend());
    CT_CHECK(undo.IsDirty());   // the amended change is unsaved
    CT_CHECK_EQ(undo.UndoEntries(), size_t(1));
    CT_CHECK_EQ(std::string(undo.UndoLabel()), std::string("Edit camera"));
    const std::string s1 = ToJson(p);
    CT_REQUIRE(undo.Undo(p));
    CT_CHECK_EQ(ToJson(p), s0);   // parameters and keys together
    CT_REQUIRE(undo.Redo(p));
    CT_CHECK_EQ(ToJson(p), s1);
    CT_CHECK(undo.IsDirty());
}
