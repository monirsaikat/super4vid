// P11 11.5: the export dialog's error UX end to end. A real export fails with an injected HRESULT (full disk, lost
// device); the dialog comes back with the red banner, the plain-language message and the recovery action, and
// "Try again" runs the export to completion.
#include "headless.h"

#include "../common/test_device.h"
#include "../common/test_media.h"
#include "../common/test_project.h"

#include <ct/audio/devices.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/render/render_context.h>
#include <ct/test/ct_test.h>
#include <ct/ui/error_ux.h>
#include <ct/ui/panels/export_dialog.h>
#include <ct/ui/recorder_errors.h>
#include <ct/ui/strings.h>

#include <ct/core/async.h>
#include <ct/core/sync.h>

#include <string>
#include <vector>

using namespace ct;
using namespace ct::ui;
using ct::ui::testing::Headless;

namespace {

// A UI-thread queue for Async::PostToUi: the export job posts its completion, which must run on the thread that
// draws the dialog (without a poster it would run on ct-export and destroy the job from its own thread).
struct UiQueue {
    SrwLock lock;
    std::vector<std::function<void()>*> items;
    static void Post(void* user, std::function<void()>* fn) {
        UiQueue* q = static_cast<UiQueue*>(user);
        ExclusiveGuard g(q->lock);
        q->items.push_back(fn);
    }
    void Drain() {
        std::vector<std::function<void()>*> run;
        {
            ExclusiveGuard g(lock);
            run.swap(items);
        }
        for (auto* fn : run) {
            (*fn)();
            delete fn;
        }
    }
};
UiQueue g_ui;

// Draws the dialog until the running export finished (Draw calls OnFinished), at most `timeout_ms`.
bool DrawUntilIdle(Headless& h, ExportDialog& dlg, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 20) {
        g_ui.Drain();
        h.Frame([&] { dlg.Draw(); }, false);
        if (!dlg.Busy()) return true;
        Sleep(20);
    }
    return false;
}

} // namespace

CT_TEST_DEVICE(export_dialog_failure_banner_and_retry) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Async::SetUiPoster(&UiQueue::Post, &g_ui);
    render::RenderContext rc;
    CT_REQUIRE(SUCCEEDED(rc.Init(*dev)));
    audio::AudioDevices devices;
    const std::string bundle = fs::TempPath(".ctrec");
    const std::string dir = fs::TempPath("");
    fs::CreateDirectories(dir);
    CT_REQUIRE(test::MakeBundle(bundle, pat, 1920, 1080, 5'000'000, false));
    const project::Project p = test::MakeExportProject(bundle, 1920, 1080, 5'000'000, 1'000'000);
    {
        Headless h(1280, 800);
        ExportDialog dlg;
        ExportDialog::Host host;
        host.device = dev;
        host.render_context = &rc;
        host.audio_devices = &devices;
        dlg.Open(p, 0, host, "mp4-720p");
        h.Frame([&] { dlg.Draw(); }, false);

        // 1. Disk full while writing: banner + "Choose another folder...", no partial file.
        const std::string out = PathJoin(dir, "full.mp4");
        dlg.SetDestinationForTest(out);
        exportng::ExportFaults f;
        f.write_error_hr = HRESULT_FROM_WIN32(ERROR_DISK_FULL);
        f.write_error_after_samples = 10;
        dlg.SetFaultsForTest(f);
        dlg.StartExportForTest();
        CT_REQUIRE(dlg.Busy());
        CT_REQUIRE(DrawUntilIdle(h, dlg, 120'000));
        CT_CHECK(dlg.IsOpen());   // reopened with the same settings
        CT_REQUIRE(dlg.Banner().Visible());
        CT_CHECK_EQ(dlg.Banner().Message(), ErrorMessage(ErrorKind::ExportDiskFull, out));
        CT_CHECK_EQ(dlg.Banner().ActionIdAt(0), static_cast<int>(kActionChooseFolder));
        CT_CHECK_EQ(dlg.Destination(), out);
        CT_CHECK(!fs::Exists(out));
        for (int i = 0; i < 3; ++i) h.Frame([&] { dlg.Draw(); }, false);   // the banner draws inside the dialog
        CT_CHECK(!h.IdConflict());

        // 2. Device lost: "Try again" without the fault completes the export.
        f = exportng::ExportFaults{};
        f.write_error_hr = DXGI_ERROR_DEVICE_REMOVED;
        f.write_error_after_samples = 10;
        dlg.SetFaultsForTest(f);
        dlg.StartExportForTest();
        CT_REQUIRE(DrawUntilIdle(h, dlg, 120'000));
        CT_REQUIRE(dlg.Banner().Visible());
        CT_CHECK_EQ(dlg.Banner().Message(), std::string(str::kErrExportDeviceLost));
        CT_CHECK_EQ(dlg.Banner().ActionIdAt(0), static_cast<int>(kActionRetry));
        dlg.SetFaultsForTest(exportng::ExportFaults{});
        dlg.RunBannerActionForTest(kActionRetry);
        CT_CHECK(!dlg.Banner().Visible());
        CT_REQUIRE(dlg.Busy());
        CT_REQUIRE(DrawUntilIdle(h, dlg, 120'000));
        CT_CHECK(!dlg.Banner().Visible());
        CT_CHECK(!dlg.IsOpen());
        CT_CHECK(fs::Exists(out));
    }
    g_ui.Drain();
    Async::SetUiPoster(nullptr, nullptr);
    fs::DeleteTree(bundle);
    fs::DeleteTree(dir);
}
