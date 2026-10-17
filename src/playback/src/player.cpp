#define CT_LOG_CATEGORY "playback"
#include <ct/playback/player.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/time.h>
#include <ct/platform/win.h>

#include <algorithm>
#include <cstring>

namespace ct::playback {

namespace {
constexpr size_t kRingFrames = 48000;      // 1 s
constexpr uint32_t kFeedChunk = 4800;      // 100 ms per decode step
constexpr TimeUs kLateUs = 50'000;
constexpr int64_t kFrameUs = 1'000'000 / AudioEngine::kRate;
} // namespace

Player::Player() = default;
Player::~Player() { Close(); }

HRESULT Player::Open(platform::D3DDevice& dev, audio::AudioDevices& devices, std::string_view path) {
    Close();
    m_dev = &dev;
    CT_RETURN_IF_FAILED(m_cache.Open(dev, path));
    const media::MediaInfo& info = m_cache.Info();
    m_duration_us = info.duration_us;
    if (m_cache.HasVideo()) {
        m_fps_num = m_cache.FpsNum();
        m_fps_den = m_cache.FpsDen();
    }
    m_engine_ok = false;
    if (!info.AudioStreams().empty()) {
        m_reader = m_cache.OpenAudio(-1);
        if (m_reader && SUCCEEDED(m_engine.Init(devices))) {
            m_feed.ring.Init(kRingFrames, AudioEngine::kChannels);
            m_feed.discard.assign(static_cast<size_t>(kFeedChunk) * AudioEngine::kChannels, 0.0f);
            m_feed_stop = false;
            Thread::Options opt;
            opt.name = "ct-audio-feed";
            opt.priority = 1;   // above normal
            m_engine_ok = m_feed_thread.Start(opt, [this] { FeedMain(); }) && m_engine.Start(m_feed);
        }
        if (!m_engine_ok) CT_LOG_WARN("Player: no audio output; the clock runs on QPC");
    }
    m_clock = PlaybackClock{};
    m_clock.Attach(m_engine_ok ? &m_engine : nullptr);
    m_clock.Seek(0);
    RestartFeed(0);
    ResetStats();
    return S_OK;
}

void Player::Close() {
    m_engine.Stop();
    m_feed_stop = true;
    m_feed_wake.Set();
    m_feed_thread.Join();
    m_reader.reset();
    m_cache.Close();
    m_engine_ok = false;
    m_clock = PlaybackClock{};
}

TimeUs Player::ClampTime(TimeUs t) const {
    const TimeUs last = m_cache.HasVideo() ? std::max<TimeUs>(m_duration_us - m_cache.FrameDurationUs() / 2, 0) : m_duration_us;
    return std::clamp<TimeUs>(t, 0, std::max<TimeUs>(last, 0));
}

void Player::RestartFeed(TimeUs from_us) {
    if (!m_engine_ok) return;
    m_feed.req_from.store(from_us, std::memory_order_relaxed);
    m_feed.req_gen.fetch_add(1, std::memory_order_release);
    m_feed_wake.Set();
}

void Player::Play() {
    if (m_clock.Playing()) return;
    TimeUs from = m_clock.Now();
    if (from >= ClampTime(m_duration_us)) from = 0;   // at the end: start over
    RestartFeed(from);
    m_clock.Play(from, m_clock.Rate());
}

void Player::Pause() { m_clock.Pause(); }

void Player::TogglePlay() {
    if (m_clock.Playing()) Pause();
    else Play();
}

void Player::Seek(TimeUs t) {
    t = ClampTime(t);
    if (m_clock.Playing()) RestartFeed(t);
    m_clock.Seek(t);
    m_seek_qpc = Clock::QpcNow();
    m_cache.Hint(t, m_clock.Rate(), m_clock.Playing());
}

void Player::StepFrames(int delta) {
    Pause();
    const int64_t idx = FrameIndexAt(m_clock.Now(), m_fps_num, m_fps_den);
    Seek(FrameTimeUs(std::max<int64_t>(idx + delta, 0), m_fps_num, m_fps_den));
}

void Player::SetRate(double rate) {
    const bool playing = m_clock.Playing();
    if (playing) RestartFeed(m_clock.Now());
    m_clock.SetRate(rate);
}

bool Player::Tick(FrameRef& frame, TimeUs& now_us) {
    TimeUs now = m_clock.Now();
    const TimeUs end = ClampTime(m_duration_us);
    if (m_clock.Playing() && ((m_clock.Rate() > 0 && now >= end) || (m_clock.Rate() < 0 && now <= 0))) {
        m_clock.Pause();
        m_clock.Seek(m_clock.Rate() > 0 ? end : 0);
        now = m_clock.Now();
    }
    now = std::clamp<TimeUs>(now, 0, std::max<TimeUs>(end, 0));
    now_us = now;
    m_cache.Hint(now, m_clock.Rate(), m_clock.Playing());
    bool exact = false;
    const bool got = m_cache.GetVideoFrame(now, frame, exact);
    if (got && frame.pts_us != m_last_pts) {
        ++m_stats.frames_shown;
        if (m_clock.Playing() && frame.pts_us + frame.duration_us < now - kLateUs) ++m_stats.frames_late;
        m_last_pts = frame.pts_us;
    }
    if (m_seek_qpc != 0 && got && exact) {
        m_stats.seek_to_frame_ms = static_cast<double>(Clock::QpcNow() - m_seek_qpc) * 1000.0 / Clock::QpcFrequency();
        m_seek_qpc = 0;
    }
    return got;
}

Player::Stats Player::GetStats() const {
    Stats s = m_stats;
    s.cache_bytes = m_cache.GetStats().cache_bytes;
    return s;
}

void Player::ResetStats() {
    m_stats = Stats{};
    m_last_pts = -1;
}

// --- audio feed ----------------------------------------------------------------------------------------------------

uint32_t Player::Feed::Pull(float* out, uint32_t frames, TimeUs from_us, double) {
    const size_t ch = AudioEngine::kChannels;
    std::memset(out, 0, sizeof(float) * frames * ch);
    if (!lock.TryLockExclusive()) return 0;   // the feed thread is clearing: one buffer of silence
    if (data_gen != req_gen.load(std::memory_order_acquire)) {
        lock.UnlockExclusive();
        return 0;
    }
    // Catch up when the ring lags the clock (refill after a seek, a slow decoder): drop the stale frames.
    while (from_us > next_us + kFrameUs) {
        const size_t behind = static_cast<size_t>((from_us - next_us) * AudioEngine::kRate / 1'000'000);
        const size_t n = ring.Read(discard.data(), std::min<size_t>(behind, discard.size() / ch));
        if (n == 0) break;
        next_us += static_cast<TimeUs>(n) * 1'000'000 / AudioEngine::kRate;
    }
    uint32_t written = 0;
    if (from_us > next_us + kFrameUs) {
        // Still behind with an empty ring: silence; the ring restarts at the clock when data arrives.
        lock.UnlockExclusive();
        return 0;
    }
    if (from_us + kFrameUs < next_us) {
        // The ring is ahead (gap in the stream): silence up to it.
        const uint32_t gap = static_cast<uint32_t>(std::min<int64_t>((next_us - from_us) * AudioEngine::kRate / 1'000'000, frames));
        written = gap;
    }
    const size_t n = ring.Read(out + static_cast<size_t>(written) * ch, frames - written);
    next_us += static_cast<TimeUs>(n) * 1'000'000 / AudioEngine::kRate;
    lock.UnlockExclusive();
    return written + static_cast<uint32_t>(n);
}

void Player::Feed::Reset(TimeUs from_us, uint64_t gen) {
    ExclusiveGuard g(lock);
    ring.Clear();
    next_us = from_us;
    data_gen = gen;
    awaiting_first = true;
}

void Player::Feed::Append(const float* interleaved, uint32_t frames, TimeUs pts_us) {
    if (!awaiting_first) {
        ring.Write(interleaved, frames);
        return;
    }
    // The ring is empty and Pull has not moved next_us (it only advances over frames it read): anchor the ring at the
    // first packet's real time. Earlier than the request -> Pull drops the stale frames; later -> silence until then.
    ExclusiveGuard g(lock);
    next_us = pts_us;
    ring.Write(interleaved, frames);
    awaiting_first = false;
}

void Player::FeedMain() {
    std::vector<float> buf(static_cast<size_t>(kFeedChunk) * AudioEngine::kChannels);
    uint64_t gen = 0;
    bool eos = true;
    while (!m_feed_stop.load()) {
        const uint64_t want = m_feed.req_gen.load(std::memory_order_acquire);
        if (want != gen) {
            gen = want;
            const TimeUs from = m_feed.req_from.load(std::memory_order_relaxed);
            m_feed.Reset(from, gen);
            eos = FAILED(m_reader->Seek(from));
        }
        bool wrote = false;
        while (!eos && m_feed.ring.Free() >= kFeedChunk && m_feed.req_gen.load(std::memory_order_acquire) == gen) {
            TimeUs pts = 0;
            const uint32_t n = m_reader->Read(buf.data(), kFeedChunk, pts);
            if (n == 0) {
                eos = true;
                break;
            }
            m_feed.Append(buf.data(), n, pts);
            wrote = true;
        }
        if (m_feed.req_gen.load(std::memory_order_acquire) != gen) continue;
        // Ring full (or end): wait until the render thread drained ~100 ms, or for a new request.
        m_feed_wake.Wait(wrote ? 20 : 50);
    }
}

} // namespace ct::playback
