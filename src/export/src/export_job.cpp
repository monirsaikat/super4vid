#define CT_LOG_CATEGORY "export"
#include <ct/export/export_job.h>

#include <ct/capture/color_convert.h>
#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/time.h>
#include <ct/export/gif_encoder.h>
#include <ct/export/scaler.h>
#include <ct/media/encode_config.h>
#include <ct/media/image_io.h>
#include <ct/media/mp4_writer.h>
#include <ct/platform/win.h>
#include <ct/playback/cache_budget.h>
#include <ct/render/compositor.h>
#include <ct/render/readback.h>
#include <ct/render/timeline_player.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

namespace ct::exportng {

using Microsoft::WRL::ComPtr;

static_assert(static_cast<int>(VideoCodec::Hevc) == static_cast<int>(media::VideoCodec::Hevc));
static_assert(static_cast<int>(RateControl::Cbr) == static_cast<int>(media::RateControl::Cbr) &&
              static_cast<int>(RateControl::Vbr) == static_cast<int>(media::RateControl::Vbr));

namespace {

constexpr size_t kWindowFrames = 50;     // MediaCache paused window (0.75 s at 60 fps) + pinned frames
constexpr int kGifPaletteStride = 30;    // two-pass global palette: every 30th frame
constexpr TimeUs kM4aChunkUs = 100'000;  // audio-only export step

int Even(int v) { return std::max(16, v & ~1); }

HRESULT MakeTarget(platform::D3DDevice& dev, SizeI size, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv,
                   ComPtr<ID3D11ShaderResourceView>& srv) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(size.w);
    d.Height = static_cast<UINT>(size.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    CT_RETURN_IF_FAILED(dev.Device()->CreateTexture2D(&d, nullptr, tex.GetAddressOf()));
    CT_RETURN_IF_FAILED(dev.Device()->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf()));
    CT_RETURN_IF_FAILED(dev.Device()->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf()));
    return S_OK;
}

HRESULT MakeStaging(platform::D3DDevice& dev, SizeI size, ComPtr<ID3D11Texture2D>& tex) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(size.w);
    d.Height = static_cast<UINT>(size.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return dev.Device()->CreateTexture2D(&d, nullptr, tex.GetAddressOf());
}

std::string LowerExt(const std::string& path) { return ToLowerAscii(PathExt(path)); }

} // namespace

struct ExportJob::Impl {
    platform::D3DDevice* dev = nullptr;
    render::RenderContext* rc = nullptr;
    ExportRequest req;
    TimeUs start = 0, end = 0;
    SizeI canvas, out;
    int fps_num = 60, fps_den = 1;

    render::TimelinePlayer tp;
    render::Compositor comp;
    render::FrameInputs inputs;
    ComPtr<ID3D11Texture2D> canvas_tex, scaled_tex;
    ComPtr<ID3D11RenderTargetView> canvas_rtv, scaled_rtv;
    ComPtr<ID3D11ShaderResourceView> canvas_srv, scaled_srv;
    int64_t inexact_frames = 0;
    size_t budget_extra = 0;
    double resolve_ms = 0, render_ms = 0, lock_ms = 0, compose_ms = 0, gpu_ms = 0;
    int64_t gpu_frames = 0;

    ID3D11Texture2D* Final() const { return scaled_tex ? scaled_tex.Get() : canvas_tex.Get(); }

    HRESULT Setup() {
        CT_RETURN_IF_FAILED(MakeTarget(*dev, canvas, canvas_tex, canvas_rtv, canvas_srv));
        if (out.w != canvas.w || out.h != canvas.h) CT_RETURN_IF_FAILED(MakeTarget(*dev, out, scaled_tex, scaled_rtv, scaled_srv));
        render::TimelinePlayer::Options o;
        o.audio_output = false;
        o.async_open = false;
        CT_RETURN_IF_FAILED(tp.Init(*rc, nullptr, o));
        tp.SetProject(&req.project_snapshot);
        // The job's MediaCaches share the process-wide decoded-frame budget with the editor's. Each needs its paused
        // prefetch window (~0.75 s of frames) or the export stalls on evictions: grow the budget for the job's
        // lifetime by that much per video file (screen + camera for recordings).
        size_t extra = 0;
        for (const project::Media& md : req.project_snapshot.media) {
            if (md.kind == project::Media::Kind::Audio || md.kind == project::Media::Kind::Image) continue;
            const size_t frame = static_cast<size_t>(std::max(md.width, 640)) * static_cast<size_t>(std::max(md.height, 360)) * 3 / 2;
            extra += frame * kWindowFrames * (md.kind == project::Media::Kind::Recording && md.has_camera ? 2 : 1);
        }
        budget_extra = extra;
        playback::CacheBudget::AddExtra(static_cast<int64_t>(budget_extra));
        platform::D3DDevice::Lock lock(*dev);
        return comp.Init(*rc);
    }

    // Resolves and renders timeline time t into Final(). With `then`, runs it under the same device lock (copy/convert).
    template <class Then>
    HRESULT Render(TimeUs t, Then&& then) {
        Stopwatch sw;
        sw.Start();
        if (!tp.ResolveExact(t, inputs, req.frame_timeout_ms)) {
            if (inexact_frames++ == 0) CT_LOG_WARN("export: frame at %lld us rendered without every exact source frame", static_cast<long long>(t));
        }
        const double r = sw.ElapsedMs();
        resolve_ms += r;
        HRESULT hr = S_OK;
        {
            platform::D3DDevice::Lock lock(*dev);
            lock_ms += sw.ElapsedMs() - r;
            render::RenderOptions ro;
            ro.gpu_timing = req.gpu_timing;
            hr = comp.RenderFrame(req.project_snapshot, t, inputs, canvas_rtv.Get(), ro);
            compose_ms += comp.LastTiming().total_ms;
            if (req.gpu_timing && comp.LastTiming().gpu_total_ms >= 0) {
                gpu_ms += comp.LastTiming().gpu_total_ms;
                ++gpu_frames;
            }
            if (SUCCEEDED(hr) && scaled_tex) hr = Scaler::Downscale(*rc, canvas_srv.Get(), canvas, scaled_rtv.Get(), out);
            if (SUCCEEDED(hr)) hr = then();
        }
        render_ms += sw.ElapsedMs() - r;
        return hr;
    }
    HRESULT Render(TimeUs t) { return Render(t, [] { return S_OK; }); }
};

ExportJob::ExportJob() = default;

ExportJob::~ExportJob() {
    Cancel();
    m_thread.Join();
}

SizeI ExportJob::OutputSize(const Preset& p, const project::Canvas& c) {
    if (p.video.width <= 0 || p.video.height <= 0 || c.width <= 0 || c.height <= 0) return SizeI{Even(c.width), Even(c.height)};
    const int w = Even(p.video.width);
    const int h = Even(static_cast<int>(std::lround(static_cast<double>(w) * c.height / c.width)));
    return SizeI{w, h};
}

void ExportJob::OutputFps(const Preset& p, const project::Canvas& c, int& num, int& den) {
    if (p.container == Preset::Container::Gif) {
        num = std::clamp(p.gif.fps, 1, 50);
        den = 1;
        return;
    }
    if (p.video.fps_num > 0 && p.video.fps_den > 0) {
        num = p.video.fps_num;
        den = p.video.fps_den;
        return;
    }
    num = c.fps_num > 0 ? c.fps_num : 60;
    den = c.fps_den > 0 ? c.fps_den : 1;
}

int64_t ExportJob::FrameCount(TimeUs start, TimeUs end, int num, int den) {
    if (end <= start || num <= 0 || den <= 0) return 0;
    const long double frames = static_cast<long double>(end - start) * num / (1e6L * den);
    int64_t n = static_cast<int64_t>(std::ceil(frames - 1e-9L));
    return std::max<int64_t>(n, 1);
}

void ExportJob::Cancel() { m_cancel.store(true, std::memory_order_release); }

bool ExportJob::Join(uint32_t timeout_ms) { return m_thread.Join(timeout_ms); }

HRESULT ExportJob::Start(platform::D3DDevice& dev, render::RenderContext& rc, audio::AudioDevices&, ExportRequest req) {
    if (m_thread.IsRunning() || m) return E_ILLEGAL_METHOD_CALL;
    if (!rc.IsInitialized() || req.output_path.empty()) return E_INVALIDARG;
    m = std::make_unique<Impl>();
    m->dev = &dev;
    m->rc = &rc;
    const project::Project& p = req.project_snapshot;
    m->start = req.range_start;
    m->end = req.range_end;
    if (req.preset.container == Preset::Container::Png) {
        m->end = m->start + 1;   // one frame at range_start
    } else if (m->end <= m->start) {
        m->start = 0;
        m->end = p.Duration();
    }
    if (req.preset.container != Preset::Container::Png && m->end <= m->start) return E_INVALIDARG;   // empty project
    m->canvas = SizeI{p.canvas.width, p.canvas.height};
    if (m->canvas.IsEmpty()) return E_INVALIDARG;
    m->out = OutputSize(req.preset, p.canvas);
    OutputFps(req.preset, p.canvas, m->fps_num, m->fps_den);
    m->req = std::move(req);
    m_cancel = false;
    Thread::Options o;
    o.name = "ct-export";
    o.priority = m->req.below_normal_priority ? -1 : 0;   // BELOW_NORMAL by default: the editor stays responsive
    if (!m_thread.Start(o, [this] { Main(); })) return E_FAIL;
    return S_OK;
}

void ExportJob::Main() {
    Impl& j = *m;
    Progress& pr = m_progress;
    Stopwatch sw;
    sw.Start();
    const Preset& preset = j.req.preset;
    const std::string& path = j.req.output_path;
    HRESULT hr = S_OK;
    std::string error;
    bool cancelled = false;
    const double fps = static_cast<double>(j.fps_num) / j.fps_den;
    auto progress = [&](int64_t done, int64_t total, double media_s) {
        pr.frames_done.store(done, std::memory_order_relaxed);
        pr.fraction.store(total > 0 ? static_cast<double>(done) / total : 1.0, std::memory_order_relaxed);
        const double el = sw.ElapsedMs() / 1000.0;
        pr.elapsed_s.store(el, std::memory_order_relaxed);
        if (el > 0) pr.speed_realtime.store(media_s / el, std::memory_order_relaxed);
        if (done > 0) pr.eta_us.store(static_cast<int64_t>(el * 1e6 * static_cast<double>(total - done) / done), std::memory_order_relaxed);
    };
    auto fail = [&](HRESULT h, std::string msg) {
        if (SUCCEEDED(hr)) {
            hr = h;
            error = std::move(msg);
            CT_LOG_ERROR("export: %s (%s)", error.c_str(), HrToString(h).c_str());
        }
    };

    hr = j.Setup();
    if (FAILED(hr)) error = "The export could not prepare the GPU resources (" + HrToString(hr) + ").";

    const int64_t total = preset.container == Preset::Container::Png ? 1
                          : preset.container == Preset::Container::M4a ? (j.end - j.start + kM4aChunkUs - 1) / kM4aChunkUs
                                                                       : FrameCount(j.start, j.end, j.fps_num, j.fps_den);
    pr.frames_total.store(total);

    if (SUCCEEDED(hr) && preset.container == Preset::Container::Mp4) {
        media::Mp4Writer w;
        media::Mp4Writer::Config cfg;
        cfg.path = path;
        cfg.has_video = true;
        media::VideoEncodeConfig& v = cfg.video;
        v.codec = static_cast<media::VideoCodec>(preset.video.codec);
        v.size = j.out;
        v.fps_num = j.fps_num;
        v.fps_den = j.fps_den;
        v.rc = static_cast<media::RateControl>(preset.video.rc);
        v.quality = preset.video.quality;
        v.bitrate_bps = preset.video.bitrate_kbps * 1000u;
        v.max_bitrate_bps = preset.video.max_bitrate_kbps * 1000u;
        v.gop_seconds = preset.video.gop_s;
        v.bframes = preset.video.bframes;
        v.low_latency = false;
        media::AudioEncodeConfig a;
        a.sample_rate = 48000;   // the writer (and every mix) is 48 kHz; 44.1 kHz presets export at 48 kHz
        a.channels = std::clamp(preset.audio.channels, 1, 2);
        a.bitrate_bps = preset.audio.bitrate_kbps * 1000u;
        a.label = "audio";
        cfg.audio.push_back(a);
        cfg.fast_start = false;   // not supported by Mp4Writer (DECISIONS P04 4.3): moov stays at the end
        // B-frames and the encoder pipeline keep several input textures referenced: a deeper pool and queue keep
        // WriteVideoSync from waiting on the encoder every frame.
        cfg.sync_pool_size = 16;
        cfg.video_queue = 16;
        capture::ColorConverter conv;
        capture::ColorConverter::Config cc;
        cc.src_size = j.out;
        cc.dst_size = j.out;
        // Pipelined: this thread resolves, renders and converts frame n into one of kSlots NV12 textures (and mixes its
        // audio) while "ct-export-write" hands frame n-1 to the writer (WriteVideoSync waits for the encoder).
        constexpr int kSlots = 3;
        ComPtr<ID3D11Texture2D> nv12[kSlots];
        HRESULT h = w.Init(*j.dev, cfg, nullptr);
        if (FAILED(h)) fail(h, w.ErrorMessage());
        else if (!w.Start()) fail(E_FAIL, "The video encoder could not start.");
        if (SUCCEEDED(hr)) {
            pr.encoder = w.EncoderName();
            h = conv.Init(*j.dev, cc);
            for (int i = 0; i < kSlots && SUCCEEDED(h); ++i) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = static_cast<UINT>(j.out.w);
                d.Height = static_cast<UINT>(j.out.h);
                d.MipLevels = 1;
                d.ArraySize = 1;
                d.Format = DXGI_FORMAT_NV12;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = conv.DstBindFlags();
                h = j.dev->Device()->CreateTexture2D(&d, nullptr, nv12[i].GetAddressOf());
            }
            if (FAILED(h)) fail(h, "The colour converter could not be created (" + HrToString(h) + ").");
        }
        struct Item {
            int slot = 0;
            TimeUs t = 0;
            std::vector<float> audio;
            uint32_t frames = 0;
        };
        SrwLock qlock;
        std::deque<Item> queue;
        Event q_wake(false, false), slot_free(false, false);
        std::atomic<bool> busy[kSlots] = {};
        std::atomic<bool> producer_done{false};
        std::atomic<HRESULT> write_hr{S_OK};
        double write_ms = 0;
        Thread writer;
        if (SUCCEEDED(hr)) {
            Thread::Options wo;
            wo.name = "ct-export-write";
            wo.priority = j.req.below_normal_priority ? -1 : 0;
            writer.Start(wo, [&] {
                for (;;) {
                    Item it;
                    bool have = false;
                    {
                        ExclusiveGuard g(qlock);
                        if (!queue.empty()) {
                            it = std::move(queue.front());
                            queue.pop_front();
                            have = true;
                        }
                    }
                    if (!have) {
                        if (producer_done.load(std::memory_order_acquire)) {
                            ExclusiveGuard g(qlock);
                            if (queue.empty()) break;
                            continue;
                        }
                        q_wake.Wait(20);
                        continue;
                    }
                    Stopwatch ws;
                    ws.Start();
                    HRESULT wh = SUCCEEDED(write_hr.load()) ? w.WriteVideoSync(nv12[it.slot].Get(), it.t - j.start) : S_OK;
                    busy[it.slot].store(false, std::memory_order_release);
                    slot_free.Set();
                    if (SUCCEEDED(wh) && SUCCEEDED(write_hr.load()) && it.frames > 0)
                        wh = w.WriteAudioSync(0, it.audio.data(), it.frames, it.t - j.start);
                    write_ms += ws.ElapsedMs();
                    if (FAILED(wh) && SUCCEEDED(write_hr.load())) {
                        write_hr.store(wh);
                        slot_free.Set();
                    }
                }
            });
        }
        const int ch = std::clamp(preset.audio.channels, 1, 2);
        std::vector<float> abuf;
        for (int64_t n = 0; SUCCEEDED(hr) && n < total; ++n) {
            if (m_cancel.load(std::memory_order_acquire)) {
                cancelled = true;
                break;
            }
            if (FAILED(write_hr.load())) break;
            const int slot = static_cast<int>(n % kSlots);
            while (busy[slot].load(std::memory_order_acquire) && SUCCEEDED(write_hr.load())) slot_free.Wait(20);
            if (FAILED(write_hr.load())) break;
            const TimeUs t = j.start + FrameTimeUs(n, j.fps_num, j.fps_den);
            const TimeUs t1 = std::min(j.end, j.start + FrameTimeUs(n + 1, j.fps_num, j.fps_den));
            ID3D11Texture2D* dst = nv12[slot].Get();
            h = j.Render(t, [&] { return conv.Convert(j.dev->Context(), j.Final(), dst); });
            if (FAILED(h)) {
                fail(h, "Rendering failed at " + FormatTimecode(t, j.fps_num, j.fps_den, true) + " (" + HrToString(h) + ").");
                break;
            }
            Item it;
            it.slot = slot;
            it.t = t;
            it.frames = render::TimelinePlayer::AudioFramesBetween(t, t1);
            if (it.frames > 0) {
                Stopwatch as;
                as.Start();
                abuf.resize(static_cast<size_t>(it.frames) * 2);
                j.tp.RenderAudio(t, t1, abuf.data());
                if (ch == 1) {
                    it.audio.resize(it.frames);
                    for (uint32_t i = 0; i < it.frames; ++i) it.audio[i] = 0.5f * (abuf[2 * i] + abuf[2 * i + 1]);
                } else {
                    it.audio = abuf;
                }
                pr.audio_ms += as.ElapsedMs();
            }
            busy[slot].store(true, std::memory_order_release);
            {
                ExclusiveGuard g(qlock);
                queue.push_back(std::move(it));
            }
            q_wake.Set();
            if (n == 0) pr.first_frame_ms = sw.ElapsedMs();
            progress(n + 1, total, static_cast<double>(n + 1) / fps);
        }
        producer_done.store(true, std::memory_order_release);
        q_wake.Set();
        writer.Join();
        pr.video_ms = write_ms;
        if (FAILED(write_hr.load())) fail(write_hr.load(), w.ErrorMessage());
        const HRESULT fh = w.Finalize();
        if (SUCCEEDED(hr) && !cancelled && FAILED(fh)) fail(fh, w.ErrorMessage());
        {
            platform::D3DDevice::Lock lock(*j.dev);
            conv.Shutdown();
        }
    } else if (SUCCEEDED(hr) && preset.container == Preset::Container::Gif) {
        GifEncoder enc;
        GifEncoder::Options go;
        go.max_colors = preset.gif.max_colors;
        go.dither = preset.gif.dither;
        go.per_frame_palette = preset.gif.per_frame_palette;
        go.frame_diff = true;
        ComPtr<ID3D11Texture2D> staging[2];
        HRESULT h = MakeStaging(*j.dev, j.out, staging[0]);
        if (SUCCEEDED(h)) h = MakeStaging(*j.dev, j.out, staging[1]);
        if (FAILED(h)) fail(h, "The export could not allocate readback buffers.");
        if (SUCCEEDED(hr) && !enc.Open(path, j.out.w, j.out.h, go)) fail(E_FAIL, "The GIF file could not be created: " + path);
        std::vector<uint8_t> frame(static_cast<size_t>(j.out.w) * j.out.h * 4);
        const int stride = j.out.w * 4;
        // Copies a mapped staging texture into `frame` (under the device lock, so AddFrame never blocks the editor).
        auto map_copy = [&](ID3D11Texture2D* s) {
            platform::D3DDevice::Lock lock(*j.dev);
            D3D11_MAPPED_SUBRESOURCE mp{};
            const HRESULT mh = j.dev->Context()->Map(s, 0, D3D11_MAP_READ, 0, &mp);
            if (FAILED(mh)) return mh;
            for (int r = 0; r < j.out.h; ++r)
                std::memcpy(frame.data() + static_cast<size_t>(r) * stride, static_cast<const uint8_t*>(mp.pData) + static_cast<size_t>(r) * mp.RowPitch,
                            static_cast<size_t>(stride));
            j.dev->Context()->Unmap(s, 0);
            return S_OK;
        };
        const int delay = GifEncoder::DelayForFps(j.fps_num / std::max(1, j.fps_den));
        // Pass 1: global palette from every 30th frame.
        if (SUCCEEDED(hr) && !go.per_frame_palette) {
            for (int64_t n = 0; n < total && SUCCEEDED(hr); n += kGifPaletteStride) {
                if (m_cancel.load()) {
                    cancelled = true;
                    break;
                }
                const TimeUs t = j.start + FrameTimeUs(n, j.fps_num, j.fps_den);
                h = j.Render(t, [&] {
                    j.dev->Context()->CopyResource(staging[0].Get(), j.Final());
                    return S_OK;
                });
                if (SUCCEEDED(h)) h = map_copy(staging[0].Get());
                if (FAILED(h)) fail(h, "Rendering failed (" + HrToString(h) + ").");
                else enc.AddPaletteSample(frame.data(), stride);
            }
        }
        // Pass 2: render frame n while frame n-1's readback is mapped (one-frame delay) and quantized on the IO pool.
        for (int64_t n = 0; SUCCEEDED(hr) && !cancelled && n <= total; ++n) {
            if (m_cancel.load()) {
                cancelled = true;
                break;
            }
            if (n < total) {
                const TimeUs t = j.start + FrameTimeUs(n, j.fps_num, j.fps_den);
                h = j.Render(t, [&] {
                    j.dev->Context()->CopyResource(staging[n & 1].Get(), j.Final());
                    return S_OK;
                });
                if (FAILED(h)) {
                    fail(h, "Rendering failed (" + HrToString(h) + ").");
                    break;
                }
            }
            if (n > 0) {
                Stopwatch vs;
                vs.Start();
                h = map_copy(staging[(n - 1) & 1].Get());
                if (FAILED(h)) {
                    fail(h, "Reading the rendered frame failed (" + HrToString(h) + ").");
                    break;
                }
                if (!enc.AddFrame(frame.data(), stride, delay)) {
                    fail(E_FAIL, "Writing the GIF failed (disk full?): " + path);
                    break;
                }
                pr.video_ms += vs.ElapsedMs();
                progress(n, total, static_cast<double>(n) / fps);
            }
        }
        if (!enc.Close() && SUCCEEDED(hr) && !cancelled) fail(E_FAIL, "Writing the GIF failed (disk full?): " + path);
    } else if (SUCCEEDED(hr) && preset.container == Preset::Container::Png) {
        HRESULT h = j.Render(j.start);
        std::vector<uint8_t> px;
        SizeI size;
        if (SUCCEEDED(h)) h = render::Readback::ToBgra8(*j.dev, j.Final(), px, size);
        if (SUCCEEDED(h)) {
            const std::string ext = LowerExt(path);
            h = (ext == ".jpg" || ext == ".jpeg") ? media::ImageIo::SaveJpeg(path, px.data(), size.w, size.h, size.w * 4, 0.92f)
                                                  : media::ImageIo::SavePng(path, px.data(), size.w, size.h, size.w * 4);
            if (FAILED(h)) fail(h, "The image could not be written: " + path);
        } else {
            fail(h, "Rendering failed (" + HrToString(h) + ").");
        }
        if (SUCCEEDED(hr)) progress(1, 1, 0);
    } else if (SUCCEEDED(hr) && preset.container == Preset::Container::M4a) {
        media::Mp4Writer w;
        media::Mp4Writer::Config cfg;
        cfg.path = path;
        cfg.has_video = false;
        media::AudioEncodeConfig a;
        a.channels = std::clamp(preset.audio.channels, 1, 2);
        a.bitrate_bps = preset.audio.bitrate_kbps * 1000u;
        a.label = "audio";
        cfg.audio.push_back(a);
        HRESULT h = w.Init(*j.dev, cfg, nullptr);
        if (FAILED(h)) fail(h, w.ErrorMessage());
        else if (!w.Start()) fail(E_FAIL, "The audio encoder could not start.");
        std::vector<float> abuf, mono;
        for (int64_t k = 0; SUCCEEDED(hr) && k < total; ++k) {
            if (m_cancel.load()) {
                cancelled = true;
                break;
            }
            const TimeUs t = j.start + k * kM4aChunkUs;
            const TimeUs t1 = std::min(j.end, t + kM4aChunkUs);
            const uint32_t frames = render::TimelinePlayer::AudioFramesBetween(t, t1);
            abuf.resize(static_cast<size_t>(frames) * 2);
            j.tp.RenderAudio(t, t1, abuf.data());
            const float* src = abuf.data();
            if (a.channels == 1) {
                mono.resize(frames);
                for (uint32_t i = 0; i < frames; ++i) mono[i] = 0.5f * (abuf[2 * i] + abuf[2 * i + 1]);
                src = mono.data();
            }
            h = w.WriteAudioSync(0, src, frames, t - j.start);
            if (FAILED(h)) {
                fail(h, w.ErrorMessage());
                break;
            }
            progress(k + 1, total, static_cast<double>(t1 - j.start) / 1e6);
        }
        const HRESULT fh = w.Finalize();
        if (SUCCEEDED(hr) && !cancelled && FAILED(fh)) fail(fh, w.ErrorMessage());
    }

    pr.resolve_ms = j.resolve_ms;
    pr.render_ms = j.render_ms;
    pr.lock_wait_ms = j.lock_ms;
    pr.compose_cpu_ms = j.compose_ms;
    pr.compose_gpu_avg_ms = j.gpu_frames > 0 ? j.gpu_ms / static_cast<double>(j.gpu_frames) : -1;
    pr.inexact_frames = j.inexact_frames;
    j.tp.Shutdown();
    if (j.budget_extra) playback::CacheBudget::AddExtra(-static_cast<int64_t>(j.budget_extra));
    {
        platform::D3DDevice::Lock lock(*j.dev);
        j.comp.Shutdown();
        j.inputs = render::FrameInputs{};
    }
    if (cancelled || FAILED(hr)) {
        fs::Delete(path);   // never leave a partial file
    } else {
        pr.output_bytes = fs::FileSize(path);
        progress(total, total, static_cast<double>(j.end - j.start) / 1e6);
    }
    pr.elapsed_s.store(sw.ElapsedMs() / 1000.0);
    pr.hr = cancelled ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : hr;
    pr.error = cancelled ? std::string("Export cancelled.") : error;
    CT_LOG_INFO("export: %s %s, %lld frames, %.2f s, %.2fx real time, %s", cancelled ? "cancelled" : (FAILED(hr) ? "failed" : "done"),
                path.c_str(), static_cast<long long>(pr.frames_done.load()), pr.elapsed_s.load(), pr.speed_realtime.load(),
                FormatBytes(pr.output_bytes).c_str());
    pr.cancelled.store(cancelled, std::memory_order_release);
    pr.finished.store(true, std::memory_order_release);
    if (j.req.on_finished) Async::PostToUi(j.req.on_finished);
}

} // namespace ct::exportng
