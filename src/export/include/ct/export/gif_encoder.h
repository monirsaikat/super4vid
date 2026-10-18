#pragma once
// ct/export/gif_encoder.h
// Animated GIF89a encoder (docs/phases/P10 section 10.4): median-cut palette on a 15-bit histogram, nearest-color
// mapping through a lazily filled 32x32x32 cache, serpentine Floyd-Steinberg dithering, frame differencing with a
// transparent index and dirty-rect crop, LZW with 255-byte sub-blocks, NETSCAPE2.0 looping.
//
// Pipelining: AddFrame copies the frame into one of two slots and returns; a single job on the ct::Async pool
// quantizes/compresses/writes the frames in order, so frame N is encoded while the caller renders frame N+1. When
// both slots are full AddFrame blocks until one is free. Close drains the queue and writes the trailer.
//
// Thread model: Open/AddPaletteSample/AddFrame/Close and the destructor from one producer thread. The static
// building blocks are pure and thread-safe. Lifetime: the destructor calls Close().

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace ct::exportng {

class GifEncoder {
public:
    struct Options {
        int max_colors = 256;            // 2..256 including the transparent slot when frame_diff
        bool dither = true;              // Floyd-Steinberg, serpentine, error clamped to [-32, 32]
        bool per_frame_palette = false;  // local color table per frame (no global table)
        bool frame_diff = true;          // unchanged pixels -> transparent index, dirty-rect crop, disposal 1
        int loop_count = 0;              // 0 = forever, n = repeat n times, < 0 = no NETSCAPE2.0 block (play once)
    };

    GifEncoder();
    ~GifEncoder();
    GifEncoder(const GifEncoder&) = delete;
    GifEncoder& operator=(const GifEncoder&) = delete;

    bool Open(std::string_view path, int width, int height, const Options& options);
    // Optional, before the first AddFrame: adds a frame's colors to the global palette histogram (the export job
    // samples every 30th frame first). Without samples the global palette comes from the first frame only. The first
    // frame is always added too. Ignored with per_frame_palette.
    void AddPaletteSample(const uint8_t* bgra, int stride);
    // bgra: width x height, B,G,R,A bytes (alpha ignored), rows `stride` bytes apart. delay_cs: 1/100 s, clamped to
    // [2, 65535]. Frames identical to the previous one extend its delay instead of adding a frame. Returns false after
    // any write error (the file is then incomplete).
    bool AddFrame(const uint8_t* bgra, int stride, int delay_cs);
    // Waits for queued frames, writes the trailer and closes the file. True when every byte was written.
    bool Close();

    bool IsOpen() const;
    uint64_t BytesWritten() const;   // valid after Close
    int FramesWritten() const;       // GIF frames emitted (merged duplicates count once), valid after Close
    double EncodeBusyMs() const;     // time the worker spent quantizing/compressing/writing, valid after Close

    // delay_cs for a frame rate: round(100 / fps), minimum 2.
    static int DelayForFps(int fps);

    // ---- building blocks (tested separately) ----
    // Palette entries are 0x00RRGGBB.
    struct Histogram {
        Histogram();
        void Clear();
        void Add(const uint8_t* bgra, int w, int h, int stride);
        void Add(uint8_t r, uint8_t g, uint8_t b) {
            const uint32_t k = (static_cast<uint32_t>(r >> 3) << 10) | (static_cast<uint32_t>(g >> 3) << 5) | (b >> 3);
            Bin& bin = bins[k];
            ++bin.count;
            bin.r += r;
            bin.g += g;
            bin.b += b;
        }
        struct Bin {
            uint64_t count = 0, r = 0, g = 0, b = 0;   // pixel count and channel sums (the box mean is exact)
        };
        std::vector<Bin> bins;   // 32768, index (r5 << 10) | (g5 << 5) | b5
    };
    // Median cut: repeatedly splits the box with the largest (widest channel range * pixel count) at the weighted
    // median of that channel until max_colors boxes (or no box can be split). Entry = count-weighted mean of the
    // box. When the image has <= max_colors distinct 15-bit colors every color gets its own exact entry.
    static void BuildPalette(const Histogram& hist, int max_colors, std::vector<uint32_t>& palette);
    static void BuildPalette(const uint8_t* bgra, int w, int h, int stride, int max_colors, std::vector<uint32_t>& palette);
    static void Quantize(const uint8_t* bgra, int w, int h, int stride, const std::vector<uint32_t>& palette, bool dither,
                         std::vector<uint8_t>& indices);
    // GIF LZW: clear code first, codes grow from min_code_size + 1 up to 12 bits, a clear code when the table is full,
    // EOI last. `out` receives the data sub-blocks (<= 255 bytes each, length-prefixed) including the 0 terminator
    // but not the leading min_code_size byte. Every index must be < (1 << min_code_size); min_code_size in [2, 8].
    static void LzwEncode(const uint8_t* indices, size_t n, int min_code_size, std::vector<uint8_t>& out);

    // Nearest palette entry per 15-bit color, filled on first use (exact squared-distance search over the palette
    // for the bucket centre). Public for tests and benchmarks.
    class NearestCache {
    public:
        void Reset(const uint32_t* palette, int count);
        uint8_t Lookup(int r, int g, int b) {
            const uint32_t k = (static_cast<uint32_t>(r >> 3) << 10) | (static_cast<uint32_t>(g >> 3) << 5) | static_cast<uint32_t>(b >> 3);
            const int16_t v = m_table[k];
            return v >= 0 ? static_cast<uint8_t>(v) : Fill(k);
        }
        const uint32_t* Palette() const { return m_palette.data(); }
        // Fills keys [begin, end) of the 32768 now (after that Lookup only reads them: safe from several threads).
        void FillRange(uint32_t begin, uint32_t end);
        void FillAll() { FillRange(0, 32768); }

    private:
        uint8_t Fill(uint32_t key);
        std::vector<uint32_t> m_palette;
        std::vector<int16_t> m_table;
    };
    // Quantizes a w x h region. skip (nullable, w*h bytes): pixels with skip != 0 get `skip_index` and neither
    // receive nor spread dither error. out: w*h indices. The region is dithered in horizontal bands laid out from h
    // alone (about 48 rows each), with no error carried across a band edge, so the encoder can quantize the bands in
    // parallel and still produce exactly this output.
    static void QuantizeRegion(const uint8_t* bgra, int w, int h, int stride, NearestCache& cache, bool dither,
                               const uint8_t* skip, uint8_t skip_index, uint8_t* out);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace ct::exportng
