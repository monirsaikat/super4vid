// render::TimelinePlayer: frame resolution (exact/non-blocking, transitions, images, recordings with events) and the
// timeline audio mix (gain, keyframes, fades, mute, speed, contiguity) on the tests/data pattern media.
#include "../../src/render/src/audio_mixer.h"
#include "../common/pattern_counter.h"
#include "../common/nv12_readback.h"
#include "../common/test_device.h"
#include "../common/test_media.h"
#include "../common/test_project.h"
#include "../common/timing.h"

#include <ct/core/async.h>
#include <ct/core/fs.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/image_io.h>
#include <ct/playback/cache_budget.h>
#include <ct/render/compositor.h>
#include <ct/render/render_context.h>
#include <ct/render/timeline_player.h>
#include <ct/test/ct_test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace ct;
using render::TimelinePlayer;

namespace {

constexpr int kW = 1920, kH = 1080;
constexpr TimeUs kPatternUs = 5'000'000;   // video frames 0..299

struct Env {
    platform::D3DDevice* dev = nullptr;
    render::RenderContext rc;
    std::string pattern;
    std::string bundle;
    bool Init(platform::D3DDevice* d, const std::string& pat) {
        // MF, WIC and WASAPI need COM on the test thread (the runner initializes it only for golden tests).
        (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        dev = d;
        pattern = pat;
        bundle = fs::TempPath(".ctrec");
        return SUCCEEDED(rc.Init(*d)) && test::MakeBundle(bundle, pattern, kW, kH, kPatternUs, true);
    }
    ~Env() {
        if (!bundle.empty()) fs::DeleteTree(bundle);
    }
};

std::unique_ptr<TimelinePlayer> MakePlayer(render::RenderContext& rc, bool async = false) {
    auto tp = std::make_unique<TimelinePlayer>();
    TimelinePlayer::Options o;
    o.audio_output = false;
    o.async_open = async;
    if (FAILED(tp->Init(rc, nullptr, o))) return nullptr;
    return tp;
}

int64_t FrameIndexOfLayer(platform::D3DDevice& dev, const render::VideoLayerInput& L) {
    if (!L.y_srv) return -3;
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    L.y_srv->GetResource(res.GetAddressOf());
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) return -4;
    test::Nv12Image img;
    if (!test::ReadbackNv12(dev, tex.Get(), img)) return -2;
    return test::ReadPatternCounterY(img.y.data(), img.width, img.height, img.width);
}

double Rms(const std::vector<float>& v, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(std::max<size_t>(to - from, 1)));
}

// Zero crossings per second of the left channel over [from, to) frames.
double CrossingsPerSecond(const std::vector<float>& v, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from + 1; i < to; ++i)
        if ((v[2 * (i - 1)] < 0) != (v[2 * i] < 0)) ++n;
    return n * 48000.0 / double(to - from);
}

project::Project AudioProject(const std::string& bundle) {
    project::Project p = test::MakeBaseProject(bundle, kW, kH, kPatternUs);
    p.clips.push_back(test::MakeClip("a1", "t5", "m1", project::Clip::Source::Mic, 0, 0, 4'000'000));
    return p;
}

} // namespace

// ---- unit ---------------------------------------------------------------------------------------------------------

CT_TEST(timeline_audio_frames_add_up_per_video_frame) {
    int64_t total = 0;
    for (int n = 0; n < 60; ++n) total += TimelinePlayer::AudioFramesBetween(FrameTimeUs(n, 60, 1), FrameTimeUs(n + 1, 60, 1));
    CT_CHECK_EQ(total, 48000);
    total = 0;
    for (int n = 0; n < 30; ++n) total += TimelinePlayer::AudioFramesBetween(FrameTimeUs(n, 30000, 1001), FrameTimeUs(n + 1, 30000, 1001));
    CT_CHECK_EQ(total, TimelinePlayer::AudioSampleIndex(FrameTimeUs(30, 30000, 1001)));
    CT_CHECK_EQ(TimelinePlayer::AudioSampleIndex(-1), -1);
    CT_CHECK_EQ(TimelinePlayer::AudioFramesBetween(100, 50), 0u);
}

CT_TEST(timeline_audio_gain_fades_keys_normalize) {
    render::detail::AudioClipPlan c;
    c.start_us = 1'000'000;
    c.end_us = 3'000'000;
    c.gain_db = -6.0206f;
    CT_CHECK_NEAR(c.GainAt(2'000'000), 0.5f, 1e-4);
    c.fade_in_us = 500'000;
    c.fade_out_us = 1'000'000;
    CT_CHECK_NEAR(c.GainAt(1'250'000), 0.25f, 1e-4);
    CT_CHECK_NEAR(c.GainAt(2'500'000), 0.25f, 1e-4);
    CT_CHECK_NEAR(c.GainAt(1'000'000), 0.0f, 1e-6);
    c.fade_in_us = c.fade_out_us = 0;
    c.gain_keys.Set(0, 0.f, project::Ease::Linear);
    c.gain_keys.Set(1'000'000, -20.f, project::Ease::Linear);
    CT_CHECK_NEAR(c.GainAt(1'000'000), 1.0f, 1e-5);
    CT_CHECK_NEAR(c.GainAt(2'000'000), 0.1f, 1e-5);
    CT_CHECK_NEAR(c.GainAt(1'500'000), std::pow(10.f, -0.5f), 1e-4);
    c.normalize = true;
    c.norm_gain = 2.f;
    CT_CHECK_NEAR(c.GainAt(2'000'000), 0.2f, 1e-5);
    std::vector<float> mn{-0.1f, -0.25f, 0.f}, mx{0.2f, 0.1f, 0.f};
    CT_CHECK_NEAR(render::detail::NormalizeGain(mn, mx, 100, 0, 30'000), std::pow(10.f, -1.f / 20.f) / 0.25f, 1e-4);
    CT_CHECK_NEAR(render::detail::NormalizeGain(mn, mx, 100, 20'000, 30'000), 1.0f, 1e-6);   // silence -> unity
    std::vector<float> q{-0.001f}, r{0.001f};
    CT_CHECK_NEAR(render::detail::NormalizeGain(q, r, 100, 0, 10'000), 10.f, 1e-4);           // +20 dB cap
}

CT_TEST(timeline_audio_stream_selection) {
    project::Media m;
    m.audio_streams = {"mic", "system"};
    project::Clip c;
    c.source = project::Clip::Source::System;
    CT_CHECK_EQ(render::detail::AudioIndexFor(m, c), 1);
    c.source = project::Clip::Source::Mic;
    CT_CHECK_EQ(render::detail::AudioIndexFor(m, c), 0);
    m.audio_streams = {"system"};
    c.source = project::Clip::Source::System;
    CT_CHECK_EQ(render::detail::AudioIndexFor(m, c), 0);
    c.source = project::Clip::Source::Default;
    CT_CHECK_EQ(render::detail::AudioIndexFor(m, c), 0);
}

// ---- device -------------------------------------------------------------------------------------------------------

CT_TEST_DEVICE(timeline_player_resolve_exact_frames_and_events) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    project::Project p = test::MakeBaseProject(env.bundle, kW, kH, kPatternUs);
    p.clips.push_back(test::MakeClip("c1", "t1", "m1", project::Clip::Source::Screen, 1'000'000, 500'000, 4'500'000));
    project::Clip cam = test::MakeClip("c2", "t2", "m1", project::Clip::Source::Camera, 0, 0, 2'000'000);
    cam.pip = project::Pip{};
    p.clips.push_back(cam);
    auto tp = MakePlayer(env.rc);
    CT_REQUIRE(tp);
    tp->SetProject(&p);
    render::FrameInputs in;
    // t = 1.5 s: track order puts t1 (screen) first, then t2 (camera, media 1.5 s = frame 90).
    CT_REQUIRE(tp->ResolveExact(1'500'000, in, 5000));
    CT_REQUIRE(in.layers.size() == 2u);
    CT_CHECK(in.layers[0].clip == p.FindClip("c1"));
    CT_CHECK(in.layers[1].clip == p.FindClip("c2"));
    CT_CHECK_EQ(in.layers[0].media_t_us, 1'000'000);            // 0.5 s in + 0.5 s into the clip
    CT_CHECK_EQ(FrameIndexOfLayer(*dev, in.layers[0]), 60);
    CT_CHECK_EQ(FrameIndexOfLayer(*dev, in.layers[1]), 90);
    CT_CHECK(in.layers[0].events != nullptr);                  // screen clip of a recording with events
    CT_CHECK(in.layers[1].events == nullptr);
    CT_CHECK_EQ(in.layers[0].video_size.w, kW);
    CT_CHECK(tp->Cache("m1") != nullptr && tp->CameraCache("m1") != nullptr && tp->Events("m1") != nullptr);
    // Frame-accurate sweep: every 7th frame of the screen clip.
    for (int n = 70; n < 250; n += 7) {
        const TimeUs t = FrameTimeUs(n, 60, 1);
        CT_REQUIRE(tp->ResolveExact(t, in, 5000));
        const project::Clip* c1 = p.FindClip("c1");
        for (const auto& L : in.layers)
            if (L.clip == c1) {
                const int64_t want = FrameIndexAt(render::Compositor::ClipMediaTime(*c1, t), 60, 1);
                CT_CHECK_EQ(FrameIndexOfLayer(*dev, L), want);
            }
    }
    // Outside every clip: no layers.
    CT_REQUIRE(tp->ResolveExact(5'200'000, in, 5000));
    CT_CHECK_EQ(in.layers.size(), 0u);
    tp->Shutdown();
}

CT_TEST_DEVICE(timeline_player_resolve_nonblocking_converges) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    project::Project p = test::MakeBaseProject(env.bundle, kW, kH, kPatternUs);
    p.clips.push_back(test::MakeClip("c1", "t1", "m1", project::Clip::Source::Screen, 0, 0, kPatternUs));
    auto tp = MakePlayer(env.rc, true);
    CT_REQUIRE(tp);
    tp->SetProject(&p);
    render::FrameInputs in;
    bool exact = true;
    Stopwatch sw;
    sw.Start();
    CT_REQUIRE(tp->Resolve(2'000'000, in, exact));
    CT_CHECK(!exact);   // media opens on the IO pool: never blocks
    CT_CHECK(sw.ElapsedMs() < 200.0);
    int tries = 0;
    while (!exact && tries++ < 500) {
        Sleep(10);
        tp->Resolve(2'000'000, in, exact);
    }
    CT_REQUIRE(exact);
    CT_REQUIRE(in.layers.size() == 1u);
    CT_CHECK_EQ(FrameIndexOfLayer(*dev, in.layers[0]), 120);
    std::printf("    exact after %d polls (%.0f ms)\n", tries, sw.ElapsedMs());
    p.tracks[0].hidden = true;   // hidden track: nothing resolved
    tp->Resolve(2'000'000, in, exact);
    CT_CHECK_EQ(in.layers.size(), 0u);
    tp->Shutdown();
}

// Regression (P09 9.7, --bench-scrub hang): three files share the global CacheBudget. When the other caches' frames
// filled it while every frame of one cache was pinned or prefetched ahead, that cache refused its playhead frame and
// slept until the next hint, which a paused playhead never sends: Resolve stayed inexact forever. Every paused seek
// must now reach the exact frame in bounded time, with the budget full of foreign bytes and with real competition.
CT_TEST_DEVICE(timeline_player_paused_seek_exact_under_full_budget) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    CT_REQUIRE_MEDIA(pat4k, "pattern_4k60_3s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    project::Project p = test::MakeBaseProject(env.bundle, kW, kH, kPatternUs);
    project::Media big;
    big.id = "m2";
    big.kind = project::Media::Kind::Video;
    big.path = fs::Absolute(pat4k);
    big.duration_us = 3'000'000;
    big.width = 3840;
    big.height = 2160;
    big.fps_num = 60;
    p.media.push_back(big);
    p.clips.push_back(test::MakeClip("s", "t1", "m1", project::Clip::Source::Screen, 0, 0, kPatternUs));
    project::Clip cam = test::MakeClip("k", "t2", "m1", project::Clip::Source::Camera, 0, 0, kPatternUs);
    cam.pip = project::Pip{};
    p.clips.push_back(cam);
    p.clips.push_back(test::MakeClip("b", "t3", "m2", project::Clip::Source::Default, 0, 0, 3'000'000));
    auto tp = MakePlayer(env.rc, true);
    CT_REQUIRE(tp);
    tp->SetProject(&p);
    CT_REQUIRE(tp->WaitMediaOpen(10'000));

    const size_t old_budget = playback::CacheBudget::Bytes();
    // Polls like the editor's frame loop (non-blocking Resolve, no re-seek) until every layer is exact.
    auto seek_until_exact = [&](TimeUs t, double limit_ms, size_t want_layers) {
        tp->Seek(t);
        render::FrameInputs in;
        bool exact = false;
        Stopwatch sw;
        sw.Start();
        while (sw.ElapsedMs() < limit_ms) {
            tp->Resolve(t, in, exact);
            if (exact) break;
            Sleep(5);
        }
        if (!exact || in.layers.size() != want_layers) {
            std::printf("    t %lld: exact %d, %zu layers after %.0f ms\n", static_cast<long long>(t), exact ? 1 : 0, in.layers.size(), sw.ElapsedMs());
            playback::MediaCache* caches[3] = {tp->Cache("m1"), tp->CameraCache("m1"), tp->Cache("m2")};
            for (playback::MediaCache* c : caches) {
                if (!c) continue;
                const playback::MediaCache::Stats st = c->GetStats();
                std::printf("      cache: %zu frames, %llu decoded, %llu seeks, %llu evictions, budget used %zu of %zu\n", st.cached_frames,
                            static_cast<unsigned long long>(st.frames_decoded), static_cast<unsigned long long>(st.seeks),
                            static_cast<unsigned long long>(st.evictions), playback::CacheBudget::Used(), playback::CacheBudget::Bytes());
            }
        }
        return exact && in.layers.size() == want_layers;
    };
    const TimeUs frame = FrameTimeUs(1, 60, 1);
    // 1) The budget is full of bytes no cache here can evict (another editor/export cache): only the playhead frames.
    playback::CacheBudget::SetBytes(64ull << 20);
    const int64_t foreign = static_cast<int64_t>(64ull << 20);
    playback::CacheBudget::Add(foreign);
    const TimeUs times1[] = {2'000'000, 500'000, 2'900'000, 1'100'000, 4'200'000, 1'100'000 + frame};
    int ok1 = 0;
    // 8 s per seek: a cold 4K seek decodes up to a GOP on a loaded machine; the bug never converged at all.
    for (TimeUs t : times1) {
        if (!seek_until_exact(t, 8000, t < 3'000'000 ? 3u : 2u)) break;   // the first failure ends the phase
        ++ok1;
    }
    playback::CacheBudget::Add(-foreign);
    CT_CHECK_EQ(ok1, static_cast<int>(sizeof(times1) / sizeof(times1[0])));
    // 2) A small budget the three windows compete for (~2 4K frames + a few 1080p): far seeks and frame steps.
    playback::CacheBudget::SetBytes(40ull << 20);
    uint32_t rng = 12345;
    int ok2 = 0;
    TimeUs t = 0;
    const int n2 = 24;
    for (int i = 0; i < n2; ++i) {
        rng = rng * 1664525u + 1013904223u;
        if (i % 3 == 2) t = std::clamp<TimeUs>(t + ((i & 4) ? frame : -frame), 0, kPatternUs - frame);
        else t = static_cast<TimeUs>(rng % static_cast<uint32_t>(kPatternUs - frame));
        if (!seek_until_exact(t, 8000, t < 3'000'000 ? 3u : 2u)) break;
        ++ok2;
    }
    playback::CacheBudget::SetBytes(old_budget);
    CT_CHECK_EQ(ok2, n2);
    tp->Shutdown();
}

CT_TEST_DEVICE(timeline_player_transition_and_image_layers) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    std::vector<uint8_t> px(64 * 64 * 4, 200);
    const std::string png = fs::TempPath(".png");
    CT_REQUIRE(SUCCEEDED(media::ImageIo::SavePng(png, px.data(), 64, 64, 64 * 4)));
    project::Project p = test::MakeBaseProject(env.bundle, kW, kH, kPatternUs);
    project::Media img;
    img.id = "m2";
    img.kind = project::Media::Kind::Image;
    img.path = png;
    img.width = img.height = 64;
    p.media.push_back(img);
    p.clips.push_back(test::MakeClip("a", "t1", "m1", project::Clip::Source::Screen, 0, 0, 2'000'000));
    p.clips.push_back(test::MakeClip("b", "t1", "m1", project::Clip::Source::Screen, 2'000'000, 3'000'000, 5'000'000));
    p.clips.push_back(test::MakeClip("i", "t3", "m2", project::Clip::Source::Default, 0, 0, 10'000'000));
    project::Transition tr;
    tr.track = "t1";
    tr.a = "a";
    tr.b = "b";
    tr.duration_us = 500'000;
    p.transitions.push_back(tr);
    auto tp = MakePlayer(env.rc);
    CT_REQUIRE(tp);
    tp->SetProject(&p);
    render::FrameInputs in;
    // 2.1 s: inside the transition window [1.75, 2.25): a (clamped to its last frame), b, then the image.
    CT_REQUIRE(tp->ResolveExact(2'100'000, in, 5000));
    CT_REQUIRE(in.layers.size() == 3u);
    CT_CHECK_EQ(in.active_transitions.size(), 1u);
    CT_CHECK(in.layers[0].clip->id == "a");
    CT_CHECK(in.layers[1].clip->id == "b");
    CT_CHECK_EQ(FrameIndexOfLayer(*dev, in.layers[0]), 119);   // a's last frame (media 2 s - 1 us)
    CT_CHECK_EQ(FrameIndexOfLayer(*dev, in.layers[1]), 186);   // b: 3 s + 0.1 s
    CT_CHECK(in.layers[2].is_rgba && in.layers[2].rgba_srv != nullptr && in.layers[2].rgba_premultiplied);
    CT_CHECK_EQ(in.layers[2].video_size.w, 64);
    // 1.9 s: a by itself, b pulled in by the transition (its first frame).
    CT_REQUIRE(tp->ResolveExact(1'900'000, in, 5000));
    CT_REQUIRE(in.layers.size() == 3u);
    CT_CHECK_EQ(FrameIndexOfLayer(*dev, in.layers[1]), 180);
    tp->Shutdown();
    fs::Delete(png);
}

CT_TEST_DEVICE(timeline_player_render_audio_mix) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    project::Project p = AudioProject(env.bundle);
    auto tp = MakePlayer(env.rc);
    CT_REQUIRE(tp);
    tp->SetProject(&p);
    // Pattern audio: 1 kHz sine at -12 dBFS (RMS 0.1776), 2 kHz for 20 ms at every whole second.
    const uint32_t n = TimelinePlayer::AudioFramesBetween(0, 2'000'000);
    CT_REQUIRE(n == 96000u);
    std::vector<float> one(size_t(n) * 2);
    CT_REQUIRE(SUCCEEDED(tp->RenderAudio(0, 2'000'000, one.data())));
    CT_CHECK_NEAR(Rms(one, 2 * 4800, 2 * 40000), 0.1776, 0.01);
    CT_CHECK_NEAR(CrossingsPerSecond(one, 4800, 40000), 2000.0, 20.0);
    // Chunked per 60 fps frame == one call (sample-contiguous decode).
    std::vector<float> chunked(size_t(n) * 2);
    {
        auto tp2 = MakePlayer(env.rc);
        CT_REQUIRE(tp2);
        tp2->SetProject(&p);
        size_t off = 0;
        for (int f = 0; f < 120; ++f) {
            const TimeUs a = FrameTimeUs(f, 60, 1), b = FrameTimeUs(f + 1, 60, 1);
            CT_REQUIRE(SUCCEEDED(tp2->RenderAudio(a, b, chunked.data() + off * 2)));
            off += TimelinePlayer::AudioFramesBetween(a, b);
        }
        CT_CHECK_EQ(off, size_t(n));
    }
    double maxdiff = 0;
    for (size_t i = 0; i < one.size(); ++i) maxdiff = std::max(maxdiff, double(std::fabs(one[i] - chunked[i])));
    CT_CHECK(maxdiff < 1e-6);
    // The beep at 1.000 s lands on sample 48000: 2 kHz in [48000, 48960), 1 kHz after.
    CT_CHECK_NEAR(CrossingsPerSecond(one, 48100, 48900), 4000.0, 250.0);
    CT_CHECK_NEAR(CrossingsPerSecond(one, 49200, 52000), 2000.0, 100.0);

    // Gain -6 dB -> half amplitude; a fade-in ramps from silence.
    p.clips[0].audio.gain_db = -6.0206f;
    p.clips[0].audio.fade_in_us = 1'000'000;
    std::vector<float> g(size_t(n) * 2);
    CT_REQUIRE(SUCCEEDED(tp->RenderAudio(0, 2'000'000, g.data())));
    CT_CHECK(Rms(g, 0, 2 * 2400) < 0.01);
    CT_CHECK_NEAR(Rms(g, 2 * 60000, 2 * 90000), 0.1776 / 2, 0.006);
    // Muted clip / muted track: silence.
    p.clips[0].audio.muted = true;
    CT_REQUIRE(SUCCEEDED(tp->RenderAudio(0, 2'000'000, g.data())));
    CT_CHECK(Rms(g, 0, g.size()) == 0.0);
    p.clips[0].audio.muted = false;
    p.tracks[4].muted = true;
    CT_REQUIRE(SUCCEEDED(tp->RenderAudio(0, 2'000'000, g.data())));
    CT_CHECK(Rms(g, 0, g.size()) == 0.0);
    p.tracks[4].muted = false;
    p.clips[0].audio = project::AudioProps{};
    // Speed 2: the 1 kHz tone plays at 2 kHz (pitch follows); 4 s of media fill 2 s.
    p.clips[0].speed = 2.0;
    CT_REQUIRE(SUCCEEDED(tp->RenderAudio(0, 2'000'000, g.data())));
    CT_CHECK_NEAR(CrossingsPerSecond(g, 2400, 20000), 4000.0, 60.0);
    CT_CHECK_NEAR(Rms(g, 2 * 2400, 2 * 20000), 0.1776, 0.015);
    // Later on the timeline: silence first, the media's 1 s beep at 1.5 s.
    p.clips[0].speed = 1.0;
    p.clips[0].start_us = 500'000;
    CT_REQUIRE(SUCCEEDED(tp->RenderAudio(0, 2'000'000, g.data())));
    CT_CHECK(Rms(g, 0, 2 * 23900) == 0.0);
    CT_CHECK_NEAR(CrossingsPerSecond(g, 24000 + 48100, 24000 + 48900), 4000.0, 250.0);
    tp->Shutdown();
}

CT_TEST_DEVICE(timeline_player_transport_without_audio_device) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    project::Project p = test::MakeBaseProject(env.bundle, kW, kH, kPatternUs);
    p.clips.push_back(test::MakeClip("c1", "t1", "m1", project::Clip::Source::Screen, 0, 0, 1'000'000));
    auto tp = MakePlayer(env.rc);
    CT_REQUIRE(tp);
    tp->SetProject(&p);
    CT_CHECK(!tp->HasAudioOutput());
    CT_CHECK_EQ(tp->Duration(), 1'000'000);
    tp->Seek(500'000);
    CT_CHECK_EQ(tp->Now(), 500'000);
    tp->StepFrames(3);
    CT_CHECK_EQ(tp->Now(), FrameTimeUs(33, 60, 1));
    tp->StepFrames(-100);
    CT_CHECK_EQ(tp->Now(), 0);
    tp->Seek(900'000);
    tp->Play();
    CT_CHECK(tp->Playing());
    Sleep(250);
    const TimeUs now = tp->Tick();   // ran past the end: paused at the end
    CT_CHECK(!tp->Playing());
    CT_CHECK_EQ(now, 1'000'000);
    tp->Play();                      // at the end: starts over
    CT_CHECK(tp->Now() < 100'000);
    tp->Pause();
    tp->SetRate(2.0);
    CT_CHECK_EQ(tp->Rate(), 2.0);
    tp->HintScrub(300'000);
    tp->Shutdown();
}

CT_TEST_DEVICE(timeline_player_playback_feed_with_audio_device) {
    CT_REQUIRE_DEVICE(dev);
    CT_REQUIRE_MEDIA(pat, "pattern_1080p60_5s.mp4");
    Env env;
    CT_REQUIRE(env.Init(dev, pat));
    audio::AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    project::Project p = AudioProject(env.bundle);
    TimelinePlayer tp;
    CT_REQUIRE(SUCCEEDED(tp.Init(env.rc, devices)));
    if (!tp.HasAudioOutput()) CT_SKIP("no audio render endpoint");
    tp.SetProject(&p);
    tp.Play();
    float peak = 0;
    for (int i = 0; i < 60; ++i) {
        Sleep(10);
        peak = std::max(peak, tp.Audio().PeakLevel());
    }
    const TimeUs now = tp.Tick();
    CT_CHECK(tp.Clock().UsingAudio());
    // About 0.6 s of playback; a loaded machine stretches the 60 sleeps, so the upper bound is loose unless strict.
    CT_CHECK(now > 300'000 && now < static_cast<TimeUs>(ct::test::TimingLimit(1'000'000, 4'000'000)));
    CT_CHECK(peak > 0.1f);   // the mixed sine (-12 dBFS, peak 0.25) reaches the device
    tp.Pause();
    tp.Shutdown();
}
