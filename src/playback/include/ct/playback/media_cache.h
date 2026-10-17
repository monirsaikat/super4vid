#pragma once
// ct/playback/media_cache.h
// One per open media file. Owns the hardware VideoDecoder and its prefetch thread ("ct-decode-N", above normal), an LRU
// cache of decoded NV12 frames (copies, so the decoder keeps running) bounded by the global CacheBudget, audio readers,
// 160x90 thumbnails (a second decoder on the IO pool) and waveform peaks (IO pool, disk cache). Images (PNG/JPG/...)
// are one RGBA texture shown at every time.
// Thread model: Open/Close on the owner (UI) thread; GetVideoFrame/Hint/Thumbnail/GetPeaks/GetStats are non-blocking
// and callable from any one thread at a time (normally the UI thread); OpenAudio from any thread (blocking: opens a
// decoder). FrameRef / thumbnail views stay valid until 4 other frames were returned by later GetVideoFrame calls (the
// 4 most recently returned frames are pinned, LRU) or Close. Draw them with D3DDevice::Lock held, as every immediate-context user must.
// Lifetime: Open -> ... -> Close (or the destructor), which joins the prefetch thread and waits for IO-pool jobs.

#include <ct/core/sync.h>
#include <ct/core/thread.h>
#include <ct/core/types.h>
#include <ct/media/audio_decoder.h>
#include <ct/media/media_info.h>
#include <ct/media/peaks.h>
#include <ct/media/video_decoder.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/nv12_blit.h>
#include <ct/playback/frame_cache.h>

#include <wrl/client.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ct::playback {

struct FrameRef {
    ID3D11ShaderResourceView* y = nullptr;     // NV12 luma plane (R8)
    ID3D11ShaderResourceView* uv = nullptr;    // NV12 chroma plane (R8G8)
    ID3D11Texture2D* nv12 = nullptr;
    TimeUs pts_us = 0, duration_us = 0;
    SizeI size;
    ID3D11ShaderResourceView* rgba = nullptr;  // addition: images are BGRA (y/uv/nv12 are null then)
};

class MediaCache {
public:
    MediaCache();
    ~MediaCache();
    MediaCache(const MediaCache&) = delete;
    MediaCache& operator=(const MediaCache&) = delete;

    HRESULT Open(platform::D3DDevice&, std::string_view path);
    const media::MediaInfo& Info() const { return m_info; }

    // Non-blocking. exact=false means a nearest earlier frame was returned (or none) and the exact one is being
    // prefetched; the UI re-requests next frame. A miss outside the last hint's window re-hints around t.
    bool GetVideoFrame(TimeUs t_us, FrameRef& out, bool& exact);
    // Prefetch window: playing -> [t-0.5 s, t+2 s] in rate direction; paused -> [t-0.25 s, t+0.5 s].
    void Hint(TimeUs playhead_us, double rate, bool playing);

    class AudioReader {   // sequential, one per consumer (clip); blocking decode: call from a worker thread
    public:
        HRESULT Seek(TimeUs);
        uint32_t Read(float* out, uint32_t frames, TimeUs& pts);   // 48 kHz stereo; 0 at the end (or on error)
        TimeUs Duration() const { return m_dec.Duration(); }

    private:
        friend class MediaCache;
        media::AudioDecoder m_dec;
    };
    std::unique_ptr<AudioReader> OpenAudio(int stream_index);   // -1 = first audio stream; nullptr if none

    // 160x90 RGBA; false = not ready yet (requested on the IO pool), cached up to 2,000 per media, 1 s buckets.
    bool Thumbnail(TimeUs t_us, ID3D11ShaderResourceView*& srv);
    // nullptr until built (IO pool, cached on disk under %LOCALAPPDATA%\Camtesia\cache\).
    const media::Peaks* GetPeaks(int stream_index);
    void Close();

    // Additions for tools/tests/P09.
    struct Stats {
        uint64_t frames_decoded = 0;     // copied into the cache
        uint64_t seeks = 0;              // decoder seeks issued by the prefetch thread
        uint64_t evictions = 0;
        uint64_t hits = 0, misses = 0;   // GetVideoFrame exact / not exact
        size_t cache_bytes = 0;
        size_t cached_frames = 0;
        double last_seek_to_frame_ms = 0;   // far hint -> exact playhead frame cached
    };
    Stats GetStats() const;
    TimeUs FrameDurationUs() const;   // nominal (video), 0 for images/audio-only
    int FpsNum() const { return m_fps_num; }
    int FpsDen() const { return m_fps_den; }
    TimeUs DurationUs() const { return m_info.duration_us; }
    bool HasVideo() const { return m_has_video; }
    // Blocks until the frame shown at t is cached (tools/tests; never on the UI thread). Hints paused at t first.
    bool WaitForFrame(TimeUs t_us, uint32_t timeout_ms);
    // The pinned set is a true LRU of the last kPinned returned slots (index 0 = least recent): returning `slot`
    // moves it to the most recent position. Pure; public for tests.
    static constexpr int kPinned = 4;
    static void PinRecent(uint32_t (&pins)[kPinned], uint32_t slot);

private:
    struct Window { TimeUs start = 0, end = 0, playhead = 0; bool forward = true; };
    struct HintState { TimeUs t = 0; double rate = 1.0; bool playing = false; uint64_t gen = 0; };
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y, uv;
        SizeI size{};
    };
    struct Thumb {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        bool ready = false;
        uint64_t last_used = 0;
    };

    void PrefetchMain();
    Window ComputeWindow(const HintState&) const;
    HintState ReadHint() const;
    TimeUs NextNeed(const Window&) const;            // first uncached time the window wants, or -1
    bool CachedExact(TimeUs t) const;
    bool StoreFrame(const media::DecodedFrame&, const Window&);   // false: budget full of frames that must stay
    int AcquireSlot(SizeI size);
    void ThumbJob();
    void PeaksJob(int stream);
    void BeginJob();
    void EndJob();

    platform::D3DDevice* m_dev = nullptr;
    std::string m_path;
    media::MediaInfo m_info;
    bool m_open = false;
    bool m_has_video = false;
    int m_fps_num = 30, m_fps_den = 1;
    TimeUs m_frame_dur = 33'333;

    // Image media.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_image;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_image_srv;
    SizeI m_image_size{};

    // Video: decoder and thread state (prefetch thread only, after Open).
    media::VideoDecoder m_dec;
    TimeUs m_dec_next = 0;                 // pts the decoder delivers next (after the last frame read)
    std::atomic<TimeUs> m_media_end{0};    // shrinks to the real end when the decoder reports EOS
    Thread m_thread;
    Event m_wake{false, false};
    Event m_inserted{false, false};   // set after every cached frame (WaitForFrame)
    std::atomic<bool> m_stop{false};

    mutable SrwLock m_hint_lock;
    HintState m_hint;
    // Bumped by Hint() only for jumps (|dt| > 1 s, play/pause or direction change): aborts a long fast-forward. Small
    // per-tick playhead moves must not, or playback would re-seek every frame.
    std::atomic<uint64_t> m_jump_gen{0};

    mutable SrwLock m_lock;   // m_cache, m_slots[*] contents, m_free, m_pinned, m_tick, m_stats
    FrameCache m_cache;
    std::vector<Slot> m_slots;          // fixed capacity after Open (no reallocation while the UI reads)
    std::vector<uint32_t> m_free;       // free slot indices whose texture can be reused
    uint32_t m_slots_used = 0;          // slots created so far
    uint32_t m_pinned[kPinned] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};   // LRU order (PinRecent)
    uint64_t m_tick = 0;
    Stats m_stats;
    int64_t m_seek_started_qpc = 0;     // prefetch thread: QPC of the last far seek, 0 when none is pending
    TimeUs m_seek_target = 0;

    // Thumbnails / peaks (IO pool).
    mutable SrwLock m_aux_lock;
    std::map<int64_t, Thumb> m_thumbs;  // bucket (seconds) -> thumbnail
    std::vector<int64_t> m_thumb_queue;
    bool m_thumb_job_running = false;
    std::unique_ptr<media::VideoDecoder> m_thumb_dec;   // IO job only
    TimeUs m_thumb_dec_next = -1;
    std::unique_ptr<platform::Nv12Blit> m_thumb_blit;
    std::map<int, std::unique_ptr<media::Peaks>> m_peaks;
    std::map<int, bool> m_peaks_requested;
    std::atomic<int> m_jobs{0};
    Event m_jobs_idle{true, true};
    uint64_t m_aux_tick = 0;
};

} // namespace ct::playback
