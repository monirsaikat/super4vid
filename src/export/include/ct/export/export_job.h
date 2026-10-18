#pragma once
// ct/export/export_job.h
// Offline render of a project snapshot into MP4 (H.264/HEVC + AAC via Mp4Writer), animated GIF (GifEncoder), a PNG/JPEG
// still or audio-only M4A, on a "ct-export" thread (below normal priority). Frames go through the same Compositor and
// TimelinePlayer (own instance: own MediaCaches) as the preview, so the export is pixel-identical to it.
//
// Thread model: Start/Cancel/Join/GetProgress/the destructor from the owner (UI) thread; GetProgress fields are atomics
// (hr/error are written before `finished` becomes true: read them only after finished). The job thread takes
// D3DDevice::Lock around each GPU step, so the editor keeps rendering on the same device and RenderContext.
// Lifetime: Start once per job; the destructor cancels and joins. The RenderContext must outlive the job.

#include <ct/audio/devices.h>
#include <ct/core/thread.h>
#include <ct/core/types.h>
#include <ct/export/preset.h>
#include <ct/platform/d3d_device.h>
#include <ct/project/project.h>
#include <ct/render/render_context.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ct::exportng {

// Fault injection (P11 11.5, tests only): forces the MP4 error paths without a full disk / lost device / missing
// encoder. All off by default.
struct ExportFaults {
    HRESULT write_error_hr = S_OK;          // the n-th WriteSample fails with this (ERROR_DISK_FULL,
    uint64_t write_error_after_samples = 0; //   DXGI_ERROR_DEVICE_REMOVED) via Mp4Writer::InjectWriteErrorForTest
    HRESULT hw_encoder_init_hr = S_OK;      // the hardware-first writer setup "fails" with this: software retry
    HRESULT encoder_init_hr = S_OK;         // every writer setup "fails" with this: no encoder
};

struct ExportRequest {
    project::Project project_snapshot;   // a copy: edits during the export never reach the file
    Preset preset;
    std::string output_path;             // UTF-8; the folder must exist. PNG presets write JPEG for .jpg/.jpeg paths
    TimeUs range_start = 0, range_end = 0;   // 0,0 = whole project. PNG: the still is the frame at range_start
    // Additions.
    std::function<void()> on_finished;   // posted to the UI thread (Async::PostToUi) once Progress::finished is set
    uint32_t frame_timeout_ms = 10'000;  // per-frame wait for decoded media before the export fails
    bool gpu_timing = false;             // measure the compositor's GPU time (Progress::compose_gpu_avg_ms)
    bool below_normal_priority = true;   // false: normal priority (export_demo --normal-priority, benchmarks)
    ExportFaults faults;                 // P11 11.5 (tests)
};

class ExportJob {
public:
    ExportJob();
    ~ExportJob();
    ExportJob(const ExportJob&) = delete;
    ExportJob& operator=(const ExportJob&) = delete;

    // Validates the request (sizes, range, path) and starts the "ct-export" thread (THREAD_PRIORITY_BELOW_NORMAL).
    // The audio devices are not opened (export never plays sound); the parameter keeps the P10 interface.
    HRESULT Start(platform::D3DDevice&, render::RenderContext&, audio::AudioDevices&, ExportRequest);

    struct Progress {
        std::atomic<double> fraction{0};
        std::atomic<int64_t> frames_done{0}, frames_total{0};
        std::atomic<double> speed_realtime{0};   // exported media seconds per wall second
        std::atomic<int64_t> eta_us{0};
        std::atomic<bool> finished{false}, cancelled{false};
        HRESULT hr = S_OK;
        std::string error;                       // user-presentable when hr failed
        // Additions (valid after finished).
        std::atomic<double> elapsed_s{0};
        uint64_t output_bytes = 0;
        std::string encoder;                     // MP4: the video encoder the sink writer loaded
        bool software_encoder = false;           // MP4: the software encoder was used (no hardware encoder worked)
        // Wall time per stage, summed over the job (ms): ResolveExact (decode waits), render (compositor + scale +
        // convert/copy under the device lock), video (MP4: the ct-export-write thread's WriteVideoSync +
        // WriteAudioSync, overlapped with rendering; GIF: readback + AddFrame), audio (RenderAudio mixing).
        double resolve_ms = 0, render_ms = 0, video_ms = 0, audio_ms = 0;
        double lock_wait_ms = 0, compose_cpu_ms = 0;   // parts of render_ms: D3DDevice::Lock wait, RenderFrame CPU
        double compose_gpu_avg_ms = -1;          // RenderFrame GPU time per frame (ExportRequest::gpu_timing), -1 unknown
        int64_t inexact_frames = 0;
        double first_frame_ms = 0;               // MP4: start -> first frame written (decoder/encoder start-up)              // frames rendered without every exact source frame (timeouts)
    };
    const Progress& GetProgress() const { return m_progress; }
    void Cancel();                    // the job stops at the next frame and deletes the partial file
    bool Join(uint32_t timeout_ms);   // true when the thread finished (or never started)
    bool Running() const { return m_thread.IsRunning(); }

    // Pure helpers (tests, dialog).
    // Output size: preset size (width kept, height derived from the canvas aspect, both even) or the canvas.
    static SizeI OutputSize(const Preset&, const project::Canvas&);
    // Export frame rate: gif.fps for GIF, preset fps or the canvas fps otherwise.
    static void OutputFps(const Preset&, const project::Canvas&, int& num, int& den);
    // Frames in [start, end) at fps: ceil((end - start) * fps / 1e6).
    static int64_t FrameCount(TimeUs start, TimeUs end, int fps_num, int fps_den);

private:
    struct Impl;
    void Main();

    std::unique_ptr<Impl> m;
    Progress m_progress;
    std::atomic<bool> m_cancel{false};
    Thread m_thread;
};

} // namespace ct::exportng
