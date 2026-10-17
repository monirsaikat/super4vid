#include <ct/playback/cache_budget.h>
#include <ct/playback/media_cache.h>

#include "../common/frame_index.h"
#include "../common/nv12_readback.h"
#include "../common/rgba_readback.h"
#include "../common/test_device.h"
#include "../common/test_media.h"

#include <ct/core/fs.h>
#include <ct/core/time.h>
#include <ct/media/image_io.h>
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace ct;
using namespace ct::playback;

namespace {

#ifdef CT_DEBUG
constexpr double kThumbBudgetMs = 2000.0;   // Debug + debug layer under parallel builds; the 1 s budget is Release
#else
constexpr double kThumbBudgetMs = 1000.0;
#endif

int64_t IndexOf(platform::D3DDevice& dev, const FrameRef& f) {
    test::Nv12Image img;
    if (!test::ReadbackNv12(dev, f.nv12, img)) return -2;
    return test::ReadFrameIndexY(img.y.data(), img.width, img.height, img.width);
}

// 48 kHz stereo 16-bit PCM WAV with a 440 Hz tone (decodes fast but long enough for a slow peaks build).
bool WriteToneWav(const std::string& path, int seconds) {
    const uint32_t frames = static_cast<uint32_t>(seconds) * 48000;
    const uint32_t data_bytes = frames * 4;
    std::vector<uint8_t> hdr(44);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&hdr[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&hdr[at], &v, 2); };
    std::memcpy(&hdr[0], "RIFF", 4);
    put32(4, 36 + data_bytes);
    std::memcpy(&hdr[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 2);
    put32(24, 48000);
    put32(28, 48000 * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(&hdr[36], "data", 4);
    put32(40, data_bytes);
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, hdr.data(), 44, &w, nullptr) != 0;
    std::vector<int16_t> chunk(48000 * 2);
    for (uint32_t i = 0; i < 48000; ++i)
        chunk[2 * i] = chunk[2 * i + 1] = static_cast<int16_t>(8000 * std::sin(2 * 3.14159265358979 * 440 * i / 48000.0));
    for (int s = 0; s < seconds && ok; ++s) ok = WriteFile(f, chunk.data(), static_cast<DWORD>(chunk.size() * 2), &w, nullptr) != 0;
    CloseHandle(f);
    return ok;
}

} // namespace

// Review fix: re-returning a pinned slot refreshes it (true LRU of the last 4 returned). With A,B,A,C,A,D,A,E the
// frame returned last but one (A) must stay pinned; the old round-robin pinned A once and unpinned it at E.
CT_TEST(media_cache_pins_are_lru) {
    uint32_t pins[MediaCache::kPinned] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const uint32_t A = 10, B = 11, C = 12, D = 13, E = 14;
    for (uint32_t s : {A, B, A, C, A, D, A, E}) MediaCache::PinRecent(pins, s);
    auto pinned = [&](uint32_t s) {
        for (uint32_t p : pins)
            if (p == s) return true;
        return false;
    };
    CT_CHECK(pinned(A));
    CT_CHECK(pinned(C));
    CT_CHECK(pinned(D));
    CT_CHECK(pinned(E));
    CT_CHECK(!pinned(B));
    CT_CHECK_EQ(pins[0], C);   // least recent first
    CT_CHECK_EQ(pins[1], D);
    CT_CHECK_EQ(pins[2], A);
    CT_CHECK_EQ(pins[3], E);
    MediaCache::PinRecent(pins, E);   // repeat of the newest: no change
    CT_CHECK_EQ(pins[0], C);
    CT_CHECK_EQ(pins[3], E);
}

CT_TEST_DEVICE(media_cache_exact_frame_after_hint) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    MediaCache cache;
    CT_REQUIRE(SUCCEEDED(cache.Open(*dev, path)));
    CT_CHECK(cache.HasVideo());
    CT_CHECK_EQ(cache.FpsNum() / cache.FpsDen(), 60);
    FrameRef f;
    bool exact = false;
    // Non-blocking: a cold request returns nothing (or an earlier frame) and starts the prefetch.
    Stopwatch sw;
    sw.Start();
    cache.GetVideoFrame(2'500'000, f, exact);
    CT_REQUIRE(cache.WaitForFrame(2'500'000, 3000));
    const double ms = sw.ElapsedMs();
    CT_REQUIRE(cache.GetVideoFrame(2'500'000, f, exact));
    CT_CHECK(exact);
    CT_CHECK(f.y != nullptr && f.uv != nullptr && f.nv12 != nullptr);
    CT_CHECK_EQ(f.size.w, 1920);
    CT_CHECK_EQ(IndexOf(*dev, f), 150);
    std::printf("    cold exact frame after %.1f ms (cache stat %.1f ms)\n", ms, cache.GetStats().last_seek_to_frame_ms);
    // Neighbours inside the paused window [t - 0.25 s, t + 0.5 s] arrive too.
    CT_REQUIRE(cache.WaitForFrame(FrameTimeUs(170, 60, 1), 3000));
    CT_REQUIRE(cache.GetVideoFrame(FrameTimeUs(170, 60, 1) + 5, f, exact));
    CT_CHECK(exact);
    CT_CHECK_EQ(IndexOf(*dev, f), 170);
    CT_REQUIRE(cache.WaitForFrame(FrameTimeUs(140, 60, 1), 3000));
    CT_REQUIRE(cache.GetVideoFrame(FrameTimeUs(140, 60, 1), f, exact));
    CT_CHECK_EQ(IndexOf(*dev, f), 140);
    // A miss far away returns the nearest earlier frame with exact = false.
    const bool got = cache.GetVideoFrame(4'900'000, f, exact);
    CT_CHECK(!exact);
    if (got) CT_CHECK(f.pts_us < 4'900'000);
    CT_REQUIRE(cache.WaitForFrame(4'900'000, 3000));
    CT_REQUIRE(cache.GetVideoFrame(4'900'000, f, exact));
    CT_CHECK_EQ(IndexOf(*dev, f), 294);
    const MediaCache::Stats st = cache.GetStats();
    CT_CHECK(st.seeks >= 2);
    CT_CHECK(st.cache_bytes <= CacheBudget::Bytes());
    cache.Close();
    CT_CHECK_EQ(CacheBudget::Used(), 0u);
}

CT_TEST_DEVICE(media_cache_budget_and_playback_window) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    const size_t old = CacheBudget::Bytes();
    const size_t frame = 1920 * 1080 * 3 / 2;
    CacheBudget::SetBytes(frame * 20);   // 20 frames
    {
        MediaCache cache;
        CT_REQUIRE(SUCCEEDED(cache.Open(*dev, path)));
        // Simulated playback at 60 fps, 1 frame per "tick", consuming in order.
        int shown = 0, late = 0;
        int64_t last_index = -1;
        Stopwatch sw;
        sw.Start();
        for (int n = 0; n < 240; ++n) {
            const TimeUs t = FrameTimeUs(n, 60, 1) + 100;
            cache.Hint(t, 1.0, true);
            if (n == 0) CT_REQUIRE(cache.WaitForFrame(t, 3000));
            FrameRef f;
            bool exact = false;
            // Wait up to one frame period for the exact frame, like a vsync'ed UI.
            Stopwatch tick;
            tick.Start();
            while (!(cache.GetVideoFrame(t, f, exact) && exact) && tick.ElapsedMs() < 16.0) Sleep(1);
            if (!exact) ++late;
            ++shown;
            CT_CHECK(CacheBudget::Used() <= frame * 20);
            if (n % 37 == 0 && exact) {
                const int64_t idx = IndexOf(*dev, f);
                CT_CHECK_EQ(idx, n);
                CT_CHECK(idx > last_index);
                last_index = idx;
            }
        }
        const MediaCache::Stats st = cache.GetStats();
        std::printf("    240 ticks in %.0f ms: %d late, %llu decoded, %llu evictions, %zu cached, %llu seeks\n", sw.ElapsedMs(), late,
                    static_cast<unsigned long long>(st.frames_decoded), static_cast<unsigned long long>(st.evictions),
                    st.cached_frames, static_cast<unsigned long long>(st.seeks));
        CT_CHECK(st.evictions > 0);
        CT_CHECK(st.cached_frames <= 20u);
#ifndef CT_DEBUG
        CT_CHECK(late <= 2);   // real-time: enforced in Release only (Debug + debug layer is report-only)
#endif
    }
    CacheBudget::SetBytes(old);
    CT_CHECK_EQ(CacheBudget::Used(), 0u);
}

CT_TEST_DEVICE(media_cache_thumbnail_within_1s) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    MediaCache cache;
    CT_REQUIRE(SUCCEEDED(cache.Open(*dev, path)));
    ID3D11ShaderResourceView* srv = nullptr;
    CT_CHECK(!cache.Thumbnail(2'500'000, srv));
    Stopwatch sw;
    sw.Start();
    while (!cache.Thumbnail(2'500'000, srv) && sw.ElapsedMs() < 3000) Sleep(5);
    const double ms = sw.ElapsedMs();
    std::printf("    thumbnail ready after %.0f ms\n", ms);
    CT_REQUIRE(srv != nullptr);
    CT_CHECK(ms < kThumbBudgetMs);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    CT_REQUIRE(test::ReadbackRgba8(*dev, test::TextureOf(srv, tex), px, w, h));
    CT_CHECK_EQ(w, 160);
    CT_CHECK_EQ(h, 90);
    // Colour bars: the thumbnail is not flat black.
    int bright = 0;
    for (size_t i = 0; i < px.size(); i += 4) bright += (px[i] + px[i + 1] + px[i + 2]) > 150;
    CT_CHECK(bright > 160 * 90 / 4);
    // The same bucket is served from the cache; the next one is decoded forward without a seek.
    CT_CHECK(cache.Thumbnail(2'900'000, srv));
    sw.Start();
    while (!cache.Thumbnail(3'000'000, srv) && sw.ElapsedMs() < 3000) Sleep(5);
    const double next_ms = sw.ElapsedMs();
    CT_CHECK(next_ms < kThumbBudgetMs);
    sw.Start();
    while (!cache.Thumbnail(4'500'000, srv) && sw.ElapsedMs() < 3000) Sleep(5);   // another GOP: seek + 30 frames
    std::printf("    next bucket %.0f ms, other GOP %.0f ms\n", next_ms, sw.ElapsedMs());
    CT_CHECK(sw.ElapsedMs() < kThumbBudgetMs);
}

CT_TEST_DEVICE(media_cache_peaks_and_audio) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    MediaCache cache;
    CT_REQUIRE(SUCCEEDED(cache.Open(*dev, path)));
    fs::Delete(media::Peaks::CachePath(path, cache.Info().AudioStreams()[0]));
    const media::Peaks* p = cache.GetPeaks(-1);
    Stopwatch sw;
    sw.Start();
    while (!(p = cache.GetPeaks(-1)) && sw.ElapsedMs() < 10000) Sleep(10);
    CT_REQUIRE(p != nullptr);
    CT_CHECK(p->min.size() >= 500 && p->min.size() <= 502);
    std::printf("    peaks after %.0f ms\n", sw.ElapsedMs());
    auto reader = cache.OpenAudio(-1);
    CT_REQUIRE(reader != nullptr);
    CT_CHECK_HR(reader->Seek(1'000'000));
    std::vector<float> buf(4800 * 2);
    TimeUs pts = 0;
    CT_CHECK_EQ(reader->Read(buf.data(), 4800, pts), 4800u);
    CT_CHECK_NEAR(pts, 1'000'000, 21);
}

CT_TEST_DEVICE(media_cache_image_and_audio_only) {
    CT_REQUIRE_DEVICE(dev);
    const std::string png = fs::TempPath(".png");
    std::vector<uint8_t> bgra(32 * 16 * 4, 255);
    CT_REQUIRE(SUCCEEDED(media::ImageIo::SavePng(png, bgra.data(), 32, 16, 32 * 4)));
    {
        MediaCache cache;
        CT_REQUIRE(SUCCEEDED(cache.Open(*dev, png)));
        FrameRef f;
        bool exact = false;
        CT_REQUIRE(cache.GetVideoFrame(123'456'789, f, exact));
        CT_CHECK(exact);
        CT_CHECK(f.rgba != nullptr);
        CT_CHECK(f.nv12 == nullptr);
        CT_CHECK_EQ(f.size.w, 32);
        ID3D11ShaderResourceView* srv = nullptr;
        CT_CHECK(cache.Thumbnail(0, srv));
    }
    fs::Delete(png);
    const char* wav = "C:\\Windows\\Media\\Windows Notify System Generic.wav";
    if (!test::FileExistsUtf8(wav)) CT_SKIP("system sound missing");
    MediaCache cache;
    CT_REQUIRE(SUCCEEDED(cache.Open(*dev, wav)));
    CT_CHECK(!cache.HasVideo());
    FrameRef f;
    bool exact = false;
    CT_CHECK(!cache.GetVideoFrame(0, f, exact));
    CT_CHECK(cache.OpenAudio(-1) != nullptr);
}

// Review fix: Peaks::Build is cancellable and MediaCache::Close cancels a running build instead of waiting for the full
// decode pass of a long file.
CT_TEST_DEVICE(media_cache_close_cancels_peaks_build) {
    CT_REQUIRE_DEVICE(dev);
    const std::string wav = fs::TempPath(".wav");
    CT_REQUIRE(WriteToneWav(wav, 600));   // 10 min, 115 MB
    const bool strict = std::getenv("CT_STRICT_TIMING") != nullptr;
    // A stop flag that is already set: Build returns E_ABORT without decoding everything.
    {
        std::atomic<bool> stop{true};
        media::Peaks p;
        CT_CHECK_EQ(media::Peaks::Build(wav, -1, p, &stop), E_ABORT);
        CT_CHECK(p.min.empty());
    }
    // Full build time, for reference (non-strict runs compare against it).
    Stopwatch sw;
    sw.Start();
    {
        media::Peaks p;
        CT_REQUIRE(SUCCEEDED(media::Peaks::Build(wav, -1, p)));
        CT_CHECK(p.min.size() >= 60'000);
    }
    const double full_ms = sw.ElapsedMs();
    double close_ms = 0;
    {
        MediaCache cache;
        CT_REQUIRE(SUCCEEDED(cache.Open(*dev, wav)));
        CT_REQUIRE(!cache.Info().AudioStreams().empty());
        fs::Delete(media::Peaks::CachePath(wav, cache.Info().AudioStreams()[0]));
        CT_CHECK(cache.GetPeaks(-1) == nullptr);   // starts the build on the IO pool
        Sleep(static_cast<DWORD>(std::min(200.0, full_ms / 4)));   // let it get into the decode loop
        sw.Start();
        cache.Close();
        close_ms = sw.ElapsedMs();
        CT_CHECK(cache.GetPeaks(-1) == nullptr);
    }
    std::printf("    full peaks build %.0f ms, Close during the build %.1f ms\n", full_ms, close_ms);
    if (strict) CT_CHECK(close_ms < 100.0);
    else CT_CHECK(close_ms < full_ms / 2);
    fs::Delete(wav);
}
