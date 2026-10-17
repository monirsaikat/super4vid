#include <ct/events/ctev.h>

#include <ct/core/fs.h>

#include <share.h>

#include <algorithm>
#include <cstring>

namespace ct::events {

namespace {

constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

uint32_t FnvAppend(uint32_t h, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
    return h;
}

size_t Pad4(size_t n) { return (n + 3) & ~size_t(3); }

} // namespace

uint32_t ShapeId(const uint8_t* bgra, size_t bytes, int16_t hot_x, int16_t hot_y) {
    uint32_t h = FnvAppend(kFnvOffset, bgra, bytes);
    const uint8_t hot[4] = {static_cast<uint8_t>(hot_x & 0xFF), static_cast<uint8_t>((hot_x >> 8) & 0xFF),
                            static_cast<uint8_t>(hot_y & 0xFF), static_cast<uint8_t>((hot_y >> 8) & 0xFF)};
    return FnvAppend(h, hot, sizeof(hot));
}

std::vector<uint8_t> RleEncode(const uint8_t* bgra, size_t bytes) {
    std::vector<uint8_t> out;
    const size_t pixels = bytes / 4;
    size_t i = 0;
    while (i < pixels) {
        uint32_t px = 0;
        std::memcpy(&px, bgra + i * 4, 4);
        size_t run = 1;
        while (i + run < pixels && run < 255) {
            uint32_t next = 0;
            std::memcpy(&next, bgra + (i + run) * 4, 4);
            if (next != px) break;
            ++run;
        }
        out.push_back(static_cast<uint8_t>(run));
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&px);
        out.insert(out.end(), b, b + 4);
        i += run;
    }
    return out;
}

bool RleDecode(const uint8_t* data, size_t size, size_t pixel_count, std::vector<uint8_t>& out_bgra) {
    out_bgra.clear();
    out_bgra.reserve(pixel_count * 4);
    size_t pos = 0;
    while (pos + 5 <= size && out_bgra.size() < pixel_count * 4) {
        const size_t run = data[pos];
        if (run == 0) return false;
        if (out_bgra.size() / 4 + run > pixel_count) return false;
        for (size_t r = 0; r < run; ++r) out_bgra.insert(out_bgra.end(), data + pos + 1, data + pos + 5);
        pos += 5;
    }
    return out_bgra.size() == pixel_count * 4;
}

void CopyLabel(char* dst, size_t dst_size, std::string_view src) {
    if (dst_size == 0) return;
    size_t n = std::min(src.size(), dst_size - 1);
    // Do not cut a multi-byte sequence: back up over continuation bytes (10xxxxxx) to the lead byte.
    if (n < src.size())
        while (n > 0 && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, src.data(), n);
    std::memset(dst + n, 0, dst_size - n);
}

// ---- EventWriter -----------------------------------------------------------------------------------------------

EventWriter::~EventWriter() { Close(); }

bool EventWriter::Open(std::string_view path, const EventHeader& header, Err* err, size_t flush_threshold) {
    Close();
    m_memory = false;
    m_buffer.clear();
    m_shapes.clear();
    m_written = 0;
    m_records = 0;
    m_threshold = flush_threshold < 4096 ? 4096 : flush_threshold;
    const std::wstring wpath = fs::ToWinPath(path);
    // Deny other writers but allow readers: the editor (or a crash-recovery tool) may read a file being recorded.
    FILE* f = _wfsopen(wpath.c_str(), L"wbN", _SH_DENYWR);
    if (!f) {
        if (err) err->Set("Could not create the events file " + std::string(path));
        return false;
    }
    m_file = f;
    m_buffer.reserve(m_threshold + 4096);
    EventHeader h = header;
    h.header_size = sizeof(EventHeader);
    m_buffer.append(reinterpret_cast<const char*>(&h), sizeof(h));
    if (!Flush()) {
        if (err) err->Set("Could not write the events file header " + std::string(path));
        Close();
        return false;
    }
    return true;
}

void EventWriter::OpenMemory(const EventHeader& header) {
    Close();
    m_memory = true;
    m_buffer.clear();
    m_shapes.clear();
    m_written = 0;
    m_records = 0;
    EventHeader h = header;
    h.header_size = sizeof(EventHeader);
    m_buffer.append(reinterpret_cast<const char*>(&h), sizeof(h));
}

void EventWriter::WriteRecord(uint8_t type, TimeUs t_us, const void* payload, size_t payload_size, const void* tail,
                              size_t tail_size) {
    if (!IsOpen()) return;
    const size_t body = payload_size + tail_size;
    const size_t total = kRecordHeaderSize + Pad4(body);
    RecordHeader rh;
    rh.size = static_cast<uint32_t>(total);
    rh.type = type;
    rh.t_us = t_us;
    m_buffer.append(reinterpret_cast<const char*>(&rh), sizeof(rh));
    if (payload_size) m_buffer.append(static_cast<const char*>(payload), payload_size);
    if (tail_size) m_buffer.append(static_cast<const char*>(tail), tail_size);
    m_buffer.append(Pad4(body) - body, '\0');
    ++m_records;
    if (!m_memory && m_buffer.size() >= m_threshold) Flush();
}

void EventWriter::CursorMove(TimeUs t_us, int32_t x, int32_t y, uint32_t shape_id, bool visible) {
    CursorMovePayload p;
    p.x = x;
    p.y = y;
    p.shape_id = shape_id;
    p.visible = visible ? 1 : 0;
    WriteRecord(static_cast<uint8_t>(RecordType::CursorMove), t_us, &p, sizeof(p));
}

void EventWriter::MouseButton(TimeUs t_us, Button button, bool down, int32_t x, int32_t y) {
    MouseButtonPayload p;
    p.button = static_cast<uint8_t>(button);
    p.down = down ? 1 : 0;
    p.x = x;
    p.y = y;
    WriteRecord(static_cast<uint8_t>(RecordType::MouseButton), t_us, &p, sizeof(p));
}

void EventWriter::MouseWheel(TimeUs t_us, int16_t delta_v, int16_t delta_h, int32_t x, int32_t y) {
    MouseWheelPayload p;
    p.delta_v = delta_v;
    p.delta_h = delta_h;
    p.x = x;
    p.y = y;
    WriteRecord(static_cast<uint8_t>(RecordType::MouseWheel), t_us, &p, sizeof(p));
}

void EventWriter::Key(TimeUs t_us, const KeyPayload& key) {
    KeyPayload p = key;
    p.label[sizeof(p.label) - 1] = '\0';
    WriteRecord(static_cast<uint8_t>(RecordType::Key), t_us, &p, sizeof(p));
}

bool EventWriter::HasShape(uint32_t shape_id) const {
    return std::binary_search(m_shapes.begin(), m_shapes.end(), shape_id);
}

uint32_t EventWriter::CursorShape(TimeUs t_us, uint32_t shape_id, uint16_t w, uint16_t h, int16_t hot_x, int16_t hot_y,
                                  const uint8_t* bgra_premultiplied) {
    const size_t bytes = size_t(w) * size_t(h) * 4;
    if (shape_id == 0) shape_id = ShapeId(bgra_premultiplied, bytes, hot_x, hot_y);
    if (HasShape(shape_id)) return shape_id;
    m_shapes.insert(std::upper_bound(m_shapes.begin(), m_shapes.end(), shape_id), shape_id);
    CursorShapePayload p;
    p.shape_id = shape_id;
    p.w = w;
    p.h = h;
    p.hot_x = hot_x;
    p.hot_y = hot_y;
    p.format = 0;
    const std::vector<uint8_t> rle = RleEncode(bgra_premultiplied, bytes);
    if (rle.size() < bytes) {
        p.compression = 1;
        p.data_size = static_cast<uint32_t>(rle.size());
        WriteRecord(static_cast<uint8_t>(RecordType::CursorShape), t_us, &p, sizeof(p), rle.data(), rle.size());
    } else {
        p.compression = 0;
        p.data_size = static_cast<uint32_t>(bytes);
        WriteRecord(static_cast<uint8_t>(RecordType::CursorShape), t_us, &p, sizeof(p), bgra_premultiplied, bytes);
    }
    return shape_id;
}

void EventWriter::WindowFocus(TimeUs t_us, const WindowFocusPayload& focus) {
    WindowFocusPayload p = focus;
    p.title[sizeof(p.title) - 1] = '\0';
    p.exe[sizeof(p.exe) - 1] = '\0';
    WriteRecord(static_cast<uint8_t>(RecordType::WindowFocus), t_us, &p, sizeof(p));
}

void EventWriter::Marker(TimeUs t_us, uint32_t kind, std::string_view label) {
    MarkerPayload p;
    p.kind = kind;
    CopyLabel(p.label, sizeof(p.label), label);
    WriteRecord(static_cast<uint8_t>(RecordType::Marker), t_us, &p, sizeof(p));
}

void EventWriter::Pause(TimeUs t_us, bool paused) {
    PausePayload p;
    p.paused = paused ? 1 : 0;
    WriteRecord(static_cast<uint8_t>(RecordType::Pause), t_us, &p, sizeof(p));
}

void EventWriter::Typing(TimeUs t_us, bool started) {
    TypingPayload p;
    p.started = started ? 1 : 0;
    WriteRecord(static_cast<uint8_t>(RecordType::Typing), t_us, &p, sizeof(p));
}

bool EventWriter::Flush() {
    if (m_memory || !m_file) return m_memory;
    if (!m_buffer.empty()) {
        const size_t n = std::fwrite(m_buffer.data(), 1, m_buffer.size(), m_file);
        m_written += n;
        if (n != m_buffer.size()) {
            m_buffer.erase(0, n);
            return false;
        }
        m_buffer.clear();   // keeps the capacity: no allocation on the next records
    }
    return std::fflush(m_file) == 0;
}

bool EventWriter::Close() {
    bool ok = true;
    if (m_file) {
        ok = Flush();
        if (std::fclose(m_file) != 0) ok = false;
        m_file = nullptr;
    }
    m_memory = false;
    return ok;
}

// ---- EventReader -----------------------------------------------------------------------------------------------

bool EventReader::Open(std::string_view path, Err* err) {
    std::string bytes;
    if (!fs::ReadAll(path, bytes)) {
        if (err) err->Set("Could not read the events file " + std::string(path));
        return false;
    }
    return Parse(std::move(bytes), err);
}

bool EventReader::Parse(std::string bytes, Err* err) {
    m_bytes = std::move(bytes);
    m_header = EventHeader{};
    m_truncated = false;
    m_pos = m_header_size = 0;
    // The fixed fields up to `flags` are required; header_size may grow in later versions.
    if (m_bytes.size() < 8 || std::memcmp(m_bytes.data(), "CTEV", 4) != 0) {
        if (err) err->Set("Not a Camtesia events file (bad magic)");
        return false;
    }
    uint16_t header_size = 0;
    std::memcpy(&header_size, m_bytes.data() + 6, 2);
    if (header_size < 40 || header_size > m_bytes.size() || (header_size & 3) != 0) {
        if (err) err->Set("Damaged events file header");
        return false;
    }
    std::memcpy(&m_header, m_bytes.data(), std::min<size_t>(header_size, sizeof(EventHeader)));
    if (m_header.version == 0) {
        if (err) err->Set("Unsupported events file version");
        return false;
    }
    m_header_size = m_pos = header_size;
    return true;
}

bool EventReader::Next(RecordView& out) {
    if (m_pos >= m_bytes.size()) return false;
    if (m_bytes.size() - m_pos < kRecordHeaderSize) {
        m_truncated = true;
        m_pos = m_bytes.size();
        return false;
    }
    RecordHeader rh;
    std::memcpy(&rh, m_bytes.data() + m_pos, sizeof(rh));
    if (rh.size < kRecordHeaderSize || rh.size > m_bytes.size() - m_pos) {
        m_truncated = true;   // torn write or garbage: nothing after this point can be trusted
        m_pos = m_bytes.size();
        return false;
    }
    out.type = rh.type;
    out.t_us = rh.t_us;
    out.payload = reinterpret_cast<const uint8_t*>(m_bytes.data()) + m_pos + kRecordHeaderSize;
    out.payload_size = rh.size - kRecordHeaderSize;
    m_pos += rh.size;
    return true;
}

} // namespace ct::events
