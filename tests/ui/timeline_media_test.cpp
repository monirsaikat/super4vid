// ct_ui tests for P09 task 9.4's media and transport wiring: the MediaCache-backed TimelineMediaProvider (thumbnails
// and peaks arrive asynchronously, calls never block, SRVs pinned per frame), the timeline's HintScrub while
// scrubbing, auto-scroll while playing (forward and reverse), and the --bench timeline draw of 1000_clips.ctproj with
// real thumbnails and waveforms.
#include "headless.h"

#include "../../src/ui/src/cache_media_provider.h"
#include "../common/test_device.h"
#include "../common/test_media.h"
#include "../common/timing.h"

#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/time.h>
#include <ct/media/peaks.h>
#include <ct/playback/media_cache.h>
#include <ct/project/serialize.h>
#include <ct/test/ct_test.h>
#include <ct/ui/editor_state.h>
#include <ct/ui/panels/timeline_widget.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::ui;
using namespace ct::project;
using ct::ui::testing::Headless;

namespace {

// Records what the timeline asks of the player; Seek moves the playhead like the real transport.
class FakePlayback final : public PlaybackControl {
public:
    explicit FakePlayback(EditorState& s) : m_s(s) {}
    bool playing = false;
    double rate = 0;
    int pauses = 0;
    std::vector<TimeUs> hints, seeks;

    bool Playing() const override { return playing; }
    double Rate() const override { return playing ? rate : 0.0; }
    void Play(double r) override {
        playing = true;
        rate = r;
    }
    void Pause() override {
        playing = false;
        ++pauses;
    }
    void Seek(TimeUs t) override {
        seeks.push_back(t);
        m_s.SetPlayhead(t);
    }
    void HintScrub(TimeUs t) override { hints.push_back(t); }
    bool Tick(double) override { return playing; }

private:
    EditorState& m_s;
};

Project OneClip(TimeUs clip_dur = 4'000'000) {
    Project p;
    p.id = "tl-media";
    Media m;
    m.id = "m1";
    m.kind = Media::Kind::Video;
    m.path = "C:/media/a.mp4";
    m.duration_us = 1'000'000'000;
    m.width = 1920;
    m.height = 1080;
    m.fps_num = 60;
    p.media.push_back(m);
    Track t;
    t.id = "t1";
    t.kind = Track::Kind::Video;
    t.name = "Video";
    p.tracks.push_back(t);
    Clip c;
    c.id = "c1";
    c.track = "t1";
    c.media = "m1";
    c.start_us = 1'000'000;
    c.out_us = clip_dur;
    p.clips.push_back(c);
    p.RecomputeNextId();
    return p;
}

void Frame(Headless& h, TimelineWidget& w, EditorState& s) {
    h.Frame([&] { w.Draw(s, ImVec2(1600, 400)); });
}

// MediaCache builds thumbnails and peaks on the IO pool. An earlier test (export dialog) shut the pool down, which
// makes Async::Run inline, and MediaCache::Thumbnail queues its job under its own lock: restart the pool for this
// scope (declare before the MediaCache, whose destructor waits for its jobs) and leave it shut down afterwards.
struct IoPool {
    IoPool() { Async::Init(2); }
    ~IoPool() { Async::Shutdown(); }
    IoPool(const IoPool&) = delete;
    IoPool& operator=(const IoPool&) = delete;
};

// A provider call only looks up caches; a decode on the calling thread would take tens of ms. The 2 ms UI-thread
// budget (docs/02 section 6) is enforced with CT_STRICT_TIMING; loaded machines get a gross-regression limit.
double CallLimitMs() {
#ifdef CT_DEBUG
    return test::TimingLimit(20.0, 50.0);   // debug CRT + D3D debug layer
#else
    return test::TimingLimit(2.0, 15.0);
#endif
}

} // namespace

// --- scrub + auto-scroll ----------------------------------------------------------------------------------------

CT_TEST(timeline_scrub_hints_the_player_every_move) {
    Headless h(1600, 400);
    EditorState s;
    s.SetProject(OneClip());
    s.pps = 100;
    FakePlayback fp(s);
    fp.playing = true;
    fp.rate = 1;
    s.playback = &fp;
    TimelineWidget w;
    Frame(h, w, s);
    const float y = w.RulerY() + 6;
    const TimelineView v = w.View();
    h.Mouse(v.X(2'000'000), y);
    Frame(h, w, s);
    h.Button(0, true);
    Frame(h, w, s);
    CT_CHECK(w.CurrentMode() == TimelineWidget::Mode::Scrub);
    CT_CHECK_EQ(fp.pauses, 1);   // scrubbing pauses playback
    CT_REQUIRE(!fp.hints.empty());
    const size_t first = fp.hints.size();
    for (int i = 1; i <= 4; ++i) {
        h.Mouse(v.X(2'000'000 + i * 500'000), y);
        Frame(h, w, s);
    }
    h.Button(0, false);
    Frame(h, w, s);
    CT_CHECK(w.CurrentMode() == TimelineWidget::Mode::Idle);
    CT_CHECK_EQ(fp.hints.size(), first + 4);   // one paused prefetch hint per playhead move
    CT_CHECK_EQ(fp.hints.back(), s.Playhead());
    CT_CHECK_NEAR(s.Playhead(), 4'000'000, 10'000);   // within a pixel at 100 px/s
    CT_CHECK_EQ(s.undo.UndoEntries(), size_t(0));      // scrubbing is not an edit
}

CT_TEST(timeline_auto_scrolls_while_playing) {
    Headless h(1600, 400);
    EditorState s;
    s.SetProject(OneClip(900'000'000));
    s.pps = 100;
    FakePlayback fp(s);
    s.playback = &fp;
    TimelineWidget w;
    Frame(h, w, s);
    const TimeUs vis = w.View().PxToUs(w.View().width);
    CT_REQUIRE(vis > 1'000'000);

    // Paused: the view stays where the user put it.
    s.SetPlayhead(500'000'000);
    Frame(h, w, s);
    CT_CHECK_EQ(s.scroll_us, TimeUs(0));

    // Playing forward, near the right edge: page by 80 %.
    fp.playing = true;
    fp.rate = 1;
    s.scroll_us = 100'000'000;
    s.SetPlayhead(100'000'000 + vis * 97 / 100);
    Frame(h, w, s);
    CT_CHECK_NEAR(s.scroll_us, 100'000'000 + vis * 80 / 100, 2);
    // Still inside: no jump.
    const TimeUs held = s.scroll_us;
    s.SetPlayhead(held + vis / 2);
    Frame(h, w, s);
    CT_CHECK_EQ(s.scroll_us, held);
    // Far outside (seek / loop wrap): the playhead lands 10 % in.
    s.SetPlayhead(600'000'000);
    Frame(h, w, s);
    CT_CHECK_NEAR(s.scroll_us, 600'000'000 - vis / 10, 2);

    // Reverse (J): near the left edge pages left; far away lands 90 % in.
    fp.rate = -2;
    s.scroll_us = 300'000'000;
    s.SetPlayhead(300'000'000 + vis * 2 / 100);
    Frame(h, w, s);
    CT_CHECK_NEAR(s.scroll_us, 300'000'000 - vis * 80 / 100, 2);
    s.SetPlayhead(50'000'000);
    Frame(h, w, s);
    CT_REQUIRE(vis * 90 / 100 < 50'000'000);
    CT_CHECK_NEAR(s.scroll_us, 50'000'000 - vis * 90 / 100, 2);
    const float x = w.View().X(s.Playhead());
    CT_CHECK(x >= w.View().origin_x && x <= w.View().origin_x + w.View().width);
}

// --- MediaCache-backed provider ---------------------------------------------------------------------------------

CT_TEST_DEVICE(cache_media_provider_thumbnails_and_peaks_never_block) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    IoPool pool;
    playback::MediaCache cache;
    CT_REQUIRE(SUCCEEDED(cache.Open(*dev, path)));
    fs::Delete(media::Peaks::CachePath(path, cache.Info().AudioStreams()[0]));   // force a real (async) build

    Headless h;   // PeaksMinMax reads ImGui time
    Project p;
    Media v;
    v.id = "v";
    v.kind = Media::Kind::Video;
    v.path = path;
    v.duration_us = 5'000'000;
    v.audio_streams = {"default"};
    p.media.push_back(v);
    Media a = v;
    a.id = "a";
    a.kind = Media::Kind::Audio;
    p.media.push_back(a);
    Media gone = v;
    gone.id = "gone";
    gone.missing = true;
    p.media.push_back(gone);
    Media opening = v;
    opening.id = "opening";
    opening.path = "C:/nowhere/opening.mp4";
    p.media.push_back(opening);
    Media broken = opening;
    broken.id = "broken";
    broken.path = "C:/nowhere/broken.mp4";
    p.media.push_back(broken);

    ui::detail::CacheMediaProvider prov(&p, [&](const Id& id, bool* failed) -> playback::MediaCache* {
        *failed = id == "broken";
        return id == "v" || id == "a" ? &cache : nullptr;
    });

    ImTextureID tex = ImTextureID_Invalid;
    ImVec2 uv0, uv1;
    CT_CHECK(prov.Thumbnail("a", 0, tex, uv0, uv1) == MediaStatus::Unavailable);   // audio media: no filmstrip
    CT_CHECK(prov.Thumbnail("gone", 0, tex, uv0, uv1) == MediaStatus::Unavailable);
    CT_CHECK(prov.Thumbnail("opening", 0, tex, uv0, uv1) == MediaStatus::Loading);   // cache still opening
    CT_CHECK(prov.Thumbnail("broken", 0, tex, uv0, uv1) == MediaStatus::Unavailable);

    // Thumbnails of all 5 one-second buckets: Loading at first, Ready after the IO pool decoded them; every call
    // returns at once.
    double worst_ms = 0;
    int ready = 0;
    Stopwatch total;
    total.Start();
    bool first_round = true;
    int loading_first = 0;
    while (ready < 5 && total.ElapsedMs() < 15000) {
        prov.BeginFrame();
        ready = 0;
        for (TimeUs t = 0; t < 5'000'000; t += 1'000'000) {
            Stopwatch sw;
            sw.Start();
            const MediaStatus st = prov.Thumbnail("v", t + 400'000, tex, uv0, uv1);
            worst_ms = std::max(worst_ms, sw.ElapsedMs());
            if (st == MediaStatus::Ready) {
                ++ready;
                CT_CHECK(tex != ImTextureID_Invalid);
            } else {
                CT_CHECK(st == MediaStatus::Loading);
                loading_first += first_round ? 1 : 0;
            }
        }
        first_round = false;
        if (ready < 5) Sleep(10);
    }
    std::printf("    5 thumbnails ready after %.0f ms, worst call %.3f ms\n", total.ElapsedMs(), worst_ms);
    CT_CHECK_EQ(ready, 5);
    CT_CHECK_EQ(loading_first, 5);   // nothing was decoded on the calling thread
    CT_CHECK(worst_ms < CallLimitMs());
    CT_CHECK_EQ(prov.Pinned(), size_t(5));   // pinned until the next frame starts
    prov.BeginFrame();
    CT_CHECK_EQ(prov.Pinned(), size_t(0));

    // Camera clips of a recording take their filmstrip from the camera file's cache.
    {
        Project q;
        Media rec = v;
        rec.id = "rec";
        rec.kind = Media::Kind::Recording;
        rec.has_camera = true;
        q.media.push_back(rec);
        Media nocam = rec;
        nocam.id = "nocam";
        nocam.has_camera = false;
        q.media.push_back(nocam);
        playback::MediaCache* asked_main = nullptr;
        int camera_calls = 0;
        ui::detail::CacheMediaProvider cp(
            &q, [&](const Id&, bool* failed) { *failed = false; return asked_main; },
            [&](const Id&, bool* failed) -> playback::MediaCache* {
                ++camera_calls;
                *failed = false;
                return &cache;
            });
        Clip cam;
        cam.media = "rec";
        cam.source = Clip::Source::Camera;
        CT_CHECK(cp.ClipThumbnail(cam, 400'000, tex, uv0, uv1) == MediaStatus::Ready);   // bucket decoded above
        CT_CHECK_EQ(camera_calls, 1);
        cam.media = "nocam";
        CT_CHECK(cp.ClipThumbnail(cam, 400'000, tex, uv0, uv1) == MediaStatus::Unavailable);
        CT_CHECK_EQ(camera_calls, 1);
        Clip screen = cam;
        screen.media = "rec";
        screen.source = Clip::Source::Screen;
        CT_CHECK(cp.ClipThumbnail(screen, 400'000, tex, uv0, uv1) == MediaStatus::Loading);   // main cache opening
        CT_CHECK_EQ(camera_calls, 1);
    }

    // Peaks: Loading until the IO pool built them, then 100 pairs per second of min <= max.
    size_t n = 0;
    int per_second = 0;
    MediaStatus st = MediaStatus::Unavailable;
    worst_ms = 0;
    const float* peaks = nullptr;
    total.Start();
    bool first = true;
    while (total.ElapsedMs() < 15000) {
        Stopwatch sw;
        sw.Start();
        peaks = prov.PeaksMinMax("v", 0, &n, &per_second, &st);
        worst_ms = std::max(worst_ms, sw.ElapsedMs());
        if (first) CT_CHECK(st == MediaStatus::Loading);
        first = false;
        if (peaks) break;
        Sleep(10);
    }
    std::printf("    peaks ready after %.0f ms, worst call %.3f ms\n", total.ElapsedMs(), worst_ms);
    CT_REQUIRE(peaks != nullptr);
    CT_CHECK(st == MediaStatus::Ready);
    CT_CHECK_EQ(per_second, 100);
    CT_CHECK(n >= 500 && n <= 502);
    for (size_t i = 0; i < n; ++i) CT_CHECK(peaks[2 * i] <= peaks[2 * i + 1]);
    CT_CHECK(worst_ms < CallLimitMs());
    // Cached in the provider: the same pointer next time.
    size_t n2 = 0;
    CT_CHECK(prov.PeaksMinMax("v", 0, &n2, &per_second, &st) == peaks);
    CT_CHECK(prov.PeaksMinMax("gone", 0, &n2, &per_second, &st) == nullptr);
    CT_CHECK(st == MediaStatus::Unavailable);
}

// The draw budget with real thumbnails and waveforms: every clip of 1000_clips.ctproj points into the 5 s pattern
// file (in 0, same durations, so the layout is the fixture's), served by one MediaCache through CacheMediaProvider.
CT_BENCH(bench_timeline_draw_1000_clips_with_thumbnails, 200) {
    platform::D3DDevice* dev = test::SharedDevice();
    const std::string path = test::FindTestMedia("pattern_1080p60_5s.mp4");
    if (!dev || path.empty()) {
        std::printf("  skipped: no device or test media\n");
        return;
    }
    Project p;
    Err err;
    if (!Load("data/projects/1000_clips.ctproj", p, &err)) {
        CT_FAIL("cannot load 1000_clips.ctproj");
        return;
    }
    char full[MAX_PATH];
    GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr);
    for (Media& m : p.media) {
        m.path = full;
        m.missing = false;
    }
    for (Clip& c : p.clips) {
        if (c.IsAnnotation()) continue;
        const TimeUs d = c.out_us - c.in_us;
        c.in_us = 0;
        c.out_us = d;
    }
    IoPool pool;
    playback::MediaCache cache;
    if (FAILED(cache.Open(*dev, full))) {
        CT_FAIL("cannot open the pattern file");
        return;
    }
    Headless h(1600, 300);
    EditorState s;
    s.SetProject(std::move(p));
    ui::detail::CacheMediaProvider prov(&s.project, [&](const Id&, bool* failed) {
        *failed = false;
        return &cache;
    });
    // The same draw with an in-memory provider (always Ready, one texture id per 1 s bucket): separates the widget's
    // drawing cost from the cache lookups.
    struct Synthetic final : TimelineMediaProvider {
        std::vector<float> peaks = std::vector<float>(2 * 1000, 0.0f);
        Synthetic() {
            for (size_t i = 0; i < 1000; ++i) {
                peaks[2 * i] = -0.3f - 0.2f * static_cast<float>(i % 7) / 7;
                peaks[2 * i + 1] = 0.3f + 0.2f * static_cast<float>(i % 5) / 5;
            }
        }
        MediaStatus Thumbnail(const Id&, TimeUs t, ImTextureID& tex, ImVec2& uv0, ImVec2& uv1) override {
            tex = static_cast<ImTextureID>(0x1000 + t / 1'000'000);
            uv0 = ImVec2(0, 0);
            uv1 = ImVec2(1, 1);
            return MediaStatus::Ready;
        }
        const float* PeaksMinMax(const Id&, int, size_t* n, int* per_second, MediaStatus* status) override {
            *n = 1000;
            *per_second = 100;
            *status = MediaStatus::Ready;
            return peaks.data();
        }
    } synthetic;
    TimelineWidget w;
    struct Zoom { const char* name; double pps; TimeUs scroll; };
    const Zoom zooms[] = {{"fit", FitPps(s.project.Duration(), 1440), 0},
                          {"10 px/s", 10, 300'000'000},
                          {"30 px/s", 30, 300'000'000},
                          {"100 px/s", 100, 300'000'000}};
    for (int pass = 0; pass < 2; ++pass) {
        w.SetMediaProvider(pass == 0 ? static_cast<TimelineMediaProvider*>(&synthetic) : &prov);
        for (const Zoom& z : zooms) {
            s.pps = z.pps;
            s.scroll_us = z.scroll;
            // Warm up until nothing is loading (thumbnails decoded, peaks built).
            Stopwatch warm;
            warm.Start();
            do {
                prov.BeginFrame();
                h.Frame([&] { w.Draw(s, ImVec2(1600, 300)); });
                if (w.WantsRedraw()) Sleep(5);
            } while (w.WantsRedraw() && warm.ElapsedMs() < 20000);
            std::vector<double> times;
            int cmds = 0;
            for (int64_t i = 0; i < CT_BENCH_N; ++i) {
                double ms = 0;
                prov.BeginFrame();
                h.Frame([&] {
                    Stopwatch sw;
                    sw.Start();
                    w.Draw(s, ImVec2(1600, 300));
                    ms = sw.ElapsedMs();
                });
                times.push_back(ms);
                cmds = 0;
                if (ImDrawData* dd = ImGui::GetDrawData())
                    for (ImDrawList* l : dd->CmdLists) cmds += l->CmdBuffer.Size;
            }
            double total_ms = 0;
            for (double t : times) total_ms += t;
            std::sort(times.begin(), times.end());
            const double avg = total_ms / static_cast<double>(times.size());
            const double median = times[times.size() / 2];
            std::printf("  timeline draw, 1000 clips, %s, %-9s: median %.3f ms, avg %.3f ms, p95 %.3f ms, %d clips, %d draw cmds%s\n",
                        pass == 0 ? "synthetic media" : "MediaCache media", z.name, median, avg,
                        times[times.size() * 95 / 100], w.ClipsDrawn(), cmds, w.WantsRedraw() ? " (still loading)" : "");
#if defined(NDEBUG)
            CT_CHECK(median < 1.0);   // the median: parallel builds on the reference machine spike single frames
#endif
        }
    }
}
