// P11 11.5/11.6 wiring tests: HRESULT -> plain-language kind, the recorder failure/warning and export failure views
// (message + recovery actions for each forced condition), banner action buttons clicked with the mouse, and the live
// microphone meter (LevelMonitor + Settings > Audio > Test microphone) measured on a real WASAPI stream.
#include "headless.h"

#include <ct/audio/wasapi_render.h>
#include <ct/core/config.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>
#include <ct/ui/error_ux.h>
#include <ct/ui/level_monitor.h>
#include <ct/ui/recorder_errors.h>
#include <ct/ui/settings_window.h>
#include <ct/ui/strings.h>

#include <windows.h>
#include <audioclient.h>
#include <dxgi.h>
#include <mferror.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <string>

using namespace ct;
using namespace ct::ui;
using ct::ui::testing::Headless;

namespace {

bool WaitFor(const std::function<bool()>& cond, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (cond()) return true;
        Sleep(10);
    }
    return cond();
}

recorder::SessionFailure Failure(recorder::FailureCause c, bool saved, HRESULT hr = E_FAIL) {
    recorder::SessionFailure f;
    f.cause = c;
    f.hr = hr;
    f.message = "log text";
    f.output_dir = "D:\\Videos\\Camtesia";
    f.bundle_path = "D:\\Videos\\Camtesia\\2026-10-18 10-00-00.ctrec";
    f.saved = saved;
    f.during_prepare = !saved;
    return f;
}

// A -20 dBFS 1 kHz tone on the default render endpoint (for the loopback meter).
struct Tone final : audio::WasapiRender::Callback {
    double phase = 0;
    void Render(float* out, uint32_t frames, int channels, int64_t) override {
        const double step = 2.0 * 3.14159265358979 * 1000.0 / 48000.0;
        for (uint32_t i = 0; i < frames; ++i) {
            const float v = static_cast<float>(0.1 * std::sin(phase));
            phase += step;
            for (int c = 0; c < channels; ++c) out[i * channels + c] = v;
        }
    }
};

} // namespace

CT_TEST(error_kind_for_hr_classifies_recording_errors) {
    ErrorKind k{};
    CT_CHECK(ErrorKindForHr(HRESULT_FROM_WIN32(ERROR_DISK_FULL), &k) && k == ErrorKind::DiskFull);
    CT_CHECK(ErrorKindForHr(HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL), &k) && k == ErrorKind::DiskFull);
    CT_CHECK(ErrorKindForHr(DXGI_ERROR_DEVICE_REMOVED, &k) && k == ErrorKind::DeviceLost);
    CT_CHECK(ErrorKindForHr(DXGI_ERROR_DEVICE_HUNG, &k) && k == ErrorKind::DeviceLost);
    CT_CHECK(ErrorKindForHr(DXGI_ERROR_ACCESS_LOST, &k) && k == ErrorKind::AccessLost);
    CT_CHECK(ErrorKindForHr(AUDCLNT_E_DEVICE_IN_USE, &k) && k == ErrorKind::MicBusy);
    CT_CHECK(ErrorKindForHr(AUDCLNT_E_DEVICE_INVALIDATED, &k) && k == ErrorKind::MicLost);
    CT_CHECK(ErrorKindForHr(MF_E_TOPO_CODEC_NOT_FOUND, &k) && k == ErrorKind::NoEncoder);
    CT_CHECK(!ErrorKindForHr(E_FAIL, &k));
    CT_CHECK(!ErrorKindForHr(S_OK, &k));
}

CT_TEST(error_messages_for_the_new_kinds) {
    CT_CHECK_EQ(ErrorMessage(ErrorKind::OutputFolder, "\\\\?\\Q:\\no such drive\\x"),
                std::string("Camtesia cannot save to Q:\\no such drive\\x. Choose another folder."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::OutputFolder, "\\\\?\\UNC\\nas\\videos"),
                std::string("Camtesia cannot save to \\\\nas\\videos. Choose another folder."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::DiskFullNotStarted, "e:\\rec"),
                std::string("Not enough disk space on E:. Nothing was recorded. Choose another folder."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::ExportDiskFull, "F:\\out\\a.mp4"),
                std::string("Not enough disk space on F:. The export was stopped and the partial file deleted."));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::MicLost), std::string(str::kErrMicLost));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::ExportDeviceLost), std::string(str::kErrExportDeviceLost));
    CT_CHECK_EQ(ErrorMessage(ErrorKind::DeviceLostNotSaved), std::string(str::kErrDeviceLostNotSaved));
}

CT_TEST(recorder_failure_views_name_the_recovery) {
    using recorder::FailureCause;
    // Disk full while recording: stopped and saved, choose another folder, open / show the kept recording.
    ErrorView v = DescribeFailure(Failure(FailureCause::DiskFull, true, HRESULT_FROM_WIN32(ERROR_DISK_FULL)));
    CT_CHECK_EQ(v.message, std::string("Not enough disk space on D:. Recording stopped and saved."));
    CT_CHECK(v.actions[0] == kActionChooseFolder && v.actions[1] == kActionOpenEditor && v.actions[2] == kActionShowFolder);
    CT_CHECK(!v.recreate_device);
    CT_CHECK(v.extra.find("2026-10-18 10-00-00.ctrec") != std::string::npos);
    // Disk full before anything was written.
    v = DescribeFailure(Failure(FailureCause::DiskFull, false));
    CT_CHECK_EQ(v.message, ErrorMessage(ErrorKind::DiskFullNotStarted, "D:"));
    CT_CHECK(v.actions[0] == kActionChooseFolder && v.actions[1] == kActionNone);
    // Output folder unusable.
    v = DescribeFailure(Failure(FailureCause::OutputFolder, false));
    CT_CHECK_EQ(v.message, ErrorMessage(ErrorKind::OutputFolder, "D:\\Videos\\Camtesia"));
    CT_CHECK(v.actions[0] == kActionChooseFolder);
    // Device lost: recreate the device; the kept recording can be opened.
    v = DescribeFailure(Failure(FailureCause::DeviceLost, true, DXGI_ERROR_DEVICE_REMOVED));
    CT_CHECK_EQ(v.message, std::string(str::kErrDeviceLost));
    CT_CHECK(v.recreate_device);
    CT_CHECK(v.actions[0] == kActionOpenEditor && v.actions[1] == kActionShowFolder);
    CT_CHECK(v.extra.find(str::kErrDeviceRecreated) != std::string::npos);
    v = DescribeFailure(Failure(FailureCause::DeviceLost, false, DXGI_ERROR_DEVICE_REMOVED));
    CT_CHECK_EQ(v.message, std::string(str::kErrDeviceLostNotSaved));
    CT_CHECK(v.actions[0] == kActionNone);
    // No encoder, access lost, other.
    CT_CHECK_EQ(DescribeFailure(Failure(FailureCause::NoEncoder, false)).message, std::string(str::kErrNoEncoder));
    CT_CHECK_EQ(DescribeFailure(Failure(FailureCause::AccessLost, true)).message, std::string(str::kErrAccessLost));
    CT_CHECK_EQ(DescribeFailure(Failure(FailureCause::Other, false)).message, std::string("Recording failed: log text"));
    // A file that could not be finalized says so in the details.
    recorder::SessionFailure nf = Failure(FailureCause::DeviceLost, true);
    nf.finalized = false;
    CT_CHECK(DescribeFailure(nf).extra.find(str::kErrNotFinalized) != std::string::npos);
}

CT_TEST(recorder_warning_views_banner_only_for_the_microphone) {
    recorder::SessionWarning w;
    w.cause = recorder::WarningCause::MicBusy;
    w.message = "The microphone is in use by another app; recording without it.";
    ErrorView v = DescribeWarning(w);
    CT_CHECK(v.banner);
    CT_CHECK_EQ(v.message, std::string(str::kErrMicBusy));
    w.cause = recorder::WarningCause::AudioDeviceLost;
    v = DescribeWarning(w);
    CT_CHECK(v.banner);
    CT_CHECK_EQ(v.message, std::string(str::kErrMicLost));
    w.cause = recorder::WarningCause::LowDisk;
    w.message = "Less than 1 GB of disk space left for recordings.";
    v = DescribeWarning(w);
    CT_CHECK(!v.banner);   // a toast
    CT_CHECK_EQ(v.message, w.message);
}

CT_TEST(export_failure_views_name_the_recovery) {
    ErrorView v = DescribeExportFailure(HRESULT_FROM_WIN32(ERROR_DISK_FULL), "The disk is full.", "F:\\out\\a.mp4");
    CT_CHECK_EQ(v.message, ErrorMessage(ErrorKind::ExportDiskFull, "F:"));
    CT_CHECK(v.actions[0] == kActionChooseFolder);
    v = DescribeExportFailure(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), "x", "Q:\\gone\\a.mp4");
    CT_CHECK_EQ(v.message, ErrorMessage(ErrorKind::ExportFolder, "Q:\\gone"));
    CT_CHECK(v.actions[0] == kActionChooseFolder);
    v = DescribeExportFailure(DXGI_ERROR_DEVICE_REMOVED, "x", "F:\\a.mp4");
    CT_CHECK_EQ(v.message, std::string(str::kErrExportDeviceLost));
    CT_CHECK(v.actions[0] == kActionRetry);
    v = DescribeExportFailure(MF_E_TOPO_CODEC_NOT_FOUND, "x", "F:\\a.mp4");
    CT_CHECK_EQ(v.message, std::string(str::kErrNoEncoder));
    CT_CHECK(v.actions[0] == kActionNone);
    v = DescribeExportFailure(E_FAIL, "Rendering failed.", "F:\\a.mp4");
    CT_CHECK_EQ(v.message, std::string("Export failed: Rendering failed."));
    CT_CHECK(v.actions[0] == kActionRetry);
}

CT_TEST(error_banner_action_buttons_click) {
    Headless h(800, 300);
    ErrorBanner b;
    b.SetLogDir("C:\\logs");
    ShowErrorView(b, DescribeFailure(Failure(recorder::FailureCause::DiskFull, true, HRESULT_FROM_WIN32(ERROR_DISK_FULL))),
                  HRESULT_FROM_WIN32(ERROR_DISK_FULL));
    CT_CHECK_EQ(b.ActionCount(), size_t(3));
    CT_CHECK(b.ActionIdAt(0) == kActionChooseFolder && b.ActionIdAt(2) == kActionShowFolder && b.ActionIdAt(3) == 0);
    float height = 0;
    h.Frame([&] {
        CT_CHECK(b.Draw("##err"));
        height = ImGui::GetCursorScreenPos().y;
    });
    CT_CHECK(!h.IdConflict());
    // Click down the left edge of the text column (x = pad + icon + gap + 10) until a button answers: the first one
    // is "Choose another folder...".
    int clicked = 0;
    for (float y = 4; y < height && !clicked; y += 4) {
        h.Mouse(48, y);
        h.Frame([&] { b.Draw("##err"); });
        h.Button(0, true);
        h.Frame([&] { b.Draw("##err"); });
        h.Button(0, false);
        h.Frame([&] { b.Draw("##err"); });
        clicked = b.TakeAction();
    }
    CT_CHECK_EQ(clicked, static_cast<int>(kActionChooseFolder));
    CT_CHECK(b.Visible());               // the caller decides when the problem is solved
    CT_CHECK_EQ(b.TakeAction(), 0);      // read once
    b.Show("again");                     // Show clears the actions
    CT_CHECK_EQ(b.ActionCount(), size_t(0));
}

CT_TEST(level_monitor_falloff_is_20_db_per_second) {
    CT_CHECK_EQ(LevelMonitor::Falloff(-10.0f, -60.0f, 0.5f), -20.0f);
    CT_CHECK_EQ(LevelMonitor::Falloff(-10.0f, -5.0f, 0.5f), -5.0f);    // a louder packet jumps up at once
    CT_CHECK_EQ(LevelMonitor::Falloff(-110.0f, -120.0f, 1.0f), -120.0f);   // never below the floor
    CT_CHECK_EQ(LevelMonitor::Falloff(-10.0f, -60.0f, 5.0f), -20.0f);  // dt clamped to 0.5 s (a stalled frame)
    LevelMonitor m;
    CT_CHECK(m.GetStatus() == LevelMonitor::Status::Stopped);
    CT_CHECK_EQ(m.PeakDb(), -120.0f);
}

// The live meter on a real stream: a tone on the default output, measured through the loopback source (this machine
// has no microphone; a microphone, when present, must at least open and run).
CT_TEST_DEVICE(live_meter_follows_a_real_stream) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    audio::WasapiRender out;
    Tone tone;
    if (FAILED(out.Init(L"", 20)) || !out.Start(tone)) {
        CoUninitialize();
        CT_SKIP("no audio output device");
    }
    {
        LevelMonitor m;
        m.Start(LevelMonitor::Source::SystemAudio, L"");
        CT_CHECK(m.Active());
        CT_CHECK(WaitFor([&] { return m.GetStatus() != LevelMonitor::Status::Starting; }, 5000));
        CT_REQUIRE(m.GetStatus() == LevelMonitor::Status::Running);
        CT_CHECK(WaitFor([&] { return m.PeakDb() > -40.0f; }, 3000));   // -20 dBFS tone (after the endpoint volume)
        std::printf("loopback peak %.1f dBFS\n", m.PeakDb());
        const float shown = m.Shown(0.016f);
        CT_CHECK(shown > -40.0f);
        m.Stop();
        CT_CHECK(m.GetStatus() == LevelMonitor::Status::Stopped);
        CT_CHECK_EQ(m.PeakDb(), -120.0f);

        LevelMonitor mic;
        mic.Start(LevelMonitor::Source::Microphone, L"");
        CT_CHECK(WaitFor([&] { return mic.GetStatus() != LevelMonitor::Status::Starting; }, 5000));
        std::printf("microphone monitor: %s\n", mic.GetStatus() == LevelMonitor::Status::Running ? "running" : "not available");
    }
    out.Stop();
    CoUninitialize();
}

// Settings > Audio > Test microphone: the meter runs while the Audio section shows and stops when it is left.
CT_TEST_DEVICE(settings_audio_meter_test_runs_and_stops) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    audio::WasapiRender out;
    Tone tone;
    if (FAILED(out.Init(L"", 20)) || !out.Start(tone)) {
        CoUninitialize();
        CT_SKIP("no audio output device");
    }
    {
        const std::string dir = fs::TempPath("");
        fs::CreateDirectories(dir);
        Headless h(900, 640);
        SettingsWindow w(PathJoin(dir, "settings.json"));
        w.SetFillViewport(true);
        w.Open(Settings::Defaults(), SettingsSection::Audio);
        h.Frame([&] { w.Draw(); }, false);
        CT_CHECK(!w.MeterTestActive());
        w.StartMeterTest(/*system_audio=*/true);   // the loopback stands in for a microphone
        CT_CHECK(w.MeterTestActive());
        CT_CHECK(w.WantsRedraw());
        bool loud = false;
        for (int i = 0; i < 300 && !loud; ++i) {
            h.Frame([&] { w.Draw(); }, false);
            loud = w.MeterTestDb() > -40.0f;
            Sleep(10);
        }
        CT_CHECK(loud);
        CT_CHECK(!h.IdConflict());
        w.SetSection(SettingsSection::Hotkeys);
        h.Frame([&] { w.Draw(); }, false);
        CT_CHECK(!w.MeterTestActive());   // leaving the section stops the device
        w.SetSection(SettingsSection::Audio);
        w.StartMeterTest(true);
        w.Close();
        CT_CHECK(!w.MeterTestActive());   // closing stops it too
        fs::DeleteTree(dir);
    }
    out.Stop();
    CoUninitialize();
}
