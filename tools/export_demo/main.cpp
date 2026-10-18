// export_demo: builds the P10 acceptance project from the tests/data pattern media (a synthetic .ctrec bundle with
// screen.mp4 + cam.mp4 + a synthetic events.ctev; 3 video layers: screen with cursor and keystroke badges, camera PIP,
// scaled overlay; 2 annotations; mic audio, looped to --seconds) and exports it with ct::exportng::ExportJob, printing
// progress, the speed against real time and the time spent per stage. With --project a .ctproj is exported instead;
// with --recording a .ctrec bundle is imported into a new project the way the editor's "Import recording" does it
// (default style, auto-zoom) and exported; --save-project writes that project as a .ctproj too (release smoke test).
//
//   export_demo [--preset mp4-1080p] [--seconds 60] [--out file] [--quality N] [--vbr KBPS]
//               [--pattern data/pattern_1080p60_5s.mp4] [--project file.ctproj] [--start S --end S] [--normal-priority]
//               [--no-bframes] [--no-camera] [--recording take.ctrec [--save-project file.ctproj]]
#include "test_project.h"

#include <ct/audio/devices.h>
#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/export/export_job.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/win.h>
#include <ct/project/importer.h>
#include <ct/project/serialize.h>
#include <ct/render/render_context.h>

#include <objbase.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace ct;
using namespace ct::exportng;

int main(int argc, char** argv) {
    std::string preset_id = "mp4-1080p", out, pattern = "tests/data/pattern_1080p60_5s.mp4", project_path, recording,
                save_project;
    double seconds = 60, start_s = 0, end_s = 0;
    int quality = -1, vbr = -1;
    bool normal_priority = false, no_bframes = false, no_camera = false;
    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--preset")) preset_id = next("--preset");
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next("--seconds"));
        else if (!std::strcmp(argv[i], "--out")) out = next("--out");
        else if (!std::strcmp(argv[i], "--quality")) quality = std::atoi(next("--quality"));
        else if (!std::strcmp(argv[i], "--vbr")) vbr = std::atoi(next("--vbr"));
        else if (!std::strcmp(argv[i], "--pattern")) pattern = next("--pattern");
        else if (!std::strcmp(argv[i], "--project")) project_path = next("--project");
        else if (!std::strcmp(argv[i], "--recording")) recording = next("--recording");
        else if (!std::strcmp(argv[i], "--save-project")) save_project = next("--save-project");
        else if (!std::strcmp(argv[i], "--start")) start_s = std::atof(next("--start"));
        else if (!std::strcmp(argv[i], "--end")) end_s = std::atof(next("--end"));
        else if (!std::strcmp(argv[i], "--normal-priority")) normal_priority = true;
        else if (!std::strcmp(argv[i], "--no-bframes")) no_bframes = true;
        else if (!std::strcmp(argv[i], "--no-camera")) no_camera = true;
        else {
            std::fprintf(stderr, "usage: export_demo [--preset id] [--seconds 60] [--out file] [--quality N] [--vbr KBPS]\n"
                                 "                   [--pattern mp4] [--project file.ctproj] [--start S --end S]\n"
                                 "                   [--recording take.ctrec [--save-project file.ctproj]]\n");
            return 2;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Async::Init(4);
    const Preset* bp = Preset::FindBuiltin(preset_id);
    if (!bp) {
        std::fprintf(stderr, "unknown preset %s\n", preset_id.c_str());
        return 2;
    }
    ExportRequest req;
    req.preset = *bp;
    if (quality >= 0) {
        req.preset.video.rc = RateControl::Quality;
        req.preset.video.quality = quality;
    }
    if (vbr > 0) {
        req.preset.video.rc = RateControl::Vbr;
        req.preset.video.bitrate_kbps = static_cast<uint32_t>(vbr);
        req.preset.video.max_bitrate_kbps = static_cast<uint32_t>(vbr) * 3 / 2;
    }
    if (no_bframes) req.preset.video.bframes = false;
    std::string bundle;
    if (!recording.empty()) {
        Err err;
        project::Project& p = req.project_snapshot;
        p.id = Uuid4();
        if (!project::ImportRecording(p, recording, &err)) {
            std::fprintf(stderr, "cannot import %s: %s\n", recording.c_str(), err.message.c_str());
            return 1;
        }
        std::printf("imported %s: %zu clips, %zu tracks, %.3f s\n", recording.c_str(), p.clips.size(), p.tracks.size(),
                    static_cast<double>(p.Duration()) / 1e6);
        if (!save_project.empty()) {
            if (!project::Save(p, save_project, &err)) {
                std::fprintf(stderr, "cannot save %s: %s\n", save_project.c_str(), err.message.c_str());
                return 1;
            }
            project::Project check;
            if (!project::Load(save_project, check, &err)) {
                std::fprintf(stderr, "cannot reload %s: %s\n", save_project.c_str(), err.message.c_str());
                return 1;
            }
            std::printf("saved and reloaded %s\n", save_project.c_str());
        }
    } else if (!project_path.empty()) {
        Err err;
        if (!project::Load(project_path, req.project_snapshot, &err)) {
            std::fprintf(stderr, "cannot load %s: %s\n", project_path.c_str(), err.message.c_str());
            return 1;
        }
    } else {
        if (!fs::Exists(pattern)) {
            std::fprintf(stderr, "pattern media %s not found (run from the repository root)\n", pattern.c_str());
            return 1;
        }
        bundle = fs::TempPath(".ctrec");
        if (!test::MakeBundle(bundle, pattern, 1920, 1080, 5'000'000, true)) {
            std::fprintf(stderr, "cannot create the test bundle\n");
            return 1;
        }
        req.project_snapshot = test::MakeExportProject(bundle, 1920, 1080, 5'000'000, static_cast<TimeUs>(seconds * 1e6));
        if (no_camera) req.project_snapshot.tracks[1].hidden = true;   // experiment: 2 layers, one decoder
    }
    if (out.empty()) out = fs::Absolute(std::string("export_demo") + ContainerExtension(req.preset.container));
    req.output_path = out;
    req.range_start = static_cast<TimeUs>(start_s * 1e6);
    req.range_end = static_cast<TimeUs>(end_s * 1e6);
    req.below_normal_priority = !normal_priority;
    req.gpu_timing = true;

    platform::D3DDevice dev;
    if (FAILED(dev.Init(false))) {
        std::fprintf(stderr, "no D3D11 device\n");
        return 1;
    }
    render::RenderContext rc;
    audio::AudioDevices devices;
    devices.Init();
    if (FAILED(rc.Init(dev))) return 1;
    std::printf("export_demo: %s, %.1f s project, %s -> %s\n", req.preset.name.c_str(), static_cast<double>(req.project_snapshot.Duration()) / 1e6,
                dev.AdapterName().c_str(), out.c_str());
    ExportJob job;
    const HRESULT hr = job.Start(dev, rc, devices, std::move(req));
    if (FAILED(hr)) {
        std::fprintf(stderr, "Start failed: %s\n", HrToString(hr).c_str());
        return 1;
    }
    const ExportJob::Progress& p = job.GetProgress();
    while (!job.Join(1000)) {
        std::printf("  %5.1f %%  %lld/%lld frames  %.2fx real time  ETA %.1f s\n", p.fraction.load() * 100.0,
                    static_cast<long long>(p.frames_done.load()), static_cast<long long>(p.frames_total.load()), p.speed_realtime.load(),
                    static_cast<double>(p.eta_us.load()) / 1e6);
    }
    const double el = p.elapsed_s.load();
    std::printf("%s: %s\n", SUCCEEDED(p.hr) ? "done" : "FAILED", SUCCEEDED(p.hr) ? out.c_str() : p.error.c_str());
    std::printf("  frames %lld in %.2f s = %.1f fps, %.2fx real time, %s, encoder %s, inexact frames %lld\n",
                static_cast<long long>(p.frames_done.load()), el, el > 0 ? static_cast<double>(p.frames_done.load()) / el : 0.0,
                p.speed_realtime.load(), FormatBytes(p.output_bytes).c_str(), p.encoder.c_str(), static_cast<long long>(p.inexact_frames));
    const double n = static_cast<double>(std::max<int64_t>(p.frames_done.load(), 1));
    std::printf("  start-up to the first frame: %.0f ms\n", p.first_frame_ms);
    std::printf("  per frame: resolve %.2f ms, render %.2f ms, video write %.2f ms, audio %.2f ms\n", p.resolve_ms / n,
                p.render_ms / n, p.video_ms / n, p.audio_ms / n);
    std::printf("  render detail: device lock wait %.2f ms, RenderFrame CPU %.2f ms, GPU %.2f ms\n", p.lock_wait_ms / n,
                p.compose_cpu_ms / n, p.compose_gpu_avg_ms);
    if (!bundle.empty()) fs::DeleteTree(bundle);
    rc.Shutdown();
    devices.Shutdown();
    Async::Shutdown();
    dev.Shutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return SUCCEEDED(p.hr) ? 0 : 1;
}
