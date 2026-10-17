#pragma once
// ct/audio/wav.h
// Minimal RIFF/WAVE writer (float32 or PCM16) and reader (PCM 16/24/32, float32, WAVE_FORMAT_EXTENSIBLE).
// Used by tests, audio_bench and debugging dumps. Blocking file IO: never on an audio or UI thread.
// Thread model: one thread per object. Lifetime: the writer patches the RIFF/data sizes on Close (or destruction).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ct::audio {

struct WavWriter {
    WavWriter() = default;
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    bool Open(std::string_view path, int rate, int channels, bool float32);   // UTF-8 path; overwrites
    bool Write(const float* interleaved, size_t frames);   // float32: as is; PCM16: dsp::FloatToPcm16
    bool Close();                                           // flushes and patches the RIFF and data sizes
    uint64_t FramesWritten() const { return m_frames; }

private:
    bool FlushBuffer();

    void* m_file = nullptr;   // HANDLE
    int m_rate = 0;
    int m_channels = 0;
    bool m_float = true;
    bool m_failed = false;
    uint64_t m_frames = 0;
    uint64_t m_data_bytes = 0;
    std::vector<uint8_t> m_buffer;   // 64 KB write buffer
};

struct WavReader {
    bool Open(std::string_view path);   // reads the whole file into memory
    int Rate() const { return m_rate; }
    int Channels() const { return m_channels; }
    size_t Frames() const { return m_frames; }
    size_t Read(float* out, size_t frames);   // interleaved float; returns frames read (0 at the end)
    void Seek(size_t frame) { m_pos = frame < m_frames ? frame : m_frames; }

private:
    std::string m_bytes;
    size_t m_data_offset = 0;
    size_t m_frames = 0;
    size_t m_pos = 0;
    int m_rate = 0;
    int m_channels = 0;
    int m_bits = 0;
    bool m_float = false;
};

} // namespace ct::audio
