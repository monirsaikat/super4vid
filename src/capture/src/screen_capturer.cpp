#define CT_LOG_CATEGORY "capture"
#include <ct/capture/screen_capturer.h>

#include <ct/core/assert.h>
#include <ct/core/error.h>
#include <ct/core/log.h>

#include <algorithm>

namespace ct::capture {

namespace {

D3D11_TEXTURE2D_DESC TexDesc(SizeI size, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(size.w);
    d.Height = static_cast<UINT>(size.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return d;
}

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

} // namespace

ScreenCapturer::~ScreenCapturer() { Stop(); }

HRESULT ScreenCapturer::Init(platform::D3DDevice& dev, const Config& cfg) {
    if (m_thread.IsRunning()) return E_UNEXPECTED;
    if (cfg.fps_num <= 0 || cfg.fps_den <= 0 || cfg.nv12_pool_size == 0) return E_INVALIDARG;
    m_dev = &dev;
    m_cfg = cfg;
    m_cfg.region = ClampRegionToDisplay(cfg.display, cfg.region);
    m_output_size = SizeI{m_cfg.region.w, m_cfg.region.h};

    DesktopDuplicator::Config dc;
    dc.display = m_cfg.display;
    dc.region = m_cfg.region;
    dc.prefer_fp16_when_hdr = m_cfg.hdr_tonemap;
    CT_RETURN_IF_FAILED(m_dup.Init(dev, dc));

    CT_RETURN_IF_FAILED(m_bgra_pool.Init(dev, TexDesc(m_dup.Size(), m_dup.Format()), 3, "capture.bgra"));

    ColorConverter::Config cc;
    cc.src_size = m_dup.Size();
    cc.src_format = m_dup.Format();
    cc.rotation = m_dup.DisplayRotation();
    cc.hdr_tonemap = m_cfg.hdr_tonemap;
    cc.sdr_white_nits = m_cfg.display.sdr_white_nits;
    cc.dst_size = m_output_size;
    cc.force_video_processor = m_cfg.force_video_processor;
    cc.allow_compute = m_cfg.allow_compute;
    CT_RETURN_IF_FAILED(m_conv.Init(dev, cc));

    D3D11_TEXTURE2D_DESC nv12 = TexDesc(m_output_size, DXGI_FORMAT_NV12);
    nv12.BindFlags = m_conv.DstBindFlags();
    CT_RETURN_IF_FAILED(m_nv12_pool.Init(dev, nv12, m_cfg.nv12_pool_size, "capture.nv12"));
    CT_LOG_IF_FAILED(m_gpu_timer.Init(dev));
    CT_LOG_INFO("ScreenCapturer: %dx%d @ %d/%d fps, NV12 pool %u", m_output_size.w, m_output_size.h, m_cfg.fps_num,
                m_cfg.fps_den, m_cfg.nv12_pool_size);
    return S_OK;
}

void ScreenCapturer::ResetStats() {
    for (auto* c : {&m_stats.ticks, &m_stats.new_frames, &m_stats.repeats, &m_stats.dropped_pool_empty, &m_stats.dropped_sink,
                    &m_stats.late_ticks, &m_stats.access_lost, &m_stats.reinits})
        c->store(0, std::memory_order_relaxed);
    m_stats.acquire.Reset();
    m_stats.convert.Reset();
    m_stats.gpu.Reset();
    m_stats.first_frame_us.store(-1, std::memory_order_relaxed);
    m_stats.last_new_frame_qpc.store(0, std::memory_order_relaxed);
}

bool ScreenCapturer::Start(int64_t epoch_qpc, FrameSink& sink) {
    if (!m_dev || m_thread.IsRunning()) return false;
    ResetStats();
    m_sink = &sink;
    m_epoch_qpc = epoch_qpc;
    m_pause_accum_qpc.store(0, std::memory_order_relaxed);
    m_paused.store(false, std::memory_order_relaxed);
    m_stop.store(false, std::memory_order_relaxed);
    m_stop_event.Reset();
    Thread::Options opt;
    opt.name = "ct-capture";
    opt.priority = THREAD_PRIORITY_TIME_CRITICAL;
    opt.mmcss_task = L"Capture";
    return m_thread.Start(opt, [this] { Run(); });
}

void ScreenCapturer::Pause() {
    if (m_paused.exchange(true, std::memory_order_acq_rel)) return;
    m_pause_qpc.store(Clock::QpcNow(), std::memory_order_release);
}

void ScreenCapturer::Resume() {
    if (!m_paused.load(std::memory_order_acquire)) return;
    m_pause_accum_qpc.fetch_add(Clock::QpcNow() - m_pause_qpc.load(std::memory_order_acquire), std::memory_order_acq_rel);
    m_paused.store(false, std::memory_order_release);
    m_wake_event.Set();
}

void ScreenCapturer::Stop() {
    if (!m_thread.Handle()) return;
    m_stop.store(true, std::memory_order_release);
    m_stop_event.Set();
    m_wake_event.Set();
    if (!m_thread.Join(5000)) CT_FATAL("ct-capture did not stop within 5 s");
    m_sink = nullptr;
}

void ScreenCapturer::Run() {
    HighResTimer timer;
    if (!timer.Init()) CT_LOG_WARN("ScreenCapturer: no waitable timer; pacing falls back to spinning");
    const int64_t freq = Clock::QpcFrequency();
    const int64_t ms_qpc = freq / 1000;
    const int64_t reinit_interval_qpc = freq / 4;   // 250 ms
    const int fps_num = m_cfg.fps_num, fps_den = m_cfg.fps_den;
    const double frame_qpc = static_cast<double>(freq) * fps_den / fps_num;
    ID3D11DeviceContext4* ctx = m_dev->Context();

    int64_t n = 0;
    bool lost = false;
    int64_t next_reinit_qpc = 0;
    uint32_t cur = 0;            // copy path: BGRA pool slot holding the newest desktop image
    bool bgra_pending = false;   // copy path: bgra[cur] is newer than the last NV12 frame (not converted yet)
    bool has_last = false;       // an NV12 pool texture (last_idx) holds the newest delivered frame
    uint32_t last_idx = 0;
    HANDLE handles[2] = {m_stop_event.Handle(), m_wake_event.Handle()};

    while (!m_stop.load(std::memory_order_acquire)) {
        if (m_paused.load(std::memory_order_acquire)) {
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            continue;
        }
        const int64_t base_qpc = m_epoch_qpc + m_pause_accum_qpc.load(std::memory_order_acquire);
        const TimeUs t_us = FrameTimeUs(n, fps_num, fps_den);
        const int64_t tick_qpc = Clock::UsToQpc(t_us, base_qpc);
        if (!timer.WaitUntilQpc(tick_qpc - ms_qpc, m_stop_event.Handle())) break;
        if (m_paused.load(std::memory_order_acquire)) continue;   // paused while waiting: this tick never happened

        HRESULT fatal = S_OK;
        {
            platform::D3DDevice::Lock lock(*m_dev);
            int64_t now = Clock::QpcNow();
            if (lost && now >= next_reinit_qpc) {
                const HRESULT hr = m_dup.Reinit();
                if (SUCCEEDED(hr)) {
                    lost = false;
                    m_stats.reinits.fetch_add(1, std::memory_order_relaxed);
                } else if (IsDeviceLost(hr)) {
                    fatal = hr;
                } else {
                    next_reinit_qpc = now + reinit_interval_qpc;
                }
            }
            bool got = false;                       // a new desktop image this tick
            ID3D11Texture2D* direct_src = nullptr;  // zero-copy: the acquired desktop image (held until the end of the tick)
            D3D11_BOX direct_box{};
            if (!lost && SUCCEEDED(fatal)) {
                now = Clock::QpcNow();
                const int64_t timeout_qpc = tick_qpc + 2 * ms_qpc - now;
                const uint32_t timeout_ms =
                    static_cast<uint32_t>(std::clamp<int64_t>((timeout_qpc + ms_qpc - 1) / ms_qpc, 0, 4));
                int64_t present_qpc = 0;
                const int64_t t0 = Clock::QpcNow();
                HRESULT hr;
                if (m_cfg.zero_copy && !m_conv.UsingVideoProcessorFallback() && m_dup.CanAcquireDirect()) {
                    hr = m_dup.AcquireDirect(timeout_ms, direct_src, direct_box, present_qpc);
                    if (hr != S_OK) direct_src = nullptr;
                } else {
                    const uint32_t next = (cur + 1) % m_bgra_pool.Count();
                    hr = m_dup.AcquireInto(m_bgra_pool.At(next), timeout_ms, present_qpc);
                    if (hr == S_OK) {
                        cur = next;
                        bgra_pending = true;
                    }
                }
                if (hr == S_OK) {
                    // Time from the later of "call" and "desktop presented" to return: the cost of acquiring, not the
                    // wait for the desktop to change.
                    const int64_t t1 = Clock::QpcNow();
                    m_stats.acquire.AddUs(Clock::QpcToUs(t1, std::max(t0, std::min(present_qpc, t1))));
                    got = true;
                    m_stats.new_frames.fetch_add(1, std::memory_order_relaxed);
                    m_stats.last_new_frame_qpc.store(t1, std::memory_order_relaxed);
                } else if (hr == E_NOTIMPL) {
                    // The direct path stopped being possible (mode change, no SRV binding); AcquireInto from now on.
                } else if (hr == DXGI_ERROR_ACCESS_LOST) [[unlikely]] {
                    m_stats.access_lost.fetch_add(1, std::memory_order_relaxed);
                    lost = true;
                    next_reinit_qpc = Clock::QpcNow();
                } else if (FAILED(hr)) [[unlikely]] {
                    fatal = hr;
                }
            }

            if (SUCCEEDED(fatal) && (got || bgra_pending)) {
                // A new image (or one the copy path could not convert last tick): convert it into a fresh pool texture.
                uint32_t idx = 0;
                ID3D11Texture2D* nv12 = m_nv12_pool.Acquire(idx);
                if (!nv12) [[unlikely]] {
                    m_stats.dropped_pool_empty.fetch_add(1, std::memory_order_relaxed);
                } else {
                    const int64_t c0 = Clock::QpcNow();
                    m_gpu_timer.Begin(ctx);
                    const HRESULT hr = direct_src ? m_conv.ConvertRegion(ctx, direct_src, direct_box, nv12)
                                                  : m_conv.Convert(ctx, m_bgra_pool.At(cur), nv12);
                    m_gpu_timer.End(ctx);
                    m_stats.convert.AddUs(Clock::QpcToUs(Clock::QpcNow(), c0));
                    if (FAILED(hr)) [[unlikely]] {
                        m_nv12_pool.Release(idx);
                        fatal = hr;
                    } else {
                        bgra_pending = false;
                        has_last = true;
                        last_idx = idx;
                        int64_t expected = -1;
                        m_stats.first_frame_us.compare_exchange_strong(expected, t_us, std::memory_order_relaxed);
                        if (!m_sink->OnFrame(nv12, idx, t_us)) {
                            m_nv12_pool.Release(idx);
                            m_stats.dropped_sink.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            } else if (SUCCEEDED(fatal) && has_last) {
                // No new image: re-send the previous NV12 texture (no GPU work, no extra pool texture). Only this
                // thread writes pool textures, so last_idx still holds that frame even if the sink released it.
                m_stats.repeats.fetch_add(1, std::memory_order_relaxed);
                m_nv12_pool.AddRef(last_idx);
                if (!m_sink->OnFrame(m_nv12_pool.At(last_idx), last_idx, t_us)) {
                    m_nv12_pool.Release(last_idx);
                    m_stats.dropped_sink.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (direct_src) m_dup.ReleaseDirect();
            double gpu_ms = 0;
            if (m_gpu_timer.Resolve(ctx, gpu_ms)) m_stats.gpu.AddUs(static_cast<int64_t>(gpu_ms * 1000.0));
        }
        if (FAILED(fatal)) [[unlikely]] {
            CT_LOG_ERROR("ScreenCapturer: capture failed: %s", HrToString(fatal).c_str());
            m_sink->OnError(fatal);
            break;
        }

        ++n;
        m_stats.ticks.fetch_add(1, std::memory_order_relaxed);
        // Catch up by skipping ticks when more than 1.5 frames late: the file keeps CFR, the skipped ticks are drops.
        const int64_t now = Clock::QpcNow();
        if (static_cast<double>(now - tick_qpc) > 1.5 * frame_qpc) [[unlikely]] {
            const int64_t current = FrameIndexAt(Clock::QpcToUs(now, base_qpc), fps_num, fps_den);
            if (current > n) {
                m_stats.late_ticks.fetch_add(static_cast<uint64_t>(current - n), std::memory_order_relaxed);
                n = current;
            }
        }
    }
}

} // namespace ct::capture
