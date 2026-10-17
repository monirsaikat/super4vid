#pragma once
// ct/media/audio_decoder.h
// Decodes one audio stream to interleaved float, 48 kHz stereo, with sample-accurate seeks. The Source Reader converts
// to float 48 kHz stereo when it can; otherwise the decoder asks for float at the source rate/channel count and converts
// with audio::Resampler + dsp::MonoToStereo/DownmixToStereo.
// Thread model: one thread at a time (the owner serializes every call). Blocking MF calls: never on the UI or audio
// render thread. Lifetime: Init -> ... -> Shutdown (or the destructor).

#include <ct/audio/resampler.h>
#include <ct/core/types.h>

#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

struct IMFSourceReader;
typedef long HRESULT;   // same type as <winnt.h>

namespace ct::media {

class AudioDecoder {
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;

    struct Config {
        std::string path;
        int stream_index = -1;          // -1 = first audio stream
        bool force_resampler = false;   // tests: skip the reader's 48 kHz stereo conversion and use our Resampler
    };

    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    HRESULT Init(const Config&);
    void Shutdown();
    // Sample-accurate: the next Read starts at t (frames before t in the first decoded packet are skipped).
    HRESULT Seek(TimeUs t_us);
    // Fills up to capacity_frames (decoding as needed). pts_us = time of the first frame written. Frames are contiguous
    // between seeks (pts of the next call = pts + frames * 1e6 / 48000). S_OK with frames_out > 0; S_FALSE with
    // frames_out == 0 at end of stream.
    HRESULT Read(float* interleaved_out, uint32_t capacity_frames, uint32_t& frames_out, TimeUs& pts_us);

    TimeUs Duration() const { return m_duration_us; }
    int SourceRate() const { return m_src_rate; }
    int SourceChannels() const { return m_src_channels; }
    bool UsingResampler() const { return m_convert; }

private:
    HRESULT DecodeMore();   // reads one MF sample into m_pending (or sets m_eos)
    void AppendConverted(const float* in, uint32_t frames);

    Microsoft::WRL::ComPtr<IMFSourceReader> m_reader;
    unsigned long m_stream = 0;
    audio::Resampler m_resampler;
    std::vector<float> m_pending;     // 48 kHz stereo frames not yet handed out
    size_t m_pending_pos = 0;         // frames consumed from m_pending
    std::vector<float> m_stereo;      // conversion scratch (source rate, stereo)
    std::vector<float> m_resampled;   // conversion scratch (48 kHz)
    TimeUs m_next_pts_us = 0;         // time of m_pending[m_pending_pos]
    int64_t m_frames_since_base = 0;  // frames handed out since m_base_pts_us
    TimeUs m_base_pts_us = 0;
    TimeUs m_seek_target = 0;
    bool m_need_base = true;          // next decoded packet defines the timeline (after Init / Seek)
    bool m_eos = false;
    bool m_convert = false;           // our resampler/channel conversion is active
    int m_src_rate = 0, m_src_channels = 0;
    TimeUs m_duration_us = 0;
};

} // namespace ct::media
