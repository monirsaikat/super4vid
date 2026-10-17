#pragma once
// ct/capture/screen_capturer.h
// Constant-frame-rate screen capture: a pacer thread ("ct-capture", TIME_CRITICAL, MMCSS "Capture") ticks at
// fps_num/fps_den, acquires the newest desktop image (or repeats the previous one), converts it to NV12 and hands it to
// a FrameSink with a tick-aligned timestamp (docs/03 section 4).
// Thread model: Init/Start/Pause/Resume/Stop from the owner thread (UI/session); Stats() from any thread (atomics).
// The capture thread owns the duplicator and converter; it uses the immediate context under D3DDevice::Lock.
// Lifetime: Init -> [Nv12Pool() setup] -> Start -> (Pause/Resume)* -> Stop. The destructor calls Stop().
// No allocation on the capture thread after Start (view caches fill during the first frames).

#include <ct/capture/color_convert.h>
#include <ct/capture/desktop_duplicator.h>
#include <ct/capture/display.h>
#include <ct/capture/frame_sink.h>
#include <ct/core/stats.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/gpu_timer.h>
#include <ct/platform/texture_pool.h>

#include <atomic>
#include <cstdint>

namespace ct::capture {

struct CaptureStats {
    std::atomic<uint64_t> ticks{0}, new_frames{0}, repeats{0}, dropped_pool_empty{0}, dropped_sink{0}, late_ticks{0},
        access_lost{0}, reinits{0};
    Stats::Timing acquire;   // AcquireInto calls that returned S_OK (includes the wait for the frame, max 4 ms)
    Stats::Timing convert;   // CPU time of ColorConverter::Convert (command submission)
    Stats::Timing gpu;       // GPU time of the NV12 conversion passes (GpuTimer; resolved a few frames late)
    std::atomic<int64_t> first_frame_us{-1};   // t_us of the first frame delivered (tick-aligned), -1 before
    std::atomic<int64_t> last_new_frame_qpc{0};   // QPC when the last new desktop image arrived
};

class ScreenCapturer {
public:
    struct Config {
        DisplayInfo display;
        RectI region;                  // virtual-screen px; clamped with ClampRegionToDisplay by Init
        int fps_num = 60, fps_den = 1;
        bool hdr_tonemap = true;
        uint32_t nv12_pool_size = 12;
        bool force_video_processor = false;   // tests/bench: ColorConverter fallback path
    };

    ScreenCapturer() = default;
    ~ScreenCapturer();
    ScreenCapturer(const ScreenCapturer&) = delete;
    ScreenCapturer& operator=(const ScreenCapturer&) = delete;

    // Creates duplicator, converter, BGRA pool(3), NV12 pool (bind RT|SR, DXGI_FORMAT_NV12) and timers.
    HRESULT Init(platform::D3DDevice&, const Config&);
    platform::TexturePool& Nv12Pool() { return m_nv12_pool; }   // P04 attaches reuse predicate/user data before Start
    // Spawns ct-capture. Frame n has t_us = FrameTimeUs(n) relative to epoch_qpc (plus pause shifts); the first frame
    // delivered is the first tick with a desktop image, so the stream's first timestamp is usually not 0.
    bool Start(int64_t epoch_qpc, FrameSink& sink);
    void Pause();    // stops ticking
    void Resume();   // shifts the tick base by the pause length so media time has no gap
    void Stop();     // joins the thread (5 s, then CT_FATAL)
    const CaptureStats& Stats() const { return m_stats; }
    SizeI OutputSize() const { return m_output_size; }
    const RectI& Region() const { return m_cfg.region; }
    bool IsRunning() const { return m_thread.IsRunning(); }

private:
    void Run();
    void ResetStats();

    platform::D3DDevice* m_dev = nullptr;
    Config m_cfg;
    DesktopDuplicator m_dup;
    ColorConverter m_conv;
    platform::TexturePool m_bgra_pool;
    platform::TexturePool m_nv12_pool;
    platform::GpuTimer m_gpu_timer;
    Thread m_thread;
    Event m_stop_event{true, false};
    Event m_wake_event{false, false};
    FrameSink* m_sink = nullptr;
    CaptureStats m_stats;
    SizeI m_output_size{};
    int64_t m_epoch_qpc = 0;
    std::atomic<int64_t> m_pause_accum_qpc{0};
    std::atomic<int64_t> m_pause_qpc{0};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_stop{false};
};

} // namespace ct::capture
