// ui::ExportDialog: file-name helpers and a headless draw of the settings window for every container (no device:
// the export itself is covered by ct_export_tests).
#include "headless.h"

#include <ct/core/async.h>
#include <ct/project/project.h>
#include <ct/test/ct_test.h>
#include <ct/ui/panels/export_dialog.h>

using namespace ct;
using ct::ui::ExportDialog;
using ct::ui::testing::Headless;

CT_TEST(export_dialog_file_names_and_preset_ids) {
    CT_CHECK_EQ(ExportDialog::SanitizeFileName("a/b:c*?.\"x\"  "), std::string("a_b_c__._x_"));
    CT_CHECK_EQ(ExportDialog::SanitizeFileName("..."), std::string("Untitled"));
    CT_CHECK_EQ(ExportDialog::DefaultFileName("Demo 1", exportng::Preset::Container::Gif), std::string("Demo 1.gif"));
    CT_CHECK_EQ(ExportDialog::DefaultFileName("", exportng::Preset::Container::Mp4), std::string("Untitled.mp4"));
    CT_CHECK_EQ(ExportDialog::MakePresetId("My 4K export!"), std::string("user-my-4k-export"));
    CT_CHECK_EQ(ExportDialog::MakePresetId("***"), std::string("user-preset"));
    CT_CHECK(exportng::Preset::IsValidId(ExportDialog::MakePresetId("Ünïcode name with spaces")));
}

CT_TEST(export_dialog_draws_every_container_headless) {
    Headless h(1280, 800);
    project::Project p;
    p.name = "demo";
    project::Track t;
    t.id = "t1";
    t.kind = project::Track::Kind::Annotations;
    p.tracks.push_back(t);
    project::Clip c;
    c.id = "c1";
    c.track = "t1";
    c.out_us = 3'000'000;
    c.annotation = project::Annotation{};
    p.clips.push_back(c);
    p.ui.loop = std::make_pair(TimeUs(500'000), TimeUs(1'500'000));
    ExportDialog dlg;
    dlg.Open(p, 1'000'000, ExportDialog::Host{});
    // Without a UI poster the async results would land on the pool thread: drain the pool now; later Opens then run
    // their jobs synchronously (Async::Run after Shutdown).
    Async::Shutdown();
    CT_CHECK(dlg.IsOpen());
    CT_CHECK(!dlg.Busy());
    for (const char* id : {"mp4-1080p", "gif-720p", "png-frame", "m4a-audio", "mp4-hevc-1080p"}) {
        dlg.Open(p, 1'000'000, ExportDialog::Host{}, id);
        for (int i = 0; i < 3; ++i) h.Frame([&] { dlg.Draw(); }, false);
    }
    CT_CHECK(dlg.IsOpen());
}
