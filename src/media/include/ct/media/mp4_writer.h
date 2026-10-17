#pragma once
// ct/media/mp4_writer.h
// MP4 writer: H.264/HEVC video from NV12 GPU textures (hardware encoder through the DXGI device manager, zero CPU
// copies; software fallback) plus up to four AAC streams from 48 kHz float audio, via IMFSinkWriter.
// Stream order in the file: video = 0 (when has_video), then audio in Config::audio order (mic first, system second).
//
// Thread model:
//  - Init/Start/Finalize/Abort from one owner thread (a worker, never the UI thread: they block on MF).
//  - OnFrame from the one video producer (ct-capture, with D3DDevice::Lock held): only pushes to a queue.
//  - OnAudio(stream) from one producer per stream (ct-audio-mic / ct-audio-sys): converts to PCM16 and pushes.
//  - WriteVideoSync / WriteAudioSync (export, no pool) from one caller thread; they never mix with OnFrame/OnAudio.
//  - Every IMFSinkWriter call happens on the "ct-encode" thread (ABOVE_NORMAL, MMCSS "Playback") between Start and
//    Finalize; it writes queued items in timestamp order (ChooseNextStream) and finally calls IMFSinkWriter::Finalize.
//  - GetStats/LastError/EncoderName/IsHardware/ErrorMessage from any thread.
// Texture ownership: with an NV12 pool (capture), every OnFrame that returns true is one pool hand-out that the
// writer releases on ct-encode right after WriteSample; the pool's reuse predicate keeps the texture out of
// circulation until Media Foundation dropped every sample that references it (repeated frames send the same texture
// in several samples; it becomes reusable only after all of them were encoded). Without a pool (export), the sync
// calls copy the caller's texture into an internal pool, so the caller may overwrite its texture once they return.
// Lifetime: Init -> Start -> producers run -> producers stop -> Finalize (or Abort on the crash path). The NV12 pool
// must outlive the writer (the destructor detaches the predicate). A writer is used for one file.

#include <ct/audio/audio_sink.h>
#include <ct/capture/frame_sink.h>
#include <ct/core/stats.h>
#include <ct/core/types.h>
#include <ct/media/encode_config.h>
#include <ct/platform/d3d_device.h>
#include <ct/platform/texture_pool.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ct::media {

class Mp4Writer final : public capture::FrameSink, public audio::AudioSink {
public:
    static constexpr uint32_t kMaxAudioStreams = 4;
    static constexpr uint32_t kAudioItemFrames = 4800;   // 100 ms per AAC input sample

    // Notified once on ct-encode when writing failed (disk full, device removed, encoder error). The writer has then
    // stopped accepting input and already tried to finalize the file with what it had.
    struct Listener {
        virtual void OnWriterError(HRESULT hr) = 0;
        virtual ~Listener() = default;
    };

    struct Config {
        std::string path;                       // UTF-8; the folder must exist
        bool has_video = true;
        VideoEncodeConfig video;
        std::vector<AudioEncodeConfig> audio;   // 0..4 streams
        bool fast_start = false;                // moov before mdat: NOT supported yet (ignored with a warning, see DECISIONS)
        uint32_t video_queue = 8;               // 2..64 frames
        uint32_t audio_queue = 64;              // 2..64 items of up to 100 ms per stream
        Listener* listener = nullptr;
        uint32_t sync_pool_size = 8;            // internal NV12 pool for WriteVideoSync (no external pool)
    };

    struct Stats {
        std::atomic<uint64_t> video_written{0}, video_rejected{0}, audio_written[kMaxAudioStreams]{}, queue_depth{0};
        std::atomic<int64_t> last_video_us{0}, last_audio_us[kMaxAudioStreams]{};
        // Additions (P04): audio frames rejected (queue/pool full or not accepting), silence padded at the start of each
        // audio stream, WriteSample cost and OnFrame -> WriteSample latency, IMFSinkWriter::Finalize duration.
        std::atomic<uint64_t> audio_rejected_frames[kMaxAudioStreams]{}, audio_lead_pad_frames[kMaxAudioStreams]{};
        ct::Stats::Timing video_write;     // WriteSample call duration (video)
        ct::Stats::Timing video_latency;   // OnFrame -> WriteSample start
        std::atomic<int64_t> finalize_ms{-1};
        std::atomic<uint32_t> max_textures_in_mf{0};   // peak number of pool textures still referenced by MF samples
    };

    Mp4Writer();
    ~Mp4Writer() override;
    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    // Creates the sink writer and its streams (docs/phases/P04 section 4.2). pool != null: the writer creates one
    // IMFMediaBuffer per pool texture, installs the reuse predicate and releases textures when written; call before
    // the capturer starts. pool == null with has_video: an internal pool for WriteVideoSync. Needs COM on this thread.
    HRESULT Init(platform::D3DDevice& dev, const Config& cfg, platform::TexturePool* nv12_pool);
    bool Start();   // spawns ct-encode

    // FrameSink (ct-capture): queue {pool_index, t_us}; false if the queue is full or the writer stopped accepting.
    bool OnFrame(ID3D11Texture2D* nv12, uint32_t pool_index, TimeUs t_us) override;
    // Export path (no pool): copies `nv12` (same size, NV12) into an internal texture under D3DDevice::Lock and queues
    // it, waiting while the queue or the internal pool is full (up to 5 s). The caller may reuse its texture on return.
    HRESULT WriteVideoSync(ID3D11Texture2D* nv12, TimeUs t_us);

    // AudioSink (one producer thread per stream): float -> PCM16 into pooled 100 ms buffers; consecutive packets are
    // batched. false if the stream's pool/queue is full or the writer stopped accepting (the producer counts a drop).
    bool OnAudio(uint32_t stream, const audio::AudioPacket&) override;
    // Export path: as OnAudio for `frames` interleaved frames at t_us, but waits while the queue is full (up to 5 s).
    HRESULT WriteAudioSync(uint32_t stream, const float* interleaved, uint32_t frames, TimeUs t_us);

    // Stop accepting, flush partial audio buffers, drain the queues, IMFSinkWriter::Finalize, join ct-encode.
    // Blocks (typically < 1 s). Stop the producers first; calls that race it are rejected (counted as drops).
    // Returns the first error (also LastError()). Calling it again returns the same result.
    HRESULT Finalize();
    // Crash path: stop accepting, drop queued input, let ct-encode try IMFSinkWriter::Finalize; returns after at most
    // 3 s whether or not that finished. Does not join.
    void Abort();

    HRESULT LastError() const { return m_last_error.load(std::memory_order_acquire); }
    std::string ErrorMessage() const;    // user-presentable text for LastError() (disk full, missing folder, ...)
    std::string EncoderName() const;     // the video encoder the sink writer loaded, e.g. "AMDh264Encoder"
    bool IsHardware() const { return m_hardware; }
    const Stats& GetStats() const { return m_stats; }
    bool Accepting() const { return m_accepting.load(std::memory_order_acquire); }
    uint32_t VideoStreamIndex() const;   // 0, or UINT32_MAX without video
    uint32_t AudioStreamIndex(uint32_t stream) const;

    // Tests only: the n-th WriteSample (counting from 0, any stream) fails with `hr` as if the sink had returned it.
    void InjectWriteErrorForTest(uint64_t after_samples, HRESULT hr);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    Stats m_stats;
    std::atomic<HRESULT> m_last_error{0};
    std::atomic<bool> m_accepting{false};
    bool m_hardware = false;
};

} // namespace ct::media
