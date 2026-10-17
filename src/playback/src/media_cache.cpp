#define CT_LOG_CATEGORY "playback"
#include <ct/playback/media_cache.h>

#include <ct/core/async.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/stats.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/image_io.h>
#include <ct/playback/cache_budget.h>
#include <ct/platform/win.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace ct::playback {

namespace {
std::atomic<int> g_cache_counter{0};
constexpr TimeUs kForwardReachUs = 3'000'000;   // decode forward instead of seeking when the gap is this close
constexpr int kThumbW = 160, kThumbH = 90;
constexpr size_t kMaxThumbs = 2000;
constexpr TimeUs kImageDurationUs = INT64_MAX / 4;

// Decoding forward from `pos` to `target` costs (target - pos) of decoding; a seek lands on the last keyframe <= target
// and costs (target - keyframe). Forward wins unless a keyframe lies in (pos, target]: a known one (learned from seek
// landings), or one predicted from the last known keyframe <= pos plus the GOP (smallest known keyframe distance).
// Without a GOP estimate, forward is used up to kUnknownGopReachUs.
bool ForwardIsCheaper(const media::VideoDecoder& dec, TimeUs pos, TimeUs target) {
    constexpr TimeUs kUnknownGopReachUs = 1'000'000;
    if (dec.AtEnd() || target + FrameCache::kContiguousUs < pos || target - pos > kForwardReachUs) return false;
    const std::vector<TimeUs>& keys = dec.KnownKeyframes();
    auto next = std::upper_bound(keys.begin(), keys.end(), pos);
    if (next != keys.end() && *next <= target) return false;
    TimeUs gop = 0;
    for (size_t i = 1; i < keys.size(); ++i) gop = gop == 0 ? keys[i] - keys[i - 1] : std::min(gop, keys[i] - keys[i - 1]);
    if (gop == 0) return target - pos <= kUnknownGopReachUs;
    if (next == keys.begin()) return target - pos <= gop;
    const TimeUs last_key = *(next - 1);
    return last_key + gop > target;
}

double QpcMs(int64_t from, int64_t to) { return static_cast<double>(to - from) * 1000.0 / static_cast<double>(Clock::QpcFrequency()); }
} // namespace

MediaCache::MediaCache() = default;
MediaCache::~MediaCache() { Close(); }

HRESULT MediaCache::Open(platform::D3DDevice& dev, std::string_view path) {
    Close();
    m_stop = false;
    m_dev = &dev;
    m_path = std::string(path);
    CT_RETURN_IF_FAILED(media::MediaInfo::Load(path, m_info));
    m_open = true;
    if (m_info.is_image) {
        CT_RETURN_IF_FAILED(media::ImageIo::LoadToTexture(dev, path, m_image, m_image_size));
        CT_RETURN_IF_FAILED(dev.Device()->CreateShaderResourceView(m_image.Get(), nullptr, m_image_srv.GetAddressOf()));
        return S_OK;
    }
    const int v = m_info.FirstVideo();
    if (v < 0) return S_OK;   // audio only

    media::VideoDecoder::Config cfg;
    cfg.path = m_path;
    cfg.stream_index = v;
    // No decoder pool: the prefetch thread copies each decoded frame straight into its cache slot (VideoDecoder::
    // DecodeFrame + CopyTo), one GPU copy per frame instead of two (decoder surface -> pool -> slot).
    cfg.pool_size = 0;
    CT_RETURN_IF_FAILED(m_dec.Init(dev, cfg));
    m_has_video = true;
    m_stats.decoder_pool_textures = m_dec.PoolTextures();
    m_fps_num = m_dec.FpsNum();
    m_fps_den = m_dec.FpsDen();
    m_frame_dur = m_dec.FrameDurationUs();
    m_media_end = m_info.duration_us > 0 ? m_info.duration_us : (m_dec.Duration() > 0 ? m_dec.Duration() : INT64_MAX / 4);
    const size_t frame_bytes = static_cast<size_t>(m_dec.Size().w) * m_dec.Size().h * 3 / 2;
    const size_t cap = std::clamp<size_t>(CacheBudget::Bytes() / std::max<size_t>(frame_bytes, 1) + 8, 16, 4096);
    m_slots.assign(cap, Slot{});
    m_free.clear();
    m_slots_used = 0;
    m_dec_next = 0;
    {
        ExclusiveGuard g(m_hint_lock);
        m_hint = HintState{0, 1.0, false, 1};
        m_jump_gen = 0;
    }
    const std::string name = Format("ct-decode-%d", g_cache_counter.fetch_add(1));
    Thread::Options opt;
    opt.name = name.c_str();
    opt.priority = 1;   // THREAD_PRIORITY_ABOVE_NORMAL
    if (!m_thread.Start(opt, [this] { PrefetchMain(); })) return E_FAIL;
    m_wake.Set();
    // Pre-warm the thumbnail decoder on the IO pool (creating a decoder takes 0.1-0.5 s on a loaded machine);
    // requests made meanwhile queue up and are served by this same job.
    {
        ExclusiveGuard g(m_aux_lock);
        m_thumb_job_running = true;
    }
    BeginJob();
    Async::Run([this] {
        ThumbJob();
        EndJob();
    });
    return S_OK;
}

void MediaCache::Close() {
    m_stop = true;
    m_wake.Set();
    m_thread.Join();
    {
        ExclusiveGuard g(m_aux_lock);
        m_thumb_queue.clear();
    }
    while (m_jobs.load() > 0) m_jobs_idle.Wait(100);
    m_dec.Shutdown();
    m_thumb_dec.reset();
    m_thumb_blit.reset();
    {
        ExclusiveGuard g(m_lock);
        CacheBudget::Add(-static_cast<int64_t>(m_cache.Bytes()));
        m_cache.Clear();
        m_slots.clear();
        m_free.clear();
        m_slots_used = 0;
        for (uint32_t& p : m_pinned) p = UINT32_MAX;
        m_stats = Stats{};
    }
    {
        ExclusiveGuard g(m_aux_lock);
        m_thumbs.clear();
        m_peaks.clear();
        m_peaks_requested.clear();
    }
    m_image_srv.Reset();
    m_image.Reset();
    m_has_video = false;
    m_open = false;
    m_seek_started_qpc = 0;
}

TimeUs MediaCache::FrameDurationUs() const { return m_has_video ? m_frame_dur : 0; }

void MediaCache::Hint(TimeUs playhead_us, double rate, bool playing) {
    if (!m_has_video) return;
    {
        ExclusiveGuard g(m_hint_lock);
        if (m_hint.t == playhead_us && m_hint.rate == rate && m_hint.playing == playing) return;
        const TimeUs dt = playhead_us - m_hint.t;
        // While paused every move is a scrub: the frame a running fast-forward is heading for is no longer wanted.
        // The abort keeps the decoder position, so a small move continues forward instead of seeking again.
        const bool jump = dt > 1'000'000 || dt < -1'000'000 || m_hint.playing != playing || (m_hint.rate < 0) != (rate < 0) ||
                          (!playing && dt != 0);
        m_hint.t = playhead_us;
        m_hint.rate = rate;
        m_hint.playing = playing;
        ++m_hint.gen;
        if (jump) m_jump_gen.fetch_add(1, std::memory_order_relaxed);
    }
    m_wake.Set();
}

MediaCache::HintState MediaCache::ReadHint() const {
    SharedGuard g(m_hint_lock);
    return m_hint;
}

MediaCache::Window MediaCache::ComputeWindow(const HintState& h) const {
    Window w;
    const TimeUs end = m_media_end.load();
    w.forward = h.rate >= 0;
    w.playhead = std::clamp<TimeUs>(h.t, 0, std::max<TimeUs>(end - 1, 0));
    if (h.playing) {
        w.start = w.forward ? w.playhead - 500'000 : w.playhead - 2'000'000;
        w.end = w.forward ? w.playhead + 2'000'000 : w.playhead + 500'000;
    } else {
        w.start = w.playhead - 250'000;
        w.end = w.playhead + 500'000;
    }
    w.start = std::max<TimeUs>(w.start, 0);
    w.end = std::min(w.end, end);
    return w;
}

TimeUs MediaCache::NextNeed(const Window& w) const {
    SharedGuard g(m_lock);
    if (w.forward) {
        const TimeUs a = m_cache.FirstGap(w.playhead, FrameCache::kContiguousUs);
        if (a < w.end) return a;
        const TimeUs b = m_cache.FirstGap(w.start, FrameCache::kContiguousUs);
        if (b < w.playhead) return b;
        return -1;
    }
    const TimeUs b = m_cache.FirstGap(w.start, FrameCache::kContiguousUs);
    return b < w.end ? b : -1;
}

int MediaCache::AcquireSlot(SizeI size) {
    // Caller holds m_lock exclusively.
    for (size_t i = 0; i < m_free.size(); ++i) {
        const uint32_t s = m_free[i];
        if (m_slots[s].size.w == size.w && m_slots[s].size.h == size.h) {
            m_free.erase(m_free.begin() + static_cast<ptrdiff_t>(i));
            return static_cast<int>(s);
        }
    }
    if (m_slots_used < m_slots.size()) return static_cast<int>(m_slots_used++);
    if (!m_free.empty()) {   // wrong size (stream changed size): recreate that slot's texture
        const uint32_t s = m_free.back();
        m_free.pop_back();
        m_slots[s] = Slot{};
        return static_cast<int>(s);
    }
    return -1;
}

bool MediaCache::StoreFrame(const media::VideoDecoder::PendingFrame& f, const Window& w) {
    const size_t bytes = static_cast<size_t>(f.size.w) * f.size.h * 3 / 2;
    int slot = -1;
    {
        ExclusiveGuard g(m_lock);
        const int same = m_cache.FindPts(f.pts_us);
        if (same >= 0) {
            m_cache.Touch(same, ++m_tick);
            return true;
        }
        for (;;) {
            if (CacheBudget::Fits(bytes)) {
                slot = AcquireSlot(f.size);
                if (slot >= 0) break;
            }
            const int v = m_cache.PickVictim(w.start, w.end, w.playhead, w.forward, m_pinned, kPinned);
            if (v < 0) return false;
            const FrameCache::Entry e = m_cache.Erase(v);
            m_free.push_back(e.slot);
            CacheBudget::Add(-static_cast<int64_t>(e.bytes));
            ++m_stats.evictions;
        }
    }
    Slot& s = m_slots[static_cast<size_t>(slot)];   // reserved: no entry references it, the UI never reads it
    if (!s.tex) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = static_cast<UINT>(f.size.w);
        d.Height = static_cast<UINT>(f.size.h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = m_dev->Device()->CreateTexture2D(&d, nullptr, s.tex.GetAddressOf());
        if (SUCCEEDED(hr)) hr = platform::Nv12Blit::CreatePlaneViews(m_dev->Device(), s.tex.Get(), s.y.GetAddressOf(), s.uv.GetAddressOf());
        if (FAILED(hr)) {
            CT_LOG_ERROR("MediaCache: frame texture %dx%d: %s", f.size.w, f.size.h, HrToString(hr).c_str());
            s = Slot{};
            ExclusiveGuard g(m_lock);
            m_free.push_back(static_cast<uint32_t>(slot));
            return false;
        }
        s.size = f.size;
    }
    // The slot is reserved (no entry references it and the UI never reads it), so the copy runs outside m_lock.
    if (const HRESULT hr = m_dec.CopyTo(f, s.tex.Get()); FAILED(hr)) {
        CT_LOG_WARN("MediaCache: frame copy failed: %s", HrToString(hr).c_str());
        ExclusiveGuard g(m_lock);
        m_free.push_back(static_cast<uint32_t>(slot));
        return false;
    }
    {
        ExclusiveGuard g(m_lock);
        FrameCache::Entry e;
        e.pts_us = f.pts_us;
        e.duration_us = f.duration_us;
        e.bytes = bytes;
        e.slot = static_cast<uint32_t>(slot);
        e.last_used = ++m_tick;
        m_cache.Insert(e);
        CacheBudget::Add(static_cast<int64_t>(bytes));
        ++m_stats.frames_decoded;
        m_stats.decoder_copies = m_dec.FramesCopied();
    }
    return true;
}

void MediaCache::PrefetchMain() {
    static auto& seek_timing = ::ct::Stats::T("decode.seek_to_frame");
    uint64_t stalled_gen = 0;
    while (!m_stop.load()) {
        HintState h = ReadHint();
        Window w = ComputeWindow(h);
        const TimeUs need = NextNeed(w);
        if (need < 0 || h.gen == stalled_gen) {
            m_wake.Wait(INFINITE);
            continue;
        }
        // Seek latency: from the first hint whose playhead frame is missing to that frame being cached.
        if (m_seek_started_qpc == 0 && !CachedExact(w.playhead)) {
            m_seek_started_qpc = Clock::QpcNow();
            m_seek_target = w.playhead;
        }
        const bool reachable = ForwardIsCheaper(m_dec, m_dec_next, need);
        if (!reachable) {
            const HRESULT hr = m_dec.Seek(need);
            if (FAILED(hr)) {
                CT_LOG_WARN("MediaCache: seek to %lld failed: %s", static_cast<long long>(need), HrToString(hr).c_str());
                m_wake.Wait(200);
                continue;
            }
            m_dec_next = need;
            ExclusiveGuard g(m_lock);
            ++m_stats.seeks;
        }
        TimeUs target = need;
        TimeUs skip_before = FillFrom(w, need);
        const uint64_t jump_gen = m_jump_gen.load(std::memory_order_relaxed);
        for (;;) {
            if (m_stop.load()) break;
            // Room for one more frame? Otherwise wait for the playhead to move (a new hint).
            {
                SharedGuard g(m_lock);
                const size_t bytes = static_cast<size_t>(m_dec.Size().w) * m_dec.Size().h * 3 / 2;
                const bool room = (CacheBudget::Fits(bytes) && (m_slots_used < m_slots.size() || !m_free.empty())) ||
                                  m_cache.PickVictim(w.start, w.end, w.playhead, w.forward, m_pinned, kPinned) >= 0;
                if (!room) {
                    stalled_gen = h.gen;
                    break;
                }
            }
            media::VideoDecoder::PendingFrame f;
            const HRESULT hr = m_dec.DecodeFrame(f, skip_before, &m_jump_gen, jump_gen);
            if (hr == E_ABORT) {
                // The decoder stays positioned after the last frame it skipped: the next request decodes forward
                // from there when that is cheaper than a seek (a scrub that moved a little), else seeks.
                m_dec_next = m_dec.NextPts() != INT64_MIN ? m_dec.NextPts() : INT64_MAX / 4;
                break;
            }
            if (hr == S_FALSE) {
                // End of stream: nothing exists at or after the decoder position.
                const TimeUs real_end = std::max<TimeUs>(m_dec_next, 1);
                if (real_end < m_media_end.load()) m_media_end = real_end;
                break;
            }
            if (FAILED(hr)) {
                CT_LOG_WARN("MediaCache: ReadFrame failed: %s", HrToString(hr).c_str());
                m_wake.Wait(200);
                break;
            }
            m_dec_next = f.pts_us + f.duration_us;
            const bool stored = StoreFrame(f, w);
            f.Reset();   // give the decoder its output surface back before the next DecodeFrame
            if (!stored && f.pts_us + f.duration_us <= target) {
                skip_before = target;   // no room for an optional frame behind the playhead: skip to the needed one
                continue;
            }
            if (!stored) {
                stalled_gen = h.gen;
                break;
            }
            if (m_seek_started_qpc != 0 && f.pts_us <= m_seek_target + FrameCache::kContiguousUs &&
                m_seek_target < f.pts_us + f.duration_us + FrameCache::kContiguousUs) {
                const int64_t now = Clock::QpcNow();
                seek_timing.AddUs(static_cast<int64_t>(QpcMs(m_seek_started_qpc, now) * 1000.0));
                ExclusiveGuard g(m_lock);
                m_stats.last_seek_to_frame_ms = QpcMs(m_seek_started_qpc, now);
                m_seek_started_qpc = 0;
            }
            m_inserted.Set();
            const HintState h2 = ReadHint();
            if (h2.gen != h.gen) {
                h = h2;
                w = ComputeWindow(h);
                // A newer playhead replaces the one being measured.
                if (CachedExact(w.playhead)) m_seek_started_qpc = 0;
                else if (m_seek_started_qpc == 0 || m_seek_target != w.playhead) {
                    m_seek_started_qpc = Clock::QpcNow();
                    m_seek_target = w.playhead;
                }
            }
            const TimeUs next = NextNeed(w);
            if (next < 0) break;
            // Continue forward only while that is cheaper than a seek; otherwise the outer loop seeks.
            if (!ForwardIsCheaper(m_dec, m_dec_next, next)) break;
            target = next;
            skip_before = FillFrom(w, next);
        }
    }
}

TimeUs MediaCache::FillFrom(const Window& w, TimeUs need) const {
    // Frames of the window behind the playhead that the decoder passes on its way to `need` are kept instead of
    // skipped: filling them later would take a second seek and a second decode of the GOP (up to 2 s of frames), which
    // keeps the decoder busy when the next scrub arrives.
    if (!w.forward || need < w.playhead) return need;
    SharedGuard g(m_lock);
    const TimeUs gap = m_cache.FirstGap(w.start, FrameCache::kContiguousUs);
    return gap < need ? gap : need;
}

bool MediaCache::CachedExact(TimeUs t) const {
    SharedGuard g(m_lock);
    bool exact = false;
    return m_cache.Find(t, exact) >= 0 && exact;
}

bool MediaCache::GetVideoFrame(TimeUs t_us, FrameRef& out, bool& exact) {
    out = FrameRef{};
    exact = false;
    if (!m_open) return false;
    if (m_image_srv) {
        out.rgba = m_image_srv.Get();
        out.size = m_image_size;
        out.pts_us = 0;
        out.duration_us = kImageDurationUs;
        exact = true;
        return true;
    }
    if (!m_has_video) return false;
    bool found = false;
    {
        ExclusiveGuard g(m_lock);
        const int i = m_cache.Find(t_us, exact);
        if (i >= 0) {
            m_cache.Touch(i, ++m_tick);
            const FrameCache::Entry& e = m_cache.At(i);
            const Slot& s = m_slots[e.slot];
            out.y = s.y.Get();
            out.uv = s.uv.Get();
            out.nv12 = s.tex.Get();
            out.pts_us = e.pts_us;
            out.duration_us = e.duration_us;
            out.size = s.size;
            PinRecent(m_pinned, e.slot);
            found = true;
        }
        if (exact) ++m_stats.hits;
        else ++m_stats.misses;
    }
    if (!exact) {
        const HintState h = ReadHint();
        const Window w = ComputeWindow(h);
        if (t_us < w.start || t_us >= w.end) Hint(t_us, h.rate, h.playing);
    }
    return found;
}

void MediaCache::PinRecent(uint32_t (&pins)[kPinned], uint32_t slot) {
    int at = 0;   // not pinned: the least recent entry drops out
    for (int i = 0; i < kPinned; ++i)
        if (pins[i] == slot) at = i;
    for (int i = at; i + 1 < kPinned; ++i) pins[i] = pins[i + 1];
    pins[kPinned - 1] = slot;
}

bool MediaCache::WaitForFrame(TimeUs t_us, uint32_t timeout_ms) {
    if (!m_has_video) return m_open;
    Hint(t_us, 1.0, false);
    Stopwatch sw;
    sw.Start();
    for (;;) {
        {
            SharedGuard g(m_lock);
            bool exact = false;
            if (m_cache.Find(t_us, exact) >= 0 && exact) return true;
        }
        const double left = timeout_ms - sw.ElapsedMs();
        if (left <= 0) return false;
        m_inserted.Wait(static_cast<uint32_t>(std::min(left, 20.0)) + 1);
    }
}

MediaCache::Stats MediaCache::GetStats() const {
    SharedGuard g(m_lock);
    Stats s = m_stats;
    s.cache_bytes = m_cache.Bytes();
    s.cached_frames = m_cache.Count();
    return s;
}

// --- audio -------------------------------------------------------------------------------------------------------

HRESULT MediaCache::AudioReader::Seek(TimeUs t) { return m_dec.Seek(t); }

uint32_t MediaCache::AudioReader::Read(float* out, uint32_t frames, TimeUs& pts) {
    uint32_t n = 0;
    const HRESULT hr = m_dec.Read(out, frames, n, pts);
    return hr == S_OK ? n : 0;
}

std::unique_ptr<MediaCache::AudioReader> MediaCache::OpenAudio(int stream_index) {
    if (!m_open) return nullptr;
    if (stream_index < 0) {
        const std::vector<int> a = m_info.AudioStreams();
        if (a.empty()) return nullptr;
        stream_index = a[0];
    }
    auto r = std::make_unique<AudioReader>();
    media::AudioDecoder::Config cfg;
    cfg.path = m_path;
    cfg.stream_index = stream_index;
    if (FAILED(r->m_dec.Init(cfg))) return nullptr;
    return r;
}

// --- IO pool jobs: thumbnails and peaks ------------------------------------------------------------------------------

void MediaCache::BeginJob() {
    if (m_jobs.fetch_add(1) == 0) m_jobs_idle.Reset();
}

void MediaCache::EndJob() {
    if (m_jobs.fetch_sub(1) == 1) m_jobs_idle.Set();
}

bool MediaCache::Thumbnail(TimeUs t_us, ID3D11ShaderResourceView*& srv) {
    srv = nullptr;
    if (!m_open) return false;
    if (m_image_srv) {
        srv = m_image_srv.Get();
        return true;
    }
    if (!m_has_video) return false;
    const int64_t bucket = std::max<TimeUs>(t_us, 0) / 1'000'000;
    ExclusiveGuard g(m_aux_lock);
    auto it = m_thumbs.find(bucket);
    if (it != m_thumbs.end()) {
        it->second.last_used = ++m_aux_tick;
        if (!it->second.ready) return false;
        srv = it->second.srv.Get();
        return true;
    }
    if (m_thumbs.size() >= kMaxThumbs) {
        auto victim = m_thumbs.end();
        for (auto t = m_thumbs.begin(); t != m_thumbs.end(); ++t)
            if (t->second.ready && (victim == m_thumbs.end() || t->second.last_used < victim->second.last_used)) victim = t;
        if (victim != m_thumbs.end()) m_thumbs.erase(victim);
    }
    Thumb& th = m_thumbs[bucket];
    th.last_used = ++m_aux_tick;
    m_thumb_queue.push_back(bucket);
    if (!m_thumb_job_running) {
        m_thumb_job_running = true;
        BeginJob();
        Async::Run([this] {
            ThumbJob();
            EndJob();
        });
    }
    return false;
}

void MediaCache::ThumbJob() {
    if (!m_thumb_dec) {
        auto dec = std::make_unique<media::VideoDecoder>();
        media::VideoDecoder::Config cfg;
        cfg.path = m_path;
        cfg.stream_index = m_info.FirstVideo();
        cfg.pool_size = 2;
        // A second DXVA session (not the spec's software decoder): under CPU load from other work the software
        // decoder needed > 1 s per thumbnail; GPU decode costs ~2 ms per frame and little CPU (DECISIONS.md 6.6).
        cfg.allow_hardware = true;
        auto blit = std::make_unique<platform::Nv12Blit>();
        HRESULT hr = dec->Init(*m_dev, cfg);
        if (FAILED(hr)) {
            cfg.allow_hardware = false;
            hr = dec->Init(*m_dev, cfg);
        }
        if (FAILED(hr) || FAILED(blit->Init(*m_dev))) {
            CT_LOG_WARN("MediaCache: thumbnail decoder unavailable for %s", m_path.c_str());
            ExclusiveGuard g(m_aux_lock);
            m_thumb_queue.clear();
            m_thumb_job_running = false;
            return;
        }
        m_thumb_dec = std::move(dec);
        m_thumb_blit = std::move(blit);
        m_thumb_dec_next = 0;
    }
    for (;;) {
        int64_t bucket = 0;
        {
            ExclusiveGuard g(m_aux_lock);
            if (m_thumb_queue.empty() || m_stop.load()) {
                m_thumb_job_running = false;
                return;
            }
            bucket = m_thumb_queue.back();   // newest request first (the visible part of a scrolling timeline)
            m_thumb_queue.pop_back();
        }
        const TimeUs end = m_media_end.load();
        const TimeUs t = std::clamp<TimeUs>(bucket * 1'000'000, 0, std::max<TimeUs>(end - m_frame_dur, 0));
        bool ok = false;
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        const bool reachable = !m_thumb_dec->AtEnd() && m_thumb_dec_next <= t && t - m_thumb_dec_next <= kForwardReachUs;
        if (reachable || SUCCEEDED(m_thumb_dec->Seek(t))) {
            media::DecodedFrame f;
            if (m_thumb_dec->ReadFrame(f, t) == S_OK) {
                m_thumb_dec_next = f.pts_us + f.duration_us;
                D3D11_TEXTURE2D_DESC d{};
                d.Width = kThumbW;
                d.Height = kThumbH;
                d.MipLevels = 1;
                d.ArraySize = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11RenderTargetView> rtv;
                if (SUCCEEDED(m_dev->Device()->CreateTexture2D(&d, nullptr, tex.GetAddressOf())) &&
                    SUCCEEDED(m_dev->Device()->CreateRenderTargetView(tex.Get(), nullptr, rtv.GetAddressOf())) &&
                    SUCCEEDED(m_dev->Device()->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf()))) {
                    // Letterbox into 160x90.
                    const float sx = static_cast<float>(kThumbW) / f.size.w, sy = static_cast<float>(kThumbH) / f.size.h;
                    const float s = std::min(sx, sy);
                    const float w = f.size.w * s, h = f.size.h * s;
                    D3D11_VIEWPORT vp{(kThumbW - w) * 0.5f, (kThumbH - h) * 0.5f, w, h, 0, 1};
                    const float black[4] = {0, 0, 0, 1};
                    platform::D3DDevice::Lock lock(*m_dev);
                    m_dev->Context()->ClearRenderTargetView(rtv.Get(), black);
                    ok = SUCCEEDED(m_thumb_blit->Draw(m_dev->Context(), m_thumb_dec->YSrv(f.pool_index), m_thumb_dec->UvSrv(f.pool_index),
                                                      f.size, rtv.Get(), vp, true));
                    ID3D11RenderTargetView* none = nullptr;
                    m_dev->Context()->OMSetRenderTargets(1, &none, nullptr);
                }
                m_thumb_dec->ReleaseFrame(f.pool_index);
            }
        }
        ExclusiveGuard g(m_aux_lock);
        auto it = m_thumbs.find(bucket);
        if (it == m_thumbs.end()) continue;
        if (ok) {
            it->second.tex = tex;
            it->second.srv = srv;
            it->second.ready = true;
        } else {
            m_thumbs.erase(it);   // a later request retries
        }
    }
}

const media::Peaks* MediaCache::GetPeaks(int stream_index) {
    if (!m_open) return nullptr;
    if (stream_index < 0) {
        const std::vector<int> a = m_info.AudioStreams();
        if (a.empty()) return nullptr;
        stream_index = a[0];
    }
    ExclusiveGuard g(m_aux_lock);
    auto it = m_peaks.find(stream_index);
    if (it != m_peaks.end()) return it->second.get();
    if (!m_peaks_requested[stream_index]) {
        m_peaks_requested[stream_index] = true;
        BeginJob();
        Async::Run([this, stream_index] {
            PeaksJob(stream_index);
            EndJob();
        });
    }
    return nullptr;
}

void MediaCache::PeaksJob(int stream) {
    if (m_stop.load()) return;
    auto p = std::make_unique<media::Peaks>();
    if (!media::Peaks::LoadCache(m_path, stream, *p)) {
        const HRESULT hr = media::Peaks::Build(m_path, stream, *p, &m_stop);   // Close cancels a long build
        if (hr == E_ABORT) return;
        if (FAILED(hr)) {
            CT_LOG_WARN("MediaCache: peaks for %s stream %d: %s", m_path.c_str(), stream, HrToString(hr).c_str());
            return;
        }
        media::Peaks::SaveCache(m_path, stream, *p);
    }
    ExclusiveGuard g(m_aux_lock);
    m_peaks[stream] = std::move(p);
}

} // namespace ct::playback
