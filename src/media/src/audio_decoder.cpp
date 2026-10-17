#define CT_LOG_CATEGORY "media"
#include <ct/media/audio_decoder.h>

#include "mf_util_internal.h"

#include <ct/audio/dsp.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>

#include <mferror.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::media {

AudioDecoder::AudioDecoder() = default;
AudioDecoder::~AudioDecoder() { Shutdown(); }

void AudioDecoder::Shutdown() {
    m_reader.Reset();
    m_pending.clear();
    m_pending_pos = 0;
    m_eos = false;
    m_need_base = true;
    m_convert = false;
}

HRESULT AudioDecoder::Init(const Config& cfg) {
    Shutdown();
    audio::dsp::Init();
    CT_RETURN_IF_FAILED(platform::MfRuntime::EnsureForProcess());
    const std::wstring wpath = Utf8ToWide(cfg.path);
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, m_reader.GetAddressOf());
    if (FAILED(hr)) {
        CT_LOG_WARN("AudioDecoder: cannot open %s: %s", cfg.path.c_str(), HrToString(hr).c_str());
        return hr;
    }
    m_stream = cfg.stream_index >= 0 ? static_cast<DWORD>(cfg.stream_index) : detail::kFirstAudio;
    CT_RETURN_IF_FAILED(m_reader->SetStreamSelection(detail::kAllStreams, FALSE));
    hr = m_reader->SetStreamSelection(m_stream, TRUE);
    if (FAILED(hr)) {
        CT_LOG_WARN("AudioDecoder: %s has no audio stream %d: %s", cfg.path.c_str(), cfg.stream_index, HrToString(hr).c_str());
        return hr;
    }
    ComPtr<IMFMediaType> native;
    CT_RETURN_IF_FAILED(m_reader->GetNativeMediaType(m_stream, 0, native.GetAddressOf()));
    m_src_rate = static_cast<int>(MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
    m_src_channels = static_cast<int>(MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));

    hr = E_FAIL;
    if (!cfg.force_resampler) {
        ComPtr<IMFMediaType> full;
        CT_RETURN_IF_FAILED(MFCreateMediaType(full.GetAddressOf()));
        full->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        full->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        full->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        full->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
        full->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        full->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
        full->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 8);
        hr = m_reader->SetCurrentMediaType(m_stream, nullptr, full.Get());
    }
    if (SUCCEEDED(hr)) {
        // The reader may accept the type but still report something else; check what it really delivers.
        ComPtr<IMFMediaType> cur;
        CT_RETURN_IF_FAILED(m_reader->GetCurrentMediaType(m_stream, cur.GetAddressOf()));
        const int rate = static_cast<int>(MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
        const int ch = static_cast<int>(MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        if (rate != kRate || ch != kChannels) hr = E_FAIL;
    }
    int out_rate = kRate, out_channels = kChannels;
    if (FAILED(hr)) {
        ComPtr<IMFMediaType> partial;
        CT_RETURN_IF_FAILED(MFCreateMediaType(partial.GetAddressOf()));
        partial->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        partial->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        hr = m_reader->SetCurrentMediaType(m_stream, nullptr, partial.Get());
        if (FAILED(hr)) {
            CT_LOG_WARN("AudioDecoder: float output rejected for %s: %s", cfg.path.c_str(), HrToString(hr).c_str());
            return hr;
        }
        ComPtr<IMFMediaType> cur;
        CT_RETURN_IF_FAILED(m_reader->GetCurrentMediaType(m_stream, cur.GetAddressOf()));
        out_rate = static_cast<int>(MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
        out_channels = static_cast<int>(MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        if (out_rate <= 0 || out_channels <= 0) return MF_E_INVALIDMEDIATYPE;
        m_convert = true;
        if (out_rate != kRate && !m_resampler.Init(out_rate, kRate, kChannels)) {
            CT_LOG_WARN("AudioDecoder: cannot resample %d Hz", out_rate);
            return MF_E_UNSUPPORTED_RATE;
        }
    }
    m_src_rate = m_convert ? out_rate : m_src_rate;
    m_src_channels = m_convert ? out_channels : m_src_channels;
    m_duration_us = detail::ReaderDurationUs(m_reader.Get());
    m_need_base = true;
    m_seek_target = 0;
    CT_LOG_INFO("AudioDecoder: %s source %d Hz x %d, %s, duration %.3f s", cfg.path.c_str(), m_src_rate, m_src_channels,
                m_convert ? "own conversion" : "reader conversion", m_duration_us / 1e6);
    return S_OK;
}

HRESULT AudioDecoder::Seek(TimeUs t_us) {
    if (!m_reader) return E_ILLEGAL_METHOD_CALL;
    if (t_us < 0) t_us = 0;
    m_pending.clear();
    m_pending_pos = 0;
    if (m_duration_us > 0 && t_us >= m_duration_us) {
        // Past the end: SetCurrentPosition would fail; the next Read reports the end of the stream.
        m_eos = true;
        m_need_base = false;
        m_base_pts_us = t_us;
        m_frames_since_base = 0;
        return S_OK;
    }
    CT_RETURN_IF_FAILED(detail::SetReaderPosition(m_reader.Get(), t_us));
    if (m_convert && m_src_rate != kRate) m_resampler.Reset();
    m_seek_target = t_us;
    m_need_base = true;
    m_eos = false;
    return S_OK;
}

void AudioDecoder::AppendConverted(const float* in, uint32_t frames) {
    const float* stereo = in;
    if (m_src_channels != kChannels) {
        m_stereo.resize(static_cast<size_t>(frames) * kChannels);
        if (m_src_channels == 1) audio::dsp::MonoToStereo(in, m_stereo.data(), frames);
        else audio::dsp::DownmixToStereo(in, m_src_channels, m_stereo.data(), frames);
        stereo = m_stereo.data();
    }
    if (m_src_rate == kRate) {
        m_pending.insert(m_pending.end(), stereo, stereo + static_cast<size_t>(frames) * kChannels);
        return;
    }
    const size_t cap = static_cast<size_t>(frames) * kRate / m_src_rate + 64;
    m_resampled.resize(cap * kChannels);
    size_t done = 0;
    while (done < frames) {
        size_t consumed = 0;
        const size_t out = m_resampler.Process(stereo + done * kChannels, frames - done, m_resampled.data(), cap, &consumed);
        m_pending.insert(m_pending.end(), m_resampled.data(), m_resampled.data() + out * kChannels);
        done += consumed;
        if (consumed == 0 && out == 0) break;
    }
}

HRESULT AudioDecoder::DecodeMore() {
    for (;;) {
        DWORD actual = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        CT_RETURN_IF_FAILED(m_reader->ReadSample(m_stream, 0, &actual, &flags, &ts, sample.GetAddressOf()));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            m_eos = true;
            if (m_convert && m_src_rate != kRate) {
                // Drain the resampler's look-ahead.
                m_resampled.resize(1024 * kChannels);
                size_t n = 0;
                do {
                    n = m_resampler.Flush(m_resampled.data(), 1024);
                    m_pending.insert(m_pending.end(), m_resampled.data(), m_resampled.data() + n * kChannels);
                } while (n == 1024);
            }
            return S_OK;
        }
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        CT_RETURN_IF_FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()));
        BYTE* data = nullptr;
        DWORD len = 0;
        CT_RETURN_IF_FAILED(buf->Lock(&data, nullptr, &len));
        const int ch = m_convert ? m_src_channels : kChannels;
        const uint32_t frames = static_cast<uint32_t>(len / (sizeof(float) * ch));
        const size_t before = (m_pending.size() / kChannels) - m_pending_pos;
        if (m_convert) AppendConverted(reinterpret_cast<const float*>(data), frames);
        else m_pending.insert(m_pending.end(), reinterpret_cast<const float*>(data), reinterpret_cast<const float*>(data) + static_cast<size_t>(frames) * kChannels);
        buf->Unlock();
        if (m_need_base) {
            // The first packet after Init/Seek defines the timeline; drop the frames before the seek target.
            const TimeUs pts = ts / 10;
            int64_t skip = 0;
            if (pts < m_seek_target) skip = (m_seek_target - pts) * kRate / 1'000'000;
            const size_t avail = (m_pending.size() / kChannels) - m_pending_pos;
            if (static_cast<size_t>(skip) >= avail) {
                // Whole packet is before the target: drop it and keep the target (next packet's pts decides).
                m_pending.clear();
                m_pending_pos = 0;
                continue;
            }
            m_pending_pos += static_cast<size_t>(skip);
            m_base_pts_us = pts + skip * 1'000'000 / kRate;
            m_frames_since_base = 0;
            m_need_base = false;
        }
        if ((m_pending.size() / kChannels) - m_pending_pos > before) return S_OK;
    }
}

HRESULT AudioDecoder::Read(float* out, uint32_t capacity_frames, uint32_t& frames_out, TimeUs& pts_us) {
    frames_out = 0;
    pts_us = 0;
    if (!m_reader) return E_ILLEGAL_METHOD_CALL;
    bool have_pts = false;
    while (frames_out < capacity_frames) {
        size_t avail = (m_pending.size() / kChannels) - m_pending_pos;
        if (avail == 0) {
            if (m_eos) break;
            m_pending.clear();
            m_pending_pos = 0;
            CT_RETURN_IF_FAILED(DecodeMore());
            continue;
        }
        if (!have_pts) {
            pts_us = m_base_pts_us + m_frames_since_base * 1'000'000 / kRate;
            have_pts = true;
        }
        const size_t n = std::min<size_t>(avail, capacity_frames - frames_out);
        std::memcpy(out + static_cast<size_t>(frames_out) * kChannels, m_pending.data() + m_pending_pos * kChannels, n * kChannels * sizeof(float));
        m_pending_pos += n;
        m_frames_since_base += static_cast<int64_t>(n);
        frames_out += static_cast<uint32_t>(n);
    }
    return frames_out > 0 ? S_OK : S_FALSE;
}

} // namespace ct::media
