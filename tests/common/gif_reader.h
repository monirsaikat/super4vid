#pragma once
// tests/common/gif_reader.h
// Independent GIF87a/89a decoder for tests and tools (header-only, no dependencies): LZW decode with code-size growth
// and clear codes, global/local color tables, GCE (delay, disposal, transparency), NETSCAPE2.0 loop count,
// interlaced images, and compositing into full RGBA canvases (disposal 0/1 leave, 2 background, 3 previous).
// Thread model: plain functions on caller-owned data.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ct::test::gif {

struct LzwStats {
    int clear_codes = 0;      // including the leading one
    int max_code_size = 0;    // widest code read, in bits
    size_t consumed = 0;      // bytes of sub-block data consumed, including the 0 terminator
    bool saw_eoi = false;
};

// data: length-prefixed sub-blocks (ending with a 0 block) as written after the min_code_size byte.
// Decodes until EOI (or the data ends); fails on invalid codes or if more than max_out indices would be produced.
inline bool LzwDecode(const uint8_t* data, size_t size, int min_code_size, std::vector<uint8_t>& out, LzwStats* stats = nullptr,
                      size_t max_out = static_cast<size_t>(1) << 30) {
    out.clear();
    LzwStats st;
    if (min_code_size < 2 || min_code_size > 8) return false;
    // Gather the sub-blocks into a contiguous bit stream.
    std::vector<uint8_t> bytes;
    size_t pos = 0;
    for (;;) {
        if (pos >= size) return false;
        const size_t len = data[pos++];
        if (len == 0) break;
        if (pos + len > size) return false;
        bytes.insert(bytes.end(), data + pos, data + pos + len);
        pos += len;
    }
    st.consumed = pos;

    const int clear = 1 << min_code_size;
    const int eoi = clear + 1;
    std::vector<uint16_t> prefix(4096);
    std::vector<uint8_t> suffix(4096), first(4096);
    std::vector<uint8_t> stack(4097);
    for (int i = 0; i < clear; ++i) {
        prefix[i] = 0xFFFF;
        suffix[i] = static_cast<uint8_t>(i);
        first[i] = static_cast<uint8_t>(i);
    }
    int code_size = min_code_size + 1;
    int next = eoi + 1;
    int prev = -1;
    size_t bitpos = 0;
    const size_t total_bits = bytes.size() * 8;
    for (;;) {
        if (bitpos + static_cast<size_t>(code_size) > total_bits) break;   // ran out without EOI (tolerated)
        int code = 0;
        for (int b = 0; b < code_size; ++b, ++bitpos)
            code |= ((bytes[bitpos >> 3] >> (bitpos & 7)) & 1) << b;
        if (code_size > st.max_code_size) st.max_code_size = code_size;
        if (code == clear) {
            ++st.clear_codes;
            code_size = min_code_size + 1;
            next = eoi + 1;
            prev = -1;
            continue;
        }
        if (code == eoi) {
            st.saw_eoi = true;
            break;
        }
        if (prev < 0) {
            if (code >= clear) return false;
            out.push_back(static_cast<uint8_t>(code));
            prev = code;
            continue;
        }
        int sp = 0;
        int cur = code;
        uint8_t first_byte;
        if (code < next) {
            first_byte = first[code];
        } else if (code == next && next < 4096) {
            first_byte = first[prev];
            stack[sp++] = first_byte;   // KwKwK: string(prev) + first(prev)
            cur = prev;
        } else {
            return false;
        }
        while (cur >= 0 && cur != 0xFFFF) {
            if (cur >= clear && cur <= eoi) return false;
            if (sp >= 4097) return false;
            stack[sp++] = suffix[cur];
            cur = prefix[cur] == 0xFFFF ? -1 : prefix[cur];
        }
        if (out.size() + static_cast<size_t>(sp) > max_out) return false;
        while (sp > 0) out.push_back(stack[--sp]);
        if (next < 4096) {
            prefix[next] = static_cast<uint16_t>(prev);
            suffix[next] = first_byte;
            first[next] = first[prev];
            ++next;
            if (next >= (1 << code_size) && code_size < 12) ++code_size;
        }
        prev = code;
    }
    if (stats) *stats = st;
    return true;
}

struct Frame {
    int x = 0, y = 0, w = 0, h = 0;
    int delay_cs = 0;
    int disposal = 0;
    int transparent = -1;            // index or -1
    bool interlaced = false;
    bool local_table = false;
    int min_code_size = 0;
    std::vector<uint32_t> palette;   // the active table (local or global), 0x00RRGGBB
    std::vector<uint8_t> indices;    // w * h, de-interlaced
    LzwStats lzw;
};

struct File {
    int width = 0, height = 0;
    bool global_table = false;
    std::vector<uint32_t> global_palette;
    int background = 0;
    int loop_count = -1;             // -1: no NETSCAPE2.0 block
    std::vector<Frame> frames;
    bool trailer = false;
};

inline bool Parse(const uint8_t* d, size_t n, File& f, std::string* err = nullptr) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    f = File{};
    if (n < 13 || (std::string(reinterpret_cast<const char*>(d), 6) != "GIF89a" && std::string(reinterpret_cast<const char*>(d), 6) != "GIF87a"))
        return fail("bad signature");
    auto u16 = [&](size_t p) { return static_cast<int>(d[p] | (d[p + 1] << 8)); };
    f.width = u16(6);
    f.height = u16(8);
    const uint8_t packed = d[10];
    f.background = d[11];
    size_t p = 13;
    auto read_table = [&](int bits, std::vector<uint32_t>& pal) {
        const size_t count = static_cast<size_t>(1) << bits;
        if (p + count * 3 > n) return false;
        pal.resize(count);
        for (size_t i = 0; i < count; ++i, p += 3)
            pal[i] = (static_cast<uint32_t>(d[p]) << 16) | (static_cast<uint32_t>(d[p + 1]) << 8) | d[p + 2];
        return true;
    };
    if (packed & 0x80) {
        f.global_table = true;
        if (!read_table((packed & 7) + 1, f.global_palette)) return fail("truncated global table");
    }
    int delay = 0, disposal = 0, transparent = -1;
    auto skip_blocks = [&]() {
        for (;;) {
            if (p >= n) return false;
            const size_t len = d[p++];
            if (len == 0) return true;
            p += len;
        }
    };
    while (p < n) {
        const uint8_t tag = d[p++];
        if (tag == 0x3B) {
            f.trailer = true;
            return true;
        }
        if (tag == 0x21) {
            if (p >= n) return fail("truncated extension");
            const uint8_t label = d[p++];
            if (label == 0xF9) {
                if (p + 6 > n || d[p] != 4) return fail("bad GCE");
                const uint8_t gp = d[p + 1];
                disposal = (gp >> 2) & 7;
                delay = u16(p + 2);
                transparent = (gp & 1) ? d[p + 4] : -1;
                p += 5;
                if (!skip_blocks()) return fail("truncated GCE");
            } else if (label == 0xFF) {
                if (p + 12 <= n && d[p] == 11 && std::string(reinterpret_cast<const char*>(d + p + 1), 11) == "NETSCAPE2.0") {
                    p += 12;
                    if (p + 4 <= n && d[p] == 3 && d[p + 1] == 1) f.loop_count = u16(p + 2);
                }
                if (!skip_blocks()) return fail("truncated application extension");
            } else {
                if (!skip_blocks()) return fail("truncated extension");
            }
            continue;
        }
        if (tag != 0x2C) return fail("unknown block");
        if (p + 9 > n) return fail("truncated image descriptor");
        Frame fr;
        fr.x = u16(p);
        fr.y = u16(p + 2);
        fr.w = u16(p + 4);
        fr.h = u16(p + 6);
        const uint8_t ip = d[p + 8];
        p += 9;
        fr.interlaced = (ip & 0x40) != 0;
        if (ip & 0x80) {
            fr.local_table = true;
            if (!read_table((ip & 7) + 1, fr.palette)) return fail("truncated local table");
        } else {
            if (!f.global_table) return fail("image without a color table");
            fr.palette = f.global_palette;
        }
        if (p >= n) return fail("truncated image data");
        fr.min_code_size = d[p++];
        fr.delay_cs = delay;
        fr.disposal = disposal;
        fr.transparent = transparent;
        delay = 0;
        disposal = 0;
        transparent = -1;
        std::vector<uint8_t> raw;
        const size_t pixels = static_cast<size_t>(fr.w) * static_cast<size_t>(fr.h);
        if (!LzwDecode(d + p, n - p, fr.min_code_size, raw, &fr.lzw, pixels + 4096)) return fail("LZW decode failed");
        p += fr.lzw.consumed;
        if (raw.size() < pixels) return fail("not enough pixel data");
        raw.resize(pixels);
        for (uint8_t v : raw)
            if (v >= fr.palette.size()) return fail("index outside the color table");
        if (fr.interlaced) {
            fr.indices.resize(pixels);
            static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
            size_t src_row = 0;
            for (int pass = 0; pass < 4; ++pass)
                for (int y = start[pass]; y < fr.h; y += step[pass], ++src_row)
                    for (int x = 0; x < fr.w; ++x)
                        fr.indices[static_cast<size_t>(y) * fr.w + x] = raw[src_row * fr.w + x];
        } else {
            fr.indices = std::move(raw);
        }
        f.frames.push_back(std::move(fr));
    }
    return fail("missing trailer");
}

inline bool Parse(const std::vector<uint8_t>& data, File& f, std::string* err = nullptr) {
    return Parse(data.data(), data.size(), f, err);
}

// Canvas after each frame: 0xFFRRGGBB for painted pixels, 0 (alpha 0) where nothing was painted yet.
inline void Composite(const File& f, std::vector<std::vector<uint32_t>>& canvases) {
    canvases.clear();
    const size_t n = static_cast<size_t>(f.width) * static_cast<size_t>(f.height);
    std::vector<uint32_t> canvas(n, 0), saved;
    for (const Frame& fr : f.frames) {
        if (fr.disposal == 3) saved = canvas;
        for (int y = 0; y < fr.h; ++y) {
            const int cy = fr.y + y;
            if (cy >= f.height) break;
            for (int x = 0; x < fr.w; ++x) {
                const int cx = fr.x + x;
                if (cx >= f.width) break;
                const uint8_t v = fr.indices[static_cast<size_t>(y) * fr.w + x];
                if (v == fr.transparent) continue;
                canvas[static_cast<size_t>(cy) * f.width + cx] = 0xFF000000u | fr.palette[v];
            }
        }
        canvases.push_back(canvas);
        if (fr.disposal == 2) {
            for (int y = fr.y; y < fr.y + fr.h && y < f.height; ++y)
                for (int x = fr.x; x < fr.x + fr.w && x < f.width; ++x) canvas[static_cast<size_t>(y) * f.width + x] = 0;
        } else if (fr.disposal == 3) {
            canvas = saved;
        }
    }
}

} // namespace ct::test::gif
