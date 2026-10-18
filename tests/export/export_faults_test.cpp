// ExportJob error paths (P11 task 11.5), forced through ExportRequest::faults: a full disk and a lost device while
// writing (the partial file is deleted, the HRESULT says why), a missing hardware encoder (the software encoder takes
// over) and no encoder at all (a clear message, nothing written).
#include "../common/test_device.h"
#include "../common/test_media.h"
#include "../common/test_project.h"

#include <ct/audio/devices.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/export/export_job.h>
#include <ct/media/media_info.h>
#include <ct/platform/win.h>
#include <ct/render/render_context.h>
#include <ct/test/ct_test.h>

#include <mferror.h>

#include <cstdio>
#include <string>

using namespace ct;
using namespace ct::exportng;

namespace {

constexpr TimeUs kPatternUs = 5'000'000;

struct FaultEnv {
    platform::D3DDevice* dev = nullptr;
    render::RenderContext rc;
    audio::AudioDevices devices;
    std::string bundle, dir;
    bool Init(platform::D3DDevice* d, const std::string& pattern) {
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        dev = d;
        bundle = fs::TempPath(".ctrec");
        dir = fs::TempPath("");
        fs::CreateDirectories(dir);
        return SUCCEEDED(rc.Init(*d)) && test::MakeBundle(bundle, pattern, 1920, 1080, kPatternUs, false);
    }
    ~FaultEnv() {
        if (!bundle.empty()) fs::DeleteTree(bundle);
        if (!dir.empty()) fs::DeleteTree(dir);
    }
    std::string Out(const char* name) const { return PathJoin(dir, name); }

    // Runs an mp4-720p export of `seconds` with the faults; null when the job could not start or did not finish.
    const ExportJob::Progress* Run(ExportJob& job, const char* name, const ExportFaults& f, TimeUs seconds_us) {
        ExportRequest r;
        r.project_snapshot = test::MakeExportProject(bundle, 1920, 1080, kPatternUs, seconds_us);
        const Preset* pr = Preset::FindBuiltin("mp4-720p");
        if (!pr) return nullptr;
        r.preset = *pr;
        r.output_path = Out(name);
        r.faults = f;
        if (FAILED(job.Start(*dev, rc, devices, std::move(r)))) return nullptr;
        if (!job.Join(300'000)) return nullptr;
        return &job.GetProgress();
    }
};

} // namespace

CT_TEST_DEVICE(export_fault_disk_full_stops_and_deletes_partial) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    FaultEnv env;
    CT_REQUIRE(env.Init(dev, pat));
    ExportFaults f;
    f.write_error_hr = HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    f.write_error_after_samples = 20;
    ExportJob job;
    const ExportJob::Progress* pr = env.Run(job, "full.mp4", f, 2'000'000);
    CT_REQUIRE(pr && pr->finished.load());
    CT_CHECK(!pr->cancelled.load());
    CT_CHECK(pr->hr == HRESULT_FROM_WIN32(ERROR_DISK_FULL));
    std::printf("    disk full: %s (%lld of %lld frames)\n", pr->error.c_str(), static_cast<long long>(pr->frames_done.load()),
                static_cast<long long>(pr->frames_total.load()));
    CT_CHECK(pr->frames_done.load() < pr->frames_total.load());
    CT_CHECK(!fs::Exists(env.Out("full.mp4")));   // never a partial file
}

CT_TEST_DEVICE(export_fault_device_lost_reports_device_removed) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    FaultEnv env;
    CT_REQUIRE(env.Init(dev, pat));
    ExportFaults f;
    f.write_error_hr = DXGI_ERROR_DEVICE_REMOVED;
    f.write_error_after_samples = 20;
    ExportJob job;
    const ExportJob::Progress* pr = env.Run(job, "lost.mp4", f, 2'000'000);
    CT_REQUIRE(pr && pr->finished.load());
    CT_CHECK(pr->hr == DXGI_ERROR_DEVICE_REMOVED);
    CT_CHECK(!fs::Exists(env.Out("lost.mp4")));
}

CT_TEST_DEVICE(export_fault_no_hardware_encoder_uses_software) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    FaultEnv env;
    CT_REQUIRE(env.Init(dev, pat));
    ExportFaults f;
    f.hw_encoder_init_hr = MF_E_TOPO_CODEC_NOT_FOUND;
    ExportJob job;
    const ExportJob::Progress* pr = env.Run(job, "sw.mp4", f, 1'000'000);
    CT_REQUIRE(pr && pr->finished.load());
    CT_CHECK_HR(pr->hr);
    CT_CHECK(pr->software_encoder);
    std::printf("    software export: encoder %s, %.2fx real time\n", pr->encoder.c_str(), pr->speed_realtime.load());
    media::MediaInfo info;
    CT_REQUIRE(SUCCEEDED(media::MediaInfo::Load(env.Out("sw.mp4"), info)));
    CT_CHECK(info.FirstVideo() >= 0);
}

CT_TEST_DEVICE(export_fault_no_encoder_reports_clearly) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    FaultEnv env;
    CT_REQUIRE(env.Init(dev, pat));
    ExportFaults f;
    f.encoder_init_hr = MF_E_TOPO_CODEC_NOT_FOUND;
    ExportJob job;
    const ExportJob::Progress* pr = env.Run(job, "none.mp4", f, 1'000'000);
    CT_REQUIRE(pr && pr->finished.load());
    CT_CHECK(pr->hr == MF_E_TOPO_CODEC_NOT_FOUND);
    CT_CHECK(pr->error.find("No video encoder") != std::string::npos);
    CT_CHECK_EQ(pr->frames_done.load(), int64_t{0});
    CT_CHECK(!fs::Exists(env.Out("none.mp4")));
}
