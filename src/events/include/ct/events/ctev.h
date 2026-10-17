#pragma once
// ct/events/ctev.h
// The binary events file `events.ctev` (docs/04 section 2): packed on-disk structs, a buffered append-only writer
// and a forward-tolerant record reader. Coordinates are capture-region pixels, times are media microseconds.
// Thread model: an EventWriter / EventReader belongs to one thread (P05: the ct-events-io thread owns the writer).
// Lifetime: EventWriter owns its FILE handle; Close() (or the destructor) flushes and closes it.

#include <ct/core/error.h>
#include <ct/core/types.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace ct::events {

constexpr uint16_t kVersion = 1;
constexpr uint32_t kRecordHeaderSize = 16;   // size u32, type u8, pad u8[3], t_us i64

enum class RecordType : uint8_t {
    CursorMove = 1, MouseButton = 2, MouseWheel = 3, Key = 4, CursorShape = 5,
    WindowFocus = 6, Marker = 7, Pause = 8, Typing = 9,
};

enum class Button : uint8_t { Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4 };

// Header flags.
constexpr uint32_t kFlagKeystrokes = 1u << 0;   // keystrokes were recorded
constexpr uint32_t kFlagAllKeys = 1u << 1;      // all keys (else shortcuts only)

// KEY modifier bits.
constexpr uint8_t kModCtrl = 1u << 0;
constexpr uint8_t kModShift = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;
constexpr uint8_t kModWin = 1u << 3;

#pragma pack(push, 1)
// File header. docs/04 says header_size = 40, but the listed fields take 44 bytes; the writer stores 44 and readers
// honour whatever header_size says (DECISIONS.md, P07 task 7.0).
struct EventHeader {
    char magic[4] = {'C', 'T', 'E', 'V'};
    uint16_t version = kVersion;
    uint16_t header_size = 44;
    int64_t epoch_qpc = 0;
    int64_t qpc_frequency = 0;
    int32_t region_x = 0, region_y = 0, region_w = 0, region_h = 0;
    uint32_t flags = 0;
};

struct RecordHeader {
    uint32_t size = 0;   // total bytes incl. this header, multiple of 4
    uint8_t type = 0;
    uint8_t pad[3] = {};
    int64_t t_us = 0;
};

struct CursorMovePayload { int32_t x = 0, y = 0; uint32_t shape_id = 0; uint8_t visible = 1; uint8_t pad[3] = {}; };
struct MouseButtonPayload { uint8_t button = 0, down = 0; uint8_t pad[2] = {}; int32_t x = 0, y = 0; };
struct MouseWheelPayload { int16_t delta_v = 0, delta_h = 0; int32_t x = 0, y = 0; };
struct KeyPayload {
    uint16_t vk = 0, scancode = 0;
    uint8_t down = 0, modifiers = 0, extended = 0, pad = 0;
    uint32_t utf32_char = 0;   // 0 if none
    char label[32] = {};       // UTF-8, zero-terminated: "Ctrl", "A", "Enter"
};
struct CursorShapePayload {   // followed by data_size bytes of pixel data
    uint32_t shape_id = 0;
    uint16_t w = 0, h = 0;
    int16_t hot_x = 0, hot_y = 0;
    uint8_t format = 0;        // 0 = BGRA premultiplied
    uint8_t compression = 0;   // 0 raw, 1 RLE (u8 run_length 1..255, u32 pixel)
    uint16_t pad = 0;
    uint32_t data_size = 0;
};
struct WindowFocusPayload { uint64_t hwnd = 0; int32_t x = 0, y = 0, w = 0, h = 0; char title[256] = {}; char exe[64] = {}; };
struct MarkerPayload { uint32_t kind = 0; char label[64] = {}; };
struct PausePayload { uint8_t paused = 0; uint8_t pad[3] = {}; };
struct TypingPayload { uint8_t started = 0; uint8_t pad[3] = {}; };
#pragma pack(pop)

static_assert(sizeof(EventHeader) == 44 && sizeof(RecordHeader) == kRecordHeaderSize);
static_assert(sizeof(CursorMovePayload) == 16 && sizeof(MouseButtonPayload) == 12 && sizeof(MouseWheelPayload) == 12);
static_assert(sizeof(KeyPayload) == 44 && sizeof(CursorShapePayload) == 20 && sizeof(WindowFocusPayload) == 344);
static_assert(sizeof(MarkerPayload) == 68 && sizeof(PausePayload) == 4 && sizeof(TypingPayload) == 4);

// shape_id: FNV-1a 32-bit over the premultiplied BGRA pixels followed by hot_x, hot_y (little-endian i16 each).
uint32_t ShapeId(const uint8_t* bgra, size_t bytes, int16_t hot_x, int16_t hot_y);
// RLE as docs/04: runs of identical 32-bit pixels as (u8 run_length 1..255, u32 pixel). `bytes` must be a multiple of 4.
std::vector<uint8_t> RleEncode(const uint8_t* bgra, size_t bytes);
// Decodes exactly `pixel_count` pixels; false when the data is malformed or yields a different count.
bool RleDecode(const uint8_t* data, size_t size, size_t pixel_count, std::vector<uint8_t>& out_bgra);
// Copies UTF-8 into a fixed zero-terminated field, truncating on a code point boundary.
void CopyLabel(char* dst, size_t dst_size, std::string_view src);

// Buffered append-only writer. Each Write call appends one padded record to an in-memory buffer (no syscall; no
// allocation once the buffer reached its working size). Flush() hands the buffer to the OS (fwrite + fflush), so a
// process crash after Flush() loses nothing written before it. The buffer also flushes itself when it grows past
// `flush_threshold`. Memory mode (OpenMemory) keeps every byte in Bytes() (tests, tools, synthetic fixtures).
class EventWriter {
public:
    EventWriter() = default;
    ~EventWriter();
    EventWriter(const EventWriter&) = delete;
    EventWriter& operator=(const EventWriter&) = delete;

    // Creates/truncates the file and writes the header (flushed immediately).
    bool Open(std::string_view path, const EventHeader& header, Err* err, size_t flush_threshold = 256 * 1024);
    void OpenMemory(const EventHeader& header);
    bool IsOpen() const { return m_file != nullptr || m_memory; }

    void CursorMove(TimeUs t_us, int32_t x, int32_t y, uint32_t shape_id, bool visible);
    void MouseButton(TimeUs t_us, Button button, bool down, int32_t x, int32_t y);
    void MouseWheel(TimeUs t_us, int16_t delta_v, int16_t delta_h, int32_t x, int32_t y);
    void Key(TimeUs t_us, const KeyPayload& key);
    // Writes a shape once per file (a known shape_id is ignored) and picks RLE when it is smaller than raw.
    // shape_id 0 means "compute it with ShapeId". Returns the shape id.
    uint32_t CursorShape(TimeUs t_us, uint32_t shape_id, uint16_t w, uint16_t h, int16_t hot_x, int16_t hot_y,
                         const uint8_t* bgra_premultiplied);
    bool HasShape(uint32_t shape_id) const;
    void WindowFocus(TimeUs t_us, const WindowFocusPayload& focus);
    void Marker(TimeUs t_us, uint32_t kind, std::string_view label);
    void Pause(TimeUs t_us, bool paused);
    void Typing(TimeUs t_us, bool started);
    // Any record type (unknown ones too): payload + optional tail, zero-padded to a multiple of 4.
    void WriteRecord(uint8_t type, TimeUs t_us, const void* payload, size_t payload_size, const void* tail = nullptr,
                     size_t tail_size = 0);

    bool Flush();   // false when the OS write failed (disk full); the data stays buffered for the next attempt
    bool Close();   // Flush + fclose; idempotent

    size_t BufferedBytes() const { return m_memory ? 0 : m_buffer.size(); }
    uint64_t BytesWritten() const { return m_memory ? m_buffer.size() : m_written; }
    uint64_t RecordCount() const { return m_records; }
    const std::string& Bytes() const { return m_buffer; }   // memory mode: header + all records

private:
    FILE* m_file = nullptr;
    bool m_memory = false;
    size_t m_threshold = 256 * 1024;
    std::string m_buffer;
    uint64_t m_written = 0;
    uint64_t m_records = 0;
    std::vector<uint32_t> m_shapes;   // ids already written, sorted
};

// One record as stored. `payload` points into the reader's buffer and stays valid until the next Open/Parse.
struct RecordView {
    uint8_t type = 0;
    TimeUs t_us = 0;
    const uint8_t* payload = nullptr;
    uint32_t payload_size = 0;   // size - 16, includes the writer's zero padding
};

// Sequential reader over a whole file held in memory. Unknown record types are returned like the others (callers
// skip them by type). A truncated last record (crash while writing) ends the iteration and sets Truncated().
class EventReader {
public:
    bool Open(std::string_view path, Err* err);
    bool Parse(std::string bytes, Err* err);   // takes ownership of the bytes
    const EventHeader& Header() const { return m_header; }
    bool Next(RecordView& out);
    void Rewind() { m_pos = m_header_size; }
    bool Truncated() const { return m_truncated; }

private:
    std::string m_bytes;
    EventHeader m_header;
    size_t m_header_size = 0;
    size_t m_pos = 0;
    bool m_truncated = false;
};

} // namespace ct::events
