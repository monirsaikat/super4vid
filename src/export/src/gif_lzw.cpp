// GIF LZW compression (variable code size, 12-bit maximum, clear code when the table is full).
#include <ct/export/gif_encoder.h>

#include "gif_internal.h"

#include <algorithm>
#include <cstring>

namespace ct::exportng {

namespace {

// LSB-first bit packer into 255-byte, length-prefixed sub-blocks.
class BlockWriter {
public:
    explicit BlockWriter(std::vector<uint8_t>& out) : m_out(out) {}
    void Put(uint32_t code, int bits) {
        m_acc |= static_cast<uint64_t>(code) << m_bits;
        m_bits += bits;
        while (m_bits >= 8) {
            Byte(static_cast<uint8_t>(m_acc));
            m_acc >>= 8;
            m_bits -= 8;
        }
    }
    void Finish() {
        if (m_bits > 0) Byte(static_cast<uint8_t>(m_acc));
        m_acc = 0;
        m_bits = 0;
        if (m_len) FlushBlock();
        m_out.push_back(0);
    }

private:
    void Byte(uint8_t b) {
        m_block[m_len++] = b;
        if (m_len == 255) FlushBlock();
    }
    void FlushBlock() {
        m_out.push_back(static_cast<uint8_t>(m_len));
        m_out.insert(m_out.end(), m_block, m_block + m_len);
        m_len = 0;
    }
    std::vector<uint8_t>& m_out;
    uint64_t m_acc = 0;
    int m_bits = 0;
    uint8_t m_block[255] = {};
    int m_len = 0;
};

// (prefix code, next index) -> code. Open addressing; entries carry a generation so a clear is O(1).
class CodeTable {
public:
    static constexpr int kBits = 13;
    static constexpr uint32_t kSize = 1u << kBits;
    CodeTable() { std::memset(m_tag, 0, sizeof(m_tag)); }
    void Clear() {
        if (++m_gen == 4096) {   // generation is 12 bits
            std::memset(m_tag, 0, sizeof(m_tag));
            m_gen = 1;
        }
    }
    // Returns the code or -1; on -1, `slot` is where Insert puts it.
    int Find(uint32_t key, uint32_t& slot) const {
        const uint32_t tag = key | (m_gen << 20);
        uint32_t h = (key * 2654435761u) >> (32 - kBits);
        for (;;) {
            const uint32_t t = m_tag[h];
            if (t == tag) return m_code[h];
            if ((t >> 20) != m_gen) {
                slot = h;
                return -1;
            }
            h = (h + 1) & (kSize - 1);
        }
    }
    void Insert(uint32_t slot, uint32_t key, int code) {
        m_tag[slot] = key | (m_gen << 20);
        m_code[slot] = static_cast<uint16_t>(code);
    }

private:
    uint32_t m_tag[kSize];
    uint16_t m_code[kSize];
    uint32_t m_gen = 1;
};

// LSB-first bit buffer without sub-blocks (one LZW segment, stitched later).
class BitBuffer {
public:
    void Put(uint32_t code, int bits) {
        m_acc |= static_cast<uint64_t>(code) << m_bits;
        m_bits += bits;
        while (m_bits >= 8) {
            bytes.push_back(static_cast<uint8_t>(m_acc));
            m_acc >>= 8;
            m_bits -= 8;
        }
    }
    void Finish() {
        tail_bits = m_bits;
        tail = static_cast<uint8_t>(m_acc);
        m_acc = 0;
        m_bits = 0;
    }
    std::vector<uint8_t> bytes;   // whole bytes
    uint8_t tail = 0;             // the last tail_bits bits (LSB first)
    int tail_bits = 0;

private:
    uint64_t m_acc = 0;
    int m_bits = 0;
};

// The LZW core. With lead_clear the stream starts with a clear code; returns the code width after the last code
// (the width the following clear or EOI is written with).
template <class Sink>
int EncodeCore(const uint8_t* indices, size_t n, int min_code_size, Sink& w, bool lead_clear, CodeTable& table) {
    const int clear = 1 << min_code_size;
    const int eoi = clear + 1;
    const uint32_t mask = static_cast<uint32_t>(clear - 1);
    // `next` is the next free code. After emitting a code the decoder has exactly as many table entries as the
    // encoder, so both widen the code when `next` reaches 1 << code_size (same rule as giflib).
    int code_size = min_code_size + 1;
    int next = eoi + 1;
    auto emit = [&](int code) {
        w.Put(static_cast<uint32_t>(code), code_size);
        if (next >= (1 << code_size) && code_size < 12) ++code_size;
    };
    if (lead_clear) w.Put(static_cast<uint32_t>(clear), code_size);
    if (n > 0) {
        int cur = indices[0] & static_cast<int>(mask);
        for (size_t i = 1; i < n; ++i) {
            const uint32_t c = indices[i] & mask;
            const uint32_t key = (static_cast<uint32_t>(cur) << 8) | c;
            uint32_t slot = 0;
            const int found = table.Find(key, slot);
            if (found >= 0) {
                cur = found;
                continue;
            }
            emit(cur);
            if (next >= 4095) {   // table full: clear (read with 12 bits) instead of assigning code 4095
                w.Put(static_cast<uint32_t>(clear), code_size);
                table.Clear();
                code_size = min_code_size + 1;
                next = eoi + 1;
            } else {
                table.Insert(slot, key, next++);
            }
            cur = static_cast<int>(c);
        }
        emit(cur);
    }
    return code_size;
}

} // namespace

void GifEncoder::LzwEncode(const uint8_t* indices, size_t n, int min_code_size, std::vector<uint8_t>& out) {
    min_code_size = std::clamp(min_code_size, 2, 8);
    out.reserve(out.size() + n / 2 + 16);
    BlockWriter w(out);
    auto table = std::make_unique<CodeTable>();
    const int code_size = EncodeCore(indices, n, min_code_size, w, true, *table);
    w.Put(static_cast<uint32_t>((1 << min_code_size) + 1), code_size);   // EOI
    w.Finish();
}

namespace gif_detail {

void LzwEncodeSegmented(const uint8_t* indices, size_t n, int min_code_size, std::vector<uint8_t>& out, size_t segment,
                        Workers* workers) {
    min_code_size = std::clamp(min_code_size, 2, 8);
    segment = std::max<size_t>(segment, 1024);
    const int count = static_cast<int>(std::max<size_t>((n + segment - 1) / segment, 1));
    if (count == 1) {
        GifEncoder::LzwEncode(indices, n, min_code_size, out);
        return;
    }
    struct Seg {
        BitBuffer bits;
        int final_size = 0;
    };
    std::vector<Seg> segs(static_cast<size_t>(count));
    auto encode = [&](int k) {
        const size_t a = static_cast<size_t>(k) * segment;
        const size_t len = std::min(segment, n - a);
        Seg& s = segs[static_cast<size_t>(k)];
        s.bits.bytes.reserve(len);
        auto table = std::make_unique<CodeTable>();
        s.final_size = EncodeCore(indices + a, len, min_code_size, s.bits, k == 0, *table);
        s.bits.Finish();
    };
    if (workers) workers->For(count, encode);
    else
        for (int k = 0; k < count; ++k) encode(k);
    // Stitch: segment k > 0 is preceded by a clear code at segment k-1's final width (the decoder restarts its table).
    size_t total = 0;
    for (const Seg& s : segs) total += s.bits.bytes.size() + 2;
    out.reserve(out.size() + total + total / 255 + 16);
    BlockWriter w(out);
    const uint32_t clear = 1u << min_code_size;
    for (int k = 0; k < count; ++k) {
        const Seg& s = segs[static_cast<size_t>(k)];
        if (k > 0) w.Put(clear, segs[static_cast<size_t>(k) - 1].final_size);
        for (uint8_t b : s.bits.bytes) w.Put(b, 8);
        if (s.bits.tail_bits) w.Put(s.bits.tail, s.bits.tail_bits);
    }
    w.Put(clear + 1, segs.back().final_size);   // EOI
    w.Finish();
}

} // namespace gif_detail

} // namespace ct::exportng
