#pragma once
// ct/playback/player.h
// Single-file player: MediaCache (video frames) + an audio feed thread ("ct-audio-feed", decodes the first audio
// stream into a 1 s ring) + AudioEngine (output, master clock) + PlaybackClock. Used by player_demo and as the reference
// integration for P09.
// Thread model: everything on one control thread (the UI thread), which calls Tick() once per displayed frame. Tick is
// non-blocking. Lifetime: Open -> ... -> Close (or the destructor).

#include <ct/audio/devices.h>
#include <ct/core/ring_buffer.h>
#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/types.h>
#include <ct/playback/audio_engine.h>
#include <ct/playback/media_cache.h>
#include <ct/playback/playback_clock.h>

#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

namespace ct::playback {

class Player {
public:
    Player();
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Audio is optional: without a render endpoint (or audio stream) the clock runs on QPC.
    HRESULT Open(platform::D3DDevice&, audio::AudioDevices&, std::string_view path);
    void Close();

    void Play();
    void Pause();
    void TogglePlay();
    void Seek(TimeUs);
    void StepFrames(int delta);   // pauses first
    void SetRate(double);         // 0.25..4, negative = reverse (silent)

    // Picks the frame with pts <= now < pts+dur; returns true if a frame is available (possibly an older one while the
    // exact frame decodes). Stops at the end of the media.
    bool Tick(FrameRef& frame_to_show, TimeUs& now_us);

    const media::MediaInfo& Info() const { return m_cache.Info(); }
    struct Stats {
        double seek_to_frame_ms = 0;   // last Seek()/StepFrames() -> first exact frame from Tick
        uint64_t frames_shown = 0, frames_late = 0;
        size_t cache_bytes = 0;
    };
    Stats GetStats() const;

    // Additions.
    bool Playing() const { return m_clock.Playing(); }
    double Rate() const { return m_clock.Rate(); }
    TimeUs Duration() const { return m_duration_us; }
    bool HasAudio() const { return m_engine_ok; }
    bool ClockUsesAudio() const { return m_clock.UsingAudio(); }
    MediaCache& Cache() { return m_cache; }
    AudioEngine& Engine() { return m_engine; }
    void ResetStats();

private:
    // AudioEngine::Source over the ring; the feed thread decodes into it.
    struct Feed final : AudioEngine::Source {
        uint32_t Pull(float* interleaved, uint32_t frames, TimeUs from_us, double rate) override;
        SrwLock lock;                    // Pull try-locks; the feed thread locks to clear the ring
        AudioRing ring;
        std::vector<float> discard;      // Pull: frames skipped to catch up
        TimeUs next_us = 0;              // media time of the ring's next frame (consumer side)
        std::atomic<uint64_t> req_gen{0};
        std::atomic<int64_t> req_from{0};
        uint64_t data_gen = 0;           // generation the ring content belongs to (under lock)
    };
    void FeedMain();
    void RestartFeed(TimeUs from_us);
    TimeUs ClampTime(TimeUs t) const;

    platform::D3DDevice* m_dev = nullptr;
    MediaCache m_cache;
    AudioEngine m_engine;
    bool m_engine_ok = false;
    PlaybackClock m_clock;
    Feed m_feed;
    std::unique_ptr<MediaCache::AudioReader> m_reader;   // feed thread only
    Thread m_feed_thread;
    Event m_feed_wake{false, false};
    std::atomic<bool> m_feed_stop{false};
    TimeUs m_duration_us = 0;
    int m_fps_num = 30, m_fps_den = 1;

    Stats m_stats;
    TimeUs m_last_pts = -1;
    int64_t m_seek_qpc = 0;   // pending seek measurement
};

} // namespace ct::playback
