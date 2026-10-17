#include <ct/audio/wav.h>

#include <ct/audio/dsp.h>
#include <ct/core/fs.h>
#include <ct/platform/win.h>

#include <cstring>

namespace ct::audio {

namespace {

constexpr size_t kBufferBytes = 64 * 1024;
constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

void Put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint16_t Get16(const char* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
uint32_t Get32(const char* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// 46-byte header: RIFF, WAVE, fmt (18 bytes, cbSize 0), data.
constexpr size_t kHeaderBytes = 12 + 8 + 18 + 8;

void BuildHeader(uint8_t* h, int rate, int channels, bool is_float, uint64_t data_bytes) {
    const uint32_t data = data_bytes > 0xFFFFFFFFull - kHeaderBytes ? 0xFFFFFFFFu - kHeaderBytes
                                                                     : static_cast<uint32_t>(data_bytes);
    const uint16_t bits = is_float ? 32 : 16;
    const uint16_t block = static_cast<uint16_t>(channels * bits / 8);
    std::memcpy(h, "RIFF", 4);
    Put32(h + 4, static_cast<uint32_t>(kHeaderBytes - 8) + data);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    Put32(h + 16, 18);
    Put16(h + 20, is_float ? kFormatFloat : kFormatPcm);
    Put16(h + 22, static_cast<uint16_t>(channels));
    Put32(h + 24, static_cast<uint32_t>(rate));
    Put32(h + 28, static_cast<uint32_t>(rate) * block);
    Put16(h + 32, block);
    Put16(h + 34, bits);
    Put16(h + 36, 0);
    std::memcpy(h + 38, "data", 4);
    Put32(h + 42, data);
}

bool WriteRaw(void* file, const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (bytes > 0) {
        const DWORD chunk = bytes > (1u << 30) ? (1u << 30) : static_cast<DWORD>(bytes);
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(file), p, chunk, &written, nullptr) || written != chunk) return false;
        p += chunk;
        bytes -= chunk;
    }
    return true;
}

} // namespace

WavWriter::~WavWriter() { Close(); }

bool WavWriter::Open(std::string_view path, int rate, int channels, bool float32) {
    Close();
    if (rate <= 0 || channels <= 0 || channels > 8) return false;
    const std::wstring wpath = fs::ToWinPath(path);
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    m_file = h;
    m_rate = rate;
    m_channels = channels;
    m_float = float32;
    m_failed = false;
    m_frames = 0;
    m_data_bytes = 0;
    m_buffer.clear();
    m_buffer.reserve(kBufferBytes);
    uint8_t header[kHeaderBytes];
    BuildHeader(header, rate, channels, float32, 0);
    if (!WriteRaw(m_file, header, sizeof(header))) {
        Close();
        return false;
    }
    return true;
}

bool WavWriter::FlushBuffer() {
    if (m_buffer.empty()) return !m_failed;
    if (!WriteRaw(m_file, m_buffer.data(), m_buffer.size())) m_failed = true;
    m_buffer.clear();
    return !m_failed;
}

bool WavWriter::Write(const float* interleaved, size_t frames) {
    if (!m_file || m_failed) return false;
    const size_t samples_per_block = 4096;
    size_t samples = frames * static_cast<size_t>(m_channels);
    int16_t pcm[samples_per_block];
    while (samples > 0) {
        const size_t n = samples < samples_per_block ? samples : samples_per_block;
        const size_t bytes = n * (m_float ? 4 : 2);
        if (m_buffer.size() + bytes > kBufferBytes && !FlushBuffer()) return false;
        const size_t at = m_buffer.size();
        m_buffer.resize(at + bytes);
        if (m_float) {
            std::memcpy(m_buffer.data() + at, interleaved, bytes);
        } else {
            dsp::FloatToPcm16(interleaved, pcm, n);
            std::memcpy(m_buffer.data() + at, pcm, bytes);
        }
        interleaved += n;
        samples -= n;
        m_data_bytes += bytes;
    }
    m_frames += frames;
    return true;
}

bool WavWriter::Close() {
    if (!m_file) return true;
    bool ok = FlushBuffer();
    uint8_t header[kHeaderBytes];
    BuildHeader(header, m_rate, m_channels, m_float, m_data_bytes);
    LARGE_INTEGER zero{};
    ok = ok && SetFilePointerEx(static_cast<HANDLE>(m_file), zero, nullptr, FILE_BEGIN) &&
         WriteRaw(m_file, header, sizeof(header));
    CloseHandle(static_cast<HANDLE>(m_file));
    m_file = nullptr;
    return ok;
}

bool WavReader::Open(std::string_view path) {
    m_frames = 0;
    m_pos = 0;
    m_rate = 0;
    m_channels = 0;
    if (!fs::ReadAll(path, m_bytes)) return false;
    const char* b = m_bytes.data();
    const size_t size = m_bytes.size();
    if (size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) return false;
    size_t at = 12;
    bool have_fmt = false;
    uint16_t tag = 0;
    while (at + 8 <= size) {
        const uint32_t len = Get32(b + at + 4);
        const size_t body = at + 8;
        if (std::memcmp(b + at, "fmt ", 4) == 0 && len >= 16 && body + len <= size) {
            tag = Get16(b + body);
            m_channels = Get16(b + body + 2);
            m_rate = static_cast<int>(Get32(b + body + 4));
            m_bits = Get16(b + body + 14);
            if (tag == kFormatExtensible && len >= 40) tag = Get16(b + body + 24);   // first 2 bytes of SubFormat
            have_fmt = true;
        } else if (std::memcmp(b + at, "data", 4) == 0 && have_fmt) {
            const size_t avail = size - body;
            const size_t data_len = len > avail ? avail : len;   // tolerate unpatched/truncated sizes
            m_float = tag == kFormatFloat;
            if (!(tag == kFormatPcm || tag == kFormatFloat) || m_channels <= 0) return false;
            if (m_float && m_bits != 32) return false;
            if (!m_float && m_bits != 16 && m_bits != 24 && m_bits != 32) return false;
            m_data_offset = body;
            m_frames = data_len / (static_cast<size_t>(m_channels) * (m_bits / 8));
            return true;
        }
        at = body + len + (len & 1);
    }
    return false;
}

size_t WavReader::Read(float* out, size_t frames) {
    const size_t n = frames < m_frames - m_pos ? frames : m_frames - m_pos;
    const size_t samples = n * static_cast<size_t>(m_channels);
    const size_t bps = static_cast<size_t>(m_bits / 8);
    const char* src = m_bytes.data() + m_data_offset + m_pos * m_channels * bps;
    if (m_float) {
        std::memcpy(out, src, samples * 4);
    } else if (m_bits == 16) {
        for (size_t i = 0; i < samples; ++i) out[i] = static_cast<int16_t>(Get16(src + 2 * i)) * (1.0f / 32768.0f);
    } else if (m_bits == 24) {
        for (size_t i = 0; i < samples; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(src + 3 * i);
            const int32_t v = static_cast<int32_t>((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24));
            out[i] = static_cast<float>(v) * (1.0f / 2147483648.0f);
        }
    } else {
        for (size_t i = 0; i < samples; ++i)
            out[i] = static_cast<float>(static_cast<int32_t>(Get32(src + 4 * i))) * (1.0f / 2147483648.0f);
    }
    m_pos += n;
    return n;
}

} // namespace ct::audio
