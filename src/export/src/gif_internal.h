#pragma once
// gif_internal.h
// Parallel building blocks of GifEncoder (perf follow-up): a small fork-join worker set, banded quantization and
// segmented LZW. Results never depend on the number of threads: bands and segments are laid out from the image size
// alone, and each writes a disjoint part of the output.
// Thread model: Workers::Run from one thread at a time (the encoder's worker job); the callbacks run on the helper
// threads and on the caller.

#include <ct/export/gif_encoder.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace ct::exportng::gif_detail {

class Workers {
public:
    Workers();
    ~Workers();
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    // threads = total parallelism including the caller (1 = everything runs on the caller).
    void Start(int threads);
    void Stop();
    int Threads() const { return m_threads; }
    // Calls fn(ctx, i) for every i in [0, count) on the helpers and the caller; returns when all calls returned.
    void Run(int count, void (*fn)(void* ctx, int i), void* ctx);
    template <class F>
    void For(int count, F&& f) {
        using Fn = std::remove_reference_t<F>;
        Run(count, [](void* c, int i) { (*static_cast<Fn*>(c))(i); }, const_cast<void*>(static_cast<const void*>(&f)));
    }

    static int DefaultThreads();   // half the logical processors, 1..8

private:
    struct State;
    std::unique_ptr<State> m_s;
    int m_threads = 1;
};

// Quantization bands: rows [BandStart(h, k), BandStart(h, k + 1)) for k in [0, QuantBands(h)). Floyd-Steinberg error
// does not cross a band edge (each band starts with zero error, serpentine by absolute row parity).
int QuantBands(int h);
inline int BandStart(int h, int k) { return static_cast<int>(static_cast<int64_t>(h) * k / QuantBands(h)); }
// One band of GifEncoder::QuantizeRegion (same arguments; rows y0..y1 of the region). `cache` must be fully filled
// (NearestCache::FillAll) when bands run concurrently, so lookups only read it.
void QuantizeBand(const uint8_t* bgra, int w, int h, int stride, GifEncoder::NearestCache& cache, bool dither, const uint8_t* skip,
                  uint8_t skip_index, uint8_t* out, int y0, int y1);

// LZW of `n` indices in independent segments of `segment` symbols (the table restarts at each segment start, with a
// clear code written at the previous segment's final code width), encoded in parallel and stitched into one sub-block
// chain like GifEncoder::LzwEncode. A decoder sees an ordinary stream with extra clear codes.
void LzwEncodeSegmented(const uint8_t* indices, size_t n, int min_code_size, std::vector<uint8_t>& out, size_t segment,
                        Workers* workers);
constexpr size_t kLzwSegment = 128 * 1024;

} // namespace ct::exportng::gif_detail
