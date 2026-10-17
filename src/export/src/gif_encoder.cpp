#define CT_LOG_CATEGORY "export"
#include <ct/export/gif_encoder.h>

#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/log.h>
#include <ct/core/sync.h>
#include <ct/core/time.h>
#include <ct/platform/win.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ct::exportng {

namespace {

constexpr size_t kFlushBytes = 1u << 20;


// Smallest b in [1, 8] with (1 << b) >= entries.
int TableBits(int entries) {
    int b = 1;
    while (b < 8 && (1 << b) < entries) ++b;
    return b;
}

} // namespace

struct GifEncoder::Impl {
    struct Slot {
        std::vector<uint8_t> pixels;   // tight BGRA
        int delay_cs = 0;
    };
    struct Frame {
        bool valid = false;
        int x = 0, y = 0, w = 0, h = 0;
        int delay_cs = 0;
        bool transparent = false;
        int transparent_index = 0;
        bool local_table = false;
        std::vector<uint32_t> local_palette;   // padded to 1 << table_bits
        int table_bits = 1;
        int min_code_size = 2;
        std::vector<uint8_t> indices;          // w * h, input of the LZW job
        std::vector<uint8_t> lzw;
        // LZW runs as its own Async job while the worker quantizes the next frame. 0 = queued, 1 = running,
        // 2 = done. Whoever wins 0 -> 1 encodes (the worker itself when the job has not started yet).
        std::atomic<int> lzw_state{2};
        Event lzw_done{true, true};
    };

    // ---- producer side / shared ----
    HANDLE file = INVALID_HANDLE_VALUE;
    int width = 0, height = 0;
    Options opt;
    Histogram sample_hist;
    bool have_samples = false;
    bool any_frame = false;
    Slot slots[2];
    int head = 0, count = 0;   // guarded by lock
    bool worker_running = false;
    SrwLock lock;
    Event slot_free{false, false};
    Event idle{true, true};
    std::atomic<bool> failed{false};
    std::atomic<int> alive{0};   // launched Work() calls that have not returned yet

    // ---- worker side ----
    std::vector<uint8_t> out;
    uint64_t bytes_written = 0;
    int frames_written = 0;
    double busy_ms = 0;
    bool header_written = false;
    bool first = true;
    std::vector<uint32_t> palette;   // real colors (without the transparent slot)
    int transparent_index = 0;
    int global_bits = 1;
    NearestCache cache;
    Histogram frame_hist;
    std::vector<uint8_t> prev_src, same, skip, idx, crop;
    std::vector<uint32_t> canvas;
    Frame pending;

    void Write(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + n);
        if (out.size() >= kFlushBytes) Flush();
    }
    void Byte(uint8_t b) { out.push_back(b); }
    void U16(int v) {
        Byte(static_cast<uint8_t>(v & 255));
        Byte(static_cast<uint8_t>((v >> 8) & 255));
    }
    void Table(const std::vector<uint32_t>& pal, int bits) {
        const int n = 1 << bits;
        for (int i = 0; i < n; ++i) {
            const uint32_t c = i < static_cast<int>(pal.size()) ? pal[i] : 0;
            Byte(static_cast<uint8_t>(c >> 16));
            Byte(static_cast<uint8_t>(c >> 8));
            Byte(static_cast<uint8_t>(c));
        }
    }
    void Flush() {
        if (out.empty() || file == INVALID_HANDLE_VALUE) return;
        size_t done = 0;
        while (done < out.size()) {
            DWORD wrote = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 30));
            if (!WriteFile(file, out.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
                CT_LOG_ERROR("gif: WriteFile failed (error %lu)", GetLastError());
                failed = true;
                break;
            }
            done += wrote;
        }
        bytes_written += done;
        out.clear();
    }

    void WriteHeader() {
        Write("GIF89a", 6);
        U16(width);
        U16(height);
        if (opt.per_frame_palette) {
            Byte(0x70);   // no global table, color resolution 8 bits
        } else {
            Byte(static_cast<uint8_t>(0x80 | 0x70 | (global_bits - 1)));
        }
        Byte(0);   // background index
        Byte(0);   // pixel aspect
        if (!opt.per_frame_palette) Table(palette, global_bits);
        if (opt.loop_count >= 0) {
            static const uint8_t app[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01};
            Write(app, sizeof(app));
            U16(std::min(opt.loop_count, 65535));
            Byte(0);
        }
        header_written = true;
    }

    void RunLzw() {
        int expected = 0;
        if (!pending.lzw_state.compare_exchange_strong(expected, 1)) return;
        pending.lzw.clear();
        LzwEncode(pending.indices.data(), pending.indices.size(), pending.min_code_size, pending.lzw);
        pending.lzw_state = 2;
        pending.lzw_done.Set();
    }

    void WaitLzw() {
        RunLzw();   // inline when the job has not started (also avoids a deadlock on a 1-thread pool)
        while (pending.lzw_state.load() != 2) pending.lzw_done.Wait();
    }

    void FlushPending() {
        if (!pending.valid) return;
        WaitLzw();
        const Frame& f = pending;
        Byte(0x21);
        Byte(0xF9);
        Byte(4);
        Byte(static_cast<uint8_t>((1 << 2) | (f.transparent ? 1 : 0)));   // disposal 1 (leave in place)
        U16(f.delay_cs);
        Byte(static_cast<uint8_t>(f.transparent ? f.transparent_index : 0));
        Byte(0);
        Byte(0x2C);
        U16(f.x);
        U16(f.y);
        U16(f.w);
        U16(f.h);
        Byte(static_cast<uint8_t>(f.local_table ? (0x80 | (f.table_bits - 1)) : 0));
        if (f.local_table) Table(f.local_palette, f.table_bits);
        Byte(static_cast<uint8_t>(f.min_code_size));
        Write(f.lzw.data(), f.lzw.size());
        ++frames_written;
        pending.valid = false;
    }

    // Sets `palette`, `transparent_index`, the table size and the cache from a histogram.
    void SetPalette(const Histogram& hist) {
        const int reserve = opt.frame_diff ? 1 : 0;
        BuildPalette(hist, std::max(1, opt.max_colors - reserve), palette);
        transparent_index = static_cast<int>(palette.size());
        cache.Reset(palette.data(), static_cast<int>(palette.size()));
    }

    void EmitFrame(int x, int y, int w, int h, const uint8_t* indices, int delay_cs, bool transparent) {
        FlushPending();
        Frame& f = pending;
        f.valid = true;
        f.x = x;
        f.y = y;
        f.w = w;
        f.h = h;
        f.delay_cs = delay_cs;
        f.transparent = transparent;
        f.transparent_index = transparent_index;
        f.local_table = opt.per_frame_palette;
        const int entries = static_cast<int>(palette.size()) + (opt.frame_diff ? 1 : 0);
        f.table_bits = opt.per_frame_palette ? TableBits(entries) : global_bits;
        if (f.local_table) f.local_palette = palette;
        f.min_code_size = std::max(2, f.table_bits);
        f.indices.assign(indices, indices + static_cast<size_t>(w) * static_cast<size_t>(h));
        f.lzw_done.Reset();
        f.lzw_state = 0;   // publishes the fields above to the job
        alive.fetch_add(1);
        Async::Run([this] {
            RunLzw();
            alive.fetch_sub(1);   // last access to *this
        });
    }

    void ExtendDelay(int delay_cs) {
        if (pending.valid && pending.delay_cs + delay_cs <= 65535) {
            pending.delay_cs += delay_cs;
            return;
        }
        // The delay field is 16 bits: continue with a 1x1 fully transparent frame.
        if (opt.per_frame_palette) {
            palette.assign(1, 0);
            transparent_index = 1;
        }
        const uint8_t t = static_cast<uint8_t>(transparent_index);
        EmitFrame(0, 0, 1, 1, &t, delay_cs, true);
    }

    void ProcessFrame(const uint8_t* src, int delay_cs) {
        const int stride = width * 4;
        const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (first) {
            first = false;
            prev_src.assign(src, src + pixels * 4);
            canvas.assign(pixels, 0);
            idx.resize(pixels);
            if (opt.per_frame_palette) {
                frame_hist.Clear();
                frame_hist.Add(src, width, height, stride);
                SetPalette(frame_hist);
            } else {
                sample_hist.Add(src, width, height, stride);
                SetPalette(sample_hist);
                global_bits = TableBits(static_cast<int>(palette.size()) + (opt.frame_diff ? 1 : 0));
            }
            WriteHeader();
            QuantizeRegion(src, width, height, stride, cache, opt.dither, nullptr, 0, idx.data());
            for (size_t i = 0; i < pixels; ++i) canvas[i] = palette[idx[i]];
            EmitFrame(0, 0, width, height, idx.data(), delay_cs, false);
            return;
        }

        int x0 = 0, y0 = 0, x1 = width, y1 = height;   // dirty rect [x0, x1) x [y0, y1)
        if (opt.frame_diff) {
            // One pass: unchanged-pixel mask for the whole frame (1 = same RGB as last time) and its bounding box.
            x0 = width;
            y0 = height;
            x1 = 0;
            y1 = 0;
            same.resize(pixels);
            for (int y = 0; y < height; ++y) {
                const uint8_t* a = src + static_cast<size_t>(y) * stride;
                const uint8_t* b = prev_src.data() + static_cast<size_t>(y) * stride;
                uint8_t* mrow = same.data() + static_cast<size_t>(y) * width;
                if (std::memcmp(a, b, static_cast<size_t>(stride)) == 0) {
                    std::memset(mrow, 1, static_cast<size_t>(width));
                    continue;
                }
                const uint32_t* pa = reinterpret_cast<const uint32_t*>(a);
                const uint32_t* pb = reinterpret_cast<const uint32_t*>(b);
                int first_x = width, last_x = -1;
                for (int x = 0; x < width; ++x) {
                    const bool eq = ((pa[x] ^ pb[x]) & 0x00FFFFFFu) == 0;
                    mrow[x] = eq ? 1 : 0;
                    if (!eq) {
                        if (first_x == width) first_x = x;
                        last_x = x;
                    }
                }
                if (last_x < 0) continue;   // only alpha changed
                x0 = std::min(x0, first_x);
                x1 = std::max(x1, last_x + 1);
                y0 = std::min(y0, y);
                y1 = y + 1;
            }
            if (x1 <= x0) {
                ExtendDelay(delay_cs);
                return;
            }
        }
        const int rw = x1 - x0, rh = y1 - y0;
        const size_t rn = static_cast<size_t>(rw) * static_cast<size_t>(rh);
        const uint8_t* rsrc = src + (static_cast<size_t>(y0) * width + x0) * 4;
        const uint8_t* skip_ptr = nullptr;
        if (opt.frame_diff) {
            skip.resize(rn);
            for (int y = 0; y < rh; ++y)
                std::memcpy(skip.data() + static_cast<size_t>(y) * rw, same.data() + static_cast<size_t>(y0 + y) * width + x0,
                            static_cast<size_t>(rw));
            skip_ptr = skip.data();
        }
        if (opt.per_frame_palette) {
            frame_hist.Clear();
            for (int y = 0; y < rh; ++y) {
                const uint8_t* a = rsrc + static_cast<size_t>(y) * stride;
                const uint8_t* s = skip_ptr ? skip_ptr + static_cast<size_t>(y) * rw : nullptr;
                for (int x = 0; x < rw; ++x)
                    if (!s || !s[x]) frame_hist.Add(a[x * 4 + 2], a[x * 4 + 1], a[x * 4]);
            }
            SetPalette(frame_hist);
        }
        const uint8_t t = static_cast<uint8_t>(transparent_index);
        QuantizeRegion(rsrc, rw, rh, stride, cache, opt.dither, skip_ptr, t, idx.data());

        // Pixels whose new color equals what is already displayed become transparent; tighten the rect.
        int tx0 = rw, ty0 = rh, tx1 = 0, ty1 = 0;
        for (int y = 0; y < rh; ++y) {
            uint8_t* o = idx.data() + static_cast<size_t>(y) * rw;
            uint32_t* c = canvas.data() + static_cast<size_t>(y0 + y) * width + x0;
            for (int x = 0; x < rw; ++x) {
                if (opt.frame_diff) {
                    if (o[x] == t) continue;
                    const uint32_t color = palette[o[x]];
                    if (color == c[x]) {
                        o[x] = t;
                        continue;
                    }
                    c[x] = color;
                } else {
                    c[x] = palette[o[x]];
                }
                tx0 = std::min(tx0, x);
                tx1 = std::max(tx1, x + 1);
                ty0 = std::min(ty0, y);
                ty1 = y + 1;
            }
            std::memcpy(prev_src.data() + (static_cast<size_t>(y0 + y) * width + x0) * 4, rsrc + static_cast<size_t>(y) * stride,
                        static_cast<size_t>(rw) * 4);
        }
        if (!opt.frame_diff) {
            EmitFrame(0, 0, width, height, idx.data(), delay_cs, false);
            return;
        }
        if (tx1 <= tx0) {
            ExtendDelay(delay_cs);
            return;
        }
        const int cw = tx1 - tx0, ch = ty1 - ty0;
        const uint8_t* emit = idx.data();
        if (cw != rw || ch != rh) {
            crop.resize(static_cast<size_t>(cw) * ch);
            for (int y = 0; y < ch; ++y)
                std::memcpy(crop.data() + static_cast<size_t>(y) * cw, idx.data() + static_cast<size_t>(ty0 + y) * rw + tx0,
                            static_cast<size_t>(cw));
            emit = crop.data();
        }
        EmitFrame(x0 + tx0, y0 + ty0, cw, ch, emit, delay_cs, true);
    }

    // Runs on the Async pool; exactly one instance at a time (worker_running).
    void Work() {
        for (;;) {
            lock.LockExclusive();
            if (count == 0) {
                worker_running = false;
                lock.UnlockExclusive();
                idle.Set();
                alive.fetch_sub(1);   // last access to *this: Close may destroy the encoder right after
                return;
            }
            Slot& s = slots[head];
            lock.UnlockExclusive();
            if (!failed) {
                Stopwatch sw;
                sw.Start();
                ProcessFrame(s.pixels.data(), s.delay_cs);
                if (out.size() >= kFlushBytes) Flush();
                busy_ms += sw.ElapsedMs();
            }
            lock.LockExclusive();
            head = (head + 1) % 2;
            --count;
            lock.UnlockExclusive();
            slot_free.Set();
        }
    }

    // Producer thread only. A worker that finished just after AddFrame launched its successor may leave `idle`
    // signaled while the successor runs, hence the re-check.
    void WaitIdle() {
        for (;;) {
            idle.Wait();
            lock.LockExclusive();
            const bool busy = worker_running || count > 0;
            lock.UnlockExclusive();
            if (!busy) break;
            Sleep(1);
        }
        while (alive.load() != 0) Sleep(0);
    }
};

GifEncoder::GifEncoder() = default;

GifEncoder::~GifEncoder() { Close(); }

bool GifEncoder::IsOpen() const { return m && m->file != INVALID_HANDLE_VALUE; }
uint64_t GifEncoder::BytesWritten() const { return m ? m->bytes_written : 0; }
int GifEncoder::FramesWritten() const { return m ? m->frames_written : 0; }
double GifEncoder::EncodeBusyMs() const { return m ? m->busy_ms : 0; }

int GifEncoder::DelayForFps(int fps) {
    if (fps <= 0) return 10;
    return std::max(2, static_cast<int>(std::lround(100.0 / fps)));
}

bool GifEncoder::Open(std::string_view path, int width, int height, const Options& options) {
    Close();
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        CT_LOG_ERROR("gif: invalid size %dx%d", width, height);
        return false;
    }
    auto impl = std::make_unique<Impl>();
    impl->width = width;
    impl->height = height;
    impl->opt = options;
    impl->opt.max_colors = std::clamp(options.max_colors, 2, 256);
    const std::wstring wpath = fs::ToWinPath(path);
    impl->file = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl->file == INVALID_HANDLE_VALUE) {
        CT_LOG_ERROR("gif: cannot create %.*s (error %lu)", static_cast<int>(path.size()), path.data(), GetLastError());
        return false;
    }
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    for (auto& s : impl->slots) s.pixels.resize(bytes);
    m = std::move(impl);
    return true;
}

void GifEncoder::AddPaletteSample(const uint8_t* bgra, int stride) {
    if (!IsOpen() || m->any_frame || m->opt.per_frame_palette || !bgra) return;
    m->sample_hist.Add(bgra, m->width, m->height, stride);
    m->have_samples = true;
}

bool GifEncoder::AddFrame(const uint8_t* bgra, int stride, int delay_cs) {
    if (!IsOpen() || !bgra || m->failed) return false;
    Impl& d = *m;
    for (;;) {
        d.lock.LockExclusive();
        const int c = d.count;
        d.lock.UnlockExclusive();
        if (c < 2) break;
        d.slot_free.Wait();
    }
    // Single producer: the tail slot is not touched by the worker until count is incremented below.
    d.lock.LockExclusive();
    const int tail = (d.head + d.count) % 2;
    d.lock.UnlockExclusive();
    Impl::Slot& s = d.slots[tail];
    const size_t row = static_cast<size_t>(d.width) * 4;
    for (int y = 0; y < d.height; ++y)
        std::memcpy(s.pixels.data() + y * row, bgra + static_cast<ptrdiff_t>(y) * stride, row);
    s.delay_cs = std::clamp(delay_cs, 2, 65535);
    d.any_frame = true;
    bool launch = false;
    d.lock.LockExclusive();
    ++d.count;
    if (!d.worker_running) {
        d.worker_running = true;
        d.idle.Reset();
        launch = true;
    }
    d.lock.UnlockExclusive();
    if (launch) {
        Impl* p = m.get();
        d.alive.fetch_add(1);
        Async::Run([p] { p->Work(); });
    }
    return !d.failed;
}

bool GifEncoder::Close() {
    if (!IsOpen()) return true;
    Impl& d = *m;
    d.WaitIdle();
    if (!d.failed) {
        if (!d.header_written) {   // no frames: a valid empty GIF
            d.palette.assign(1, 0);
            d.global_bits = 1;
            const bool per_frame = d.opt.per_frame_palette;
            d.opt.per_frame_palette = false;
            d.WriteHeader();
            d.opt.per_frame_palette = per_frame;
        }
        d.FlushPending();
        d.Byte(0x3B);
        d.Flush();
    }
    const bool ok = !d.failed;
    CloseHandle(d.file);
    d.file = INVALID_HANDLE_VALUE;
    return ok;
}

} // namespace ct::exportng
