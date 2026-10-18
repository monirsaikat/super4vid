// ct/capture/screen_capturer.h robustness (P11 task 11.4): access lost (secure desktop, fullscreen game, mode change),
// DXGI_ERROR_INVALID_CALL, an unplugged display, and a whole-display recording whose desktop no longer matches the
// stream size (mode change). The DXGI failures are injected into the real duplicator (DesktopDuplicator test hooks);
// the mode change is simulated with a display rect that differs from the real desktop.
#include "test_util.h"

#include <ct/capture/color_convert.h>
#include <ct/capture/screen_capturer.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;

namespace {

struct Sink : FrameSink {
    platform::TexturePool* pool = nullptr;
    std::vector<TimeUs> times;   // reserved up front; only the capture thread writes
    std::atomic<int> errors{0};
    std::atomic<HRESULT> last_error{S_OK};
    // Keeps the frame delivered at this count (no Release) for a readback after Stop.
    int keep_at = -1;
    std::atomic<int> kept_index{-1};
    bool OnFrame(ID3D11Texture2D*, uint32_t index, TimeUs t_us) override {
        if (times.size() < times.capacity()) times.push_back(t_us);
        if (static_cast<int>(times.size()) == keep_at && kept_index.load() < 0) {
            kept_index.store(static_cast<int>(index));
            return true;
        }
        pool->Release(index);
        return true;
    }
    void OnError(HRESULT hr) override {
        last_error.store(hr);
        errors.fetch_add(1);
    }
};

bool PrimaryDisplay(D3DDevice& dev, DisplayInfo& out) {
    std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    if (displays.empty()) return false;
    out = displays[0];
    return true;
}

void PrintStats(const char* what, const CaptureStats& s, size_t frames) {
    std::printf("  %s: ticks %llu new %llu repeats %llu frames %zu | access_lost %llu reinits %llu | drops pool %llu sink %llu late %llu | fit %llu\n",
                what, static_cast<unsigned long long>(s.ticks.load()), static_cast<unsigned long long>(s.new_frames.load()),
                static_cast<unsigned long long>(s.repeats.load()), frames, static_cast<unsigned long long>(s.access_lost.load()),
                static_cast<unsigned long long>(s.reinits.load()), static_cast<unsigned long long>(s.dropped_pool_empty.load()),
                static_cast<unsigned long long>(s.dropped_sink.load()), static_cast<unsigned long long>(s.late_ticks.load()),
                static_cast<unsigned long long>(s.fit_frames.load()));
}

// Every tick after the first frame delivered exactly one frame, tick-aligned and consecutive (CFR, no holes).
void CheckContinuous(const Sink& sink, const CaptureStats& s, int fps) {
    CT_REQUIRE(!sink.times.empty());
    if (s.late_ticks.load() != 0) return;   // a starved machine skipped ticks: those are reported, not holes we made
    const int64_t n0 = FrameIndexAt(sink.times.front(), fps, 1);
    for (size_t i = 0; i < sink.times.size(); ++i) {
        if (sink.times[i] != FrameTimeUs(n0 + static_cast<int64_t>(i), fps, 1)) {
            CT_FAIL("frame timestamps are not consecutive ticks");
            return;
        }
    }
}

} // namespace

// Access lost (what a UAC prompt, Ctrl+Alt+Del, a fullscreen game or a mode switch causes): the capturer keeps
// sending the last frame on every tick, re-creates the duplication, and new images flow again. Also with
// DXGI_ERROR_INVALID_CALL, which some drivers return after a mode change, and with a secure desktop that refuses the
// new duplication (E_ACCESSDENIED) for 1.5 s: still no error, no hole, and it recovers.
CT_TEST_DEVICE(capture_access_lost_repeats_and_recovers) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    DisplayInfo disp;
    CT_REQUIRE(PrimaryDisplay(dev, disp));
    struct Case { const char* name; HRESULT acquire_hr; HRESULT reinit_hr; uint32_t reinit_failures; };
    const Case cases[] = {
        {"access lost", DXGI_ERROR_ACCESS_LOST, S_OK, 0},
        {"invalid call", DXGI_ERROR_INVALID_CALL, S_OK, 0},
        {"secure desktop 1.5 s", DXGI_ERROR_ACCESS_LOST, E_ACCESSDENIED, 6},
    };
    for (const Case& c : cases) {
        ScreenCapturer cap;
        ScreenCapturer::Config cfg;
        cfg.display = disp;
        cfg.region = RectI{disp.rect.x, disp.rect.y, 640, 360};
        cfg.fps_num = 30;
        CT_REQUIRE(SUCCEEDED(cap.Init(dev, cfg)));
        Sink sink;
        sink.pool = &cap.Nv12Pool();
        sink.times.reserve(400);
        test_util::SolidWindow win(disp.rect.x + 10, disp.rect.y + 10, 200, 150, RGB(30, 160, 60));
        CT_REQUIRE(cap.Start(Clock::QpcNow(), sink));
        Sleep(400);
        const uint64_t new_before = cap.Stats().new_frames.load();
        cap.DuplicatorForTest().InjectReinitErrorForTest(c.reinit_hr, c.reinit_failures);
        cap.DuplicatorForTest().InjectAcquireErrorForTest(c.acquire_hr, 1);
        // While lost: the capturer reports it, and only repeats go out.
        Sleep(c.reinit_failures ? 900 : 150);
        const int64_t lost_since = cap.Stats().lost_since_qpc.load();
        const uint64_t new_while_lost = cap.Stats().new_frames.load();
        Sleep(c.reinit_failures ? 1200 : 300);
        // After recovery a fresh duplication delivers the current desktop: change it so a new image is certain.
        win.SetColor(RGB(200, 40, 40));
        win.Pump();
        Sleep(500);
        win.Pump();
        cap.Stop();
        const CaptureStats& s = cap.Stats();
        PrintStats(c.name, s, sink.times.size());
        CT_CHECK_EQ(sink.errors.load(), 0);
        CT_CHECK(s.access_lost.load() >= 1);
        CT_CHECK(s.reinits.load() >= 1);
        if (c.reinit_failures) {
            CT_CHECK(lost_since != 0);                     // still lost 0.9 s in (Reinit refused 6 x 250 ms)
            CT_CHECK(new_while_lost <= new_before + 1);    // no new image while the secure desktop is up
        }
        CT_CHECK_EQ(s.lost_since_qpc.load(), 0);           // recovered
        CT_CHECK(s.new_frames.load() > new_while_lost);    // images flow again
        CT_CHECK_EQ(s.dropped_pool_empty.load() + s.dropped_sink.load(), 0ull);
        CT_CHECK_EQ(cap.Nv12Pool().InUse(), 0u);
        // One frame per tick throughout, also while access was lost.
        CT_CHECK_EQ(static_cast<uint64_t>(sink.times.size()), s.new_frames.load() + s.repeats.load());
        CheckContinuous(sink, s, 30);
    }
}

// A display that is unplugged (DXGI no longer lists it) ends the capture after display_lost_timeout_ms with
// OnError(kDisplayRemovedHr), so the session finalizes a playable file; until then the last frame repeats. One that
// comes back within the timeout is picked up again without an error.
CT_TEST_DEVICE(capture_display_removed_ends_or_recovers) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    DisplayInfo disp;
    CT_REQUIRE(PrimaryDisplay(dev, disp));
    for (int back = 0; back < 2; ++back) {
        ScreenCapturer cap;
        ScreenCapturer::Config cfg;
        cfg.display = disp;
        cfg.region = RectI{disp.rect.x, disp.rect.y, 320, 240};
        cfg.fps_num = 30;
        cfg.display_lost_timeout_ms = back ? 3000 : 700;
        CT_REQUIRE(SUCCEEDED(cap.Init(dev, cfg)));
        Sink sink;
        sink.pool = &cap.Nv12Pool();
        sink.times.reserve(400);
        CT_REQUIRE(cap.Start(Clock::QpcNow(), sink));
        Sleep(300);
        cap.DuplicatorForTest().SimulateOutputRemovedForTest(true);
        cap.DuplicatorForTest().InjectAcquireErrorForTest(DXGI_ERROR_ACCESS_LOST, 1);
        const int64_t removed_qpc = Clock::QpcNow();
        if (back) {
            Sleep(1000);
            cap.DuplicatorForTest().SimulateOutputRemovedForTest(false);
            Sleep(600);
            CT_CHECK(cap.IsRunning());
            cap.Stop();
            PrintStats("unplugged 1 s, back", cap.Stats(), sink.times.size());
            CT_CHECK_EQ(sink.errors.load(), 0);
            CT_CHECK(cap.Stats().reinits.load() >= 1);
            CT_CHECK_EQ(cap.Stats().lost_since_qpc.load(), 0);
        } else {
            // The capture thread ends by itself once the timeout passed.
            while (cap.IsRunning() && Clock::QpcNow() - removed_qpc < 5 * Clock::QpcFrequency()) Sleep(10);
            const double ended_ms = static_cast<double>(Clock::QpcNow() - removed_qpc) * 1000.0 / Clock::QpcFrequency();
            const size_t frames = sink.times.size();
            CT_CHECK(!cap.IsRunning());
            cap.Stop();
            PrintStats("unplugged", cap.Stats(), frames);
            std::printf("  capture ended %.0f ms after the display was removed (timeout 700 ms)\n", ended_ms);
            CT_CHECK_EQ(sink.errors.load(), 1);
            CT_CHECK(sink.last_error.load() == kDisplayRemovedHr);
            CT_CHECK(ended_ms >= 650 && ended_ms < 3000);
            // Frames kept coming (repeats) until the end: ~0.7 s at 30 fps after the removal.
            CT_CHECK(cap.Stats().repeats.load() >= 15);
        }
        CT_CHECK_EQ(cap.Nv12Pool().InUse(), 0u);
        CheckContinuous(sink, cap.Stats(), 30);
    }
}

// Mode change of a whole-display recording: the stream keeps its size and the new desktop is scaled in, centred,
// letterboxed. Simulated with display rects that differ from the real desktop: a 4:3 rect of the same width (the
// desktop gets bars above and below) and a half-size rect (the desktop is scaled down). A white window in the centre
// of the real display must appear in the centre of the stream.
CT_TEST_DEVICE(capture_mode_change_keeps_size_letterboxed) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    DisplayInfo real;
    CT_REQUIRE(PrimaryDisplay(dev, real));
    if (real.rotation != Rotation::Identity) CT_SKIP("primary display is rotated");
    const int W = real.rect.w & ~1, H = real.rect.h & ~1;
    test_util::SolidWindow win(real.rect.x + W / 2 - 100, real.rect.y + H / 2 - 100, 200, 200, RGB(255, 255, 255));
    win.Pump();
    Sleep(200);
    struct Case { const char* name; int w, h; };
    const Case cases[] = {{"4:3 stream", W, (W * 3 / 4) & ~1}, {"half-size stream", (W / 2) & ~1, (H / 2) & ~1}};
    for (const Case& c : cases) {
        DisplayInfo fake = real;
        fake.rect = RectI{real.rect.x, real.rect.y, c.w, c.h};
        ScreenCapturer cap;
        ScreenCapturer::Config cfg;
        cfg.display = fake;
        cfg.region = RectI{};   // whole (fake) display
        cfg.fps_num = 30;
        CT_REQUIRE(SUCCEEDED(cap.Init(dev, cfg)));
        CT_CHECK_EQ(cap.OutputSize().w, c.w);
        CT_CHECK_EQ(cap.OutputSize().h, c.h);
        Sink sink;
        sink.pool = &cap.Nv12Pool();
        sink.times.reserve(200);
        sink.keep_at = 8;
        CT_REQUIRE(cap.Start(Clock::QpcNow(), sink));
        for (int i = 0; i < 10; ++i) {
            win.Pump();
            Sleep(50);
        }
        cap.Stop();
        PrintStats(c.name, cap.Stats(), sink.times.size());
        CT_CHECK_EQ(sink.errors.load(), 0);
        CT_CHECK(cap.Stats().fit_frames.load() >= 1);
        const int kept = sink.kept_index.load();
        CT_REQUIRE(kept >= 0);
        std::vector<uint8_t> px;
        UINT w = 0, h = 0;
        CT_REQUIRE(test_util::ReadbackTexture(dev, cap.Nv12Pool().At(static_cast<uint32_t>(kept)), px, w, h));
        cap.Nv12Pool().Release(static_cast<uint32_t>(kept));
        CT_CHECK_EQ(static_cast<int>(w), c.w);
        CT_CHECK_EQ(static_cast<int>(h), c.h);
        const FitRect fr = FitContentRect(SizeI{W, H}, Rotation::Identity, SizeI{c.w, c.h});
        auto Y = [&](int x, int y) { return static_cast<int>(px[static_cast<size_t>(y) * w + x]); };
        // Centre: the white window (Y 235 for white, BT.709 limited).
        const int centre = Y(c.w / 2, c.h / 2);
        std::printf("  %s %dx%d: content (%.0f,%.0f %.0fx%.0f), centre Y %d\n", c.name, c.w, c.h, fr.x, fr.y, fr.w, fr.h, centre);
        CT_CHECK(centre >= 225);
        // Bars, when there are any, are black: Y 16 a few rows away from the content edge.
        if (fr.y >= 8) {
            int bad = 0;
            for (int x = 0; x < c.w; x += 16) {
                if (std::abs(Y(x, static_cast<int>(fr.y) - 4) - 16) > 1) ++bad;
                if (std::abs(Y(x, static_cast<int>(fr.y + fr.h) + 3) - 16) > 1) ++bad;
            }
            CT_CHECK_EQ(bad, 0);
        }
        CT_CHECK_EQ(cap.Nv12Pool().InUse(), 0u);
    }
}

// An armed (prepared, idle) capturer holds no duplication; Start re-creates it and the first frame still comes within a
// few ticks on a static desktop.
CT_TEST_DEVICE(capture_released_duplication_comes_back_at_start) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    DisplayInfo disp;
    CT_REQUIRE(PrimaryDisplay(dev, disp));
    ScreenCapturer cap;
    ScreenCapturer::Config cfg;
    cfg.display = disp;
    cfg.fps_num = 60;
    cfg.release_duplication_until_start = true;
    CT_REQUIRE(SUCCEEDED(cap.Init(dev, cfg)));
    CT_CHECK(!cap.DuplicatorForTest().HasDuplication());
    Sink sink;
    sink.pool = &cap.Nv12Pool();
    sink.times.reserve(100);
    const int64_t start = Clock::QpcNow();
    CT_REQUIRE(cap.Start(start, sink));
    int64_t first_qpc = 0;
    while (Clock::QpcNow() - start < Clock::QpcFrequency()) {
        if (!sink.times.empty() && !first_qpc) first_qpc = Clock::QpcNow();
        if (first_qpc) break;
        Sleep(1);
    }
    cap.Stop();
    CT_REQUIRE(first_qpc != 0);
    const double ms = static_cast<double>(first_qpc - start) * 1000.0 / Clock::QpcFrequency();
    std::printf("  first frame %.1f ms after Start (duplication re-created at start), reinits %llu\n", ms,
                static_cast<unsigned long long>(cap.Stats().reinits.load()));
    CT_CHECK(ms < 500);
    CT_CHECK_EQ(sink.errors.load(), 0);
}

// Multi-monitor / mixed DPI: display rects are physical pixels and match DXGI's desktop coordinates for every display
// (a DPI-virtualized rect on a scaled monitor would not), and a current DXGI factory is used as is.
CT_TEST_DEVICE(display_rects_are_physical_and_factory_current) {
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    CT_CHECK(CurrentDxgiFactory(dev.Factory()) == nullptr);   // nothing changed since the device was created
    CT_CHECK(CurrentDxgiFactory(nullptr) != nullptr);
    const std::vector<DisplayInfo> displays = EnumerateDisplays(dev.Factory());
    CT_REQUIRE(!displays.empty());
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    int matched = 0;
    for (UINT a = 0; dev.Factory()->EnumAdapters1(a, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++a) {
        Microsoft::WRL::ComPtr<IDXGIOutput> o;
        for (UINT i = 0; adapter->EnumOutputs(i, o.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC od{};
            if (FAILED(o->GetDesc(&od)) || !od.AttachedToDesktop) continue;
            for (const DisplayInfo& d : displays) {
                if (d.device_name != od.DeviceName) continue;
                const RECT& r = od.DesktopCoordinates;
                std::printf("  %ls: GDI (%d,%d %dx%d) DXGI (%ld,%ld %ldx%ld), %u dpi\n", d.device_name.c_str(), d.rect.x, d.rect.y,
                            d.rect.w, d.rect.h, r.left, r.top, r.right - r.left, r.bottom - r.top, d.dpi);
                CT_CHECK(d.rect.x == r.left && d.rect.y == r.top && d.rect.w == r.right - r.left && d.rect.h == r.bottom - r.top);
                ++matched;
            }
        }
    }
    CT_CHECK_EQ(matched, static_cast<int>(displays.size()));
}
