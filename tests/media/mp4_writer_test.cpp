// ct/media/mp4_writer.h, ct/media/probe.h: synthetic recordings through the real encoder.
#include "media_test_util.h"

#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/encoder_caps.h>
#include <ct/media/mf_runtime.h>
#include <ct/media/mp4_writer.h>
#include <ct/media/probe.h>
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <mferror.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <vector>

using namespace ct;
using namespace ct::media;
using namespace ct::media_test;
using ct::platform::D3DDevice;
using ct::platform::TexturePool;

namespace {

struct ErrorListener final : Mp4Writer::Listener {
    std::atomic<int> calls{0};
    std::atomic<HRESULT> last{S_OK};
    void OnWriterError(HRESULT hr) override {
        last.store(hr);
        calls.fetch_add(1);
    }
};

void SleepUntilQpc(int64_t qpc) {
    for (;;) {
        const int64_t now = Clock::QpcNow();
        if (now >= qpc) return;
        const int64_t ms = (qpc - now) * 1000 / Clock::QpcFrequency();
        Sleep(ms > 1 ? static_cast<DWORD>(ms - 1) : 0);
    }
}

ULONG BufferRefs(TexturePool& pool, uint32_t index) {
    auto* b = static_cast<IUnknown*>(pool.UserData(index));
    if (!b) return 0;
    b->AddRef();
    return b->Release();
}

struct SyntheticRun {
    int64_t frames_sent = 0, frames_rejected = 0, pool_empty = 0;
    int64_t audio_packets = 0, audio_rejected = 0;
};

// Feeds `frames` video frames (a new texture every `new_every` ticks, repeats in between, like ScreenCapturer) starting
// at tick `first_tick`, and 10 ms audio packets from t = audio_start_us on each audio stream, paced in real time.
SyntheticRun Feed(D3DDevice& dev, Mp4Writer& w, TexturePool* pool, int frames, int fps, int new_every, int64_t first_tick,
                  int audio_streams, TimeUs audio_start_us) {
    SyntheticRun r;
    const int64_t epoch = Clock::QpcNow();
    std::vector<float> pcm(480 * 2);
    int64_t audio_frames = 0;
    bool has_last = false;
    uint32_t last = 0;
    const TimeUs end_us = FrameTimeUs(first_tick + frames, fps, 1);
    int64_t n = first_tick;
    for (;;) {
        const TimeUs t_video = n < first_tick + frames ? FrameTimeUs(n, fps, 1) : INT64_MAX;
        const TimeUs ta = audio_start_us + audio_frames * 1'000'000 / 48000;
        const TimeUs t_audio = audio_streams > 0 && ta < end_us ? ta : INT64_MAX;
        if (t_video == INT64_MAX && t_audio == INT64_MAX) break;
        const TimeUs t = std::min(t_video, t_audio);
        SleepUntilQpc(Clock::UsToQpc(t, epoch));
        if (t == t_audio) {
            for (int s = 0; s < audio_streams; ++s) {
                Sine(pcm.data(), 480, s == 0 ? 1000.0 : 440.0, 0.25f, audio_frames);
                audio::AudioPacket p;
                p.t_us = t_audio;
                p.frames = 480;
                p.data = pcm.data();
                ++r.audio_packets;
                if (!w.OnAudio(static_cast<uint32_t>(s), p)) ++r.audio_rejected;
            }
            audio_frames += 480;
            continue;
        }
        if (pool) {
            uint32_t idx = 0;
            ID3D11Texture2D* tex = nullptr;
            if (!has_last || (n - first_tick) % new_every == 0) tex = pool->Acquire(idx);
            if (tex) {
                FillNv12(dev, tex, static_cast<uint8_t>(16 + (n * 7) % 220));
                last = idx;
                has_last = true;
            } else if (has_last) {
                if ((n - first_tick) % new_every == 0) ++r.pool_empty;
                idx = last;
                pool->AddRef(idx);
                tex = pool->At(idx);
            }
            if (tex) {
                ++r.frames_sent;
                if (!w.OnFrame(tex, idx, t_video)) {
                    ++r.frames_rejected;
                    pool->Release(idx);
                }
            }
        }
        ++n;
    }
    return r;
}

Mp4Writer::Config BaseConfig(const std::string& path, int w, int h, int fps, int audio_streams) {
    Mp4Writer::Config c;
    c.path = path;
    c.video.size = SizeI{w, h};
    c.video.fps_num = fps;
    for (int i = 0; i < audio_streams; ++i) {
        AudioEncodeConfig a;
        a.label = i == 0 ? "mic" : "system";
        a.bitrate_bps = i == 0 ? 128000 : 192000;   // tells the tracks apart in the file
        c.audio.push_back(a);
    }
    return c;
}

} // namespace

CT_TEST_DEVICE(mp4_writer_2s_1080p60_two_audio_streams) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    TexturePool pool;
    bool uav = false;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 1920, 1080, 12, true, &uav));
    const std::string path = TempPath("writer_2s.mp4");
    DeleteFileW(Utf8ToWide(path).c_str());
    {
        Mp4Writer w;
        CT_REQUIRE(SUCCEEDED(w.Init(dev, BaseConfig(path, 1920, 1080, 60, 2), &pool)));
        std::printf("  encoder %s (%s), NV12 textures %s UAV binding\n", w.EncoderName().c_str(), w.IsHardware() ? "hardware" : "software",
                    uav ? "with" : "without");
        CT_CHECK_EQ(w.VideoStreamIndex(), 0u);
        CT_CHECK_EQ(w.AudioStreamIndex(0), 1u);
        CT_CHECK_EQ(w.AudioStreamIndex(1), 2u);
        CT_REQUIRE(w.Start());
        const SyntheticRun r = Feed(dev, w, &pool, 120, 60, 2, 0, 2, 0);
        CT_CHECK_HR(w.Finalize());
        const auto& st = w.GetStats();
        std::printf("  sent %lld rejected %lld pool_empty %lld | written %llu, audio %llu/%llu frames | write avg %.3f max %.3f ms, "
                    "latency avg %.3f max %.3f ms, finalize %lld ms\n",
                    static_cast<long long>(r.frames_sent), static_cast<long long>(r.frames_rejected), static_cast<long long>(r.pool_empty),
                    static_cast<unsigned long long>(st.video_written.load()), static_cast<unsigned long long>(st.audio_written[0].load()),
                    static_cast<unsigned long long>(st.audio_written[1].load()), st.video_write.AvgMs(), st.video_write.MaxMs(),
                    st.video_latency.AvgMs(), st.video_latency.MaxMs(), static_cast<long long>(st.finalize_ms.load()));
        CT_CHECK_EQ(r.frames_sent, 120);
        CT_CHECK_EQ(r.frames_rejected, 0);
        CT_CHECK_EQ(r.pool_empty, 0);
        CT_CHECK_EQ(r.audio_rejected, 0);
        CT_CHECK_EQ(st.video_written.load(), 120ull);
        CT_CHECK_EQ(st.audio_written[0].load(), 96000ull);
        CT_CHECK_EQ(st.audio_written[1].load(), 96000ull);
        CT_CHECK_EQ(pool.InUse(), 0u);
        // Every slot becomes reusable within 500 ms after Finalize.
        Stopwatch sw;
        sw.Start();
        bool all = false;
        while (!all && sw.ElapsedMs() < 500) {
            all = true;
            for (uint32_t i = 0; i < pool.Count(); ++i) all = all && BufferRefs(pool, i) == 1;
            if (!all) Sleep(5);
        }
        CT_CHECK(all);
        std::printf("  all slots reusable %.1f ms after Finalize\n", sw.ElapsedMs());
    }
    ProbeResult p;
    CT_REQUIRE(SUCCEEDED(Probe(path, p)));
    std::printf("  probe: %lld us, %dx%d, %d/%d fps, %d video + %d audio, %s, audio %d Hz x %d, %llu bytes\n",
                static_cast<long long>(p.duration_us), p.video_width, p.video_height, p.fps_num, p.fps_den, p.video_streams,
                p.audio_streams, p.video_codec.c_str(), p.audio_sample_rate, p.audio_channels,
                static_cast<unsigned long long>(FileSize(path)));
    std::string order;
    for (const std::string& c : p.stream_codecs) order += c + " ";
    std::printf("  stream order: %s\n", order.c_str());
    CT_REQUIRE(p.stream_codecs.size() == 3);
    CT_CHECK_EQ(p.stream_codecs[0], std::string("h264"));
    CT_CHECK_EQ(p.stream_codecs[1], std::string("aac"));
    CT_CHECK_NEAR(p.duration_us, 2'000'000, 50'000);
    CT_CHECK_EQ(p.video_streams, 1);
    CT_CHECK_EQ(p.audio_streams, 2);
    CT_CHECK_EQ(p.video_width, 1920);
    CT_CHECK_EQ(p.video_height, 1080);
    CT_CHECK_EQ(p.fps_num, 60);
    CT_CHECK_EQ(p.fps_den, 1);
    CT_CHECK_EQ(p.video_codec, std::string("h264"));
    CT_CHECK_EQ(p.audio_sample_rate, 48000);
    StreamScan v, a1, a2;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 1, a1)));
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 2, a2)));
    std::printf("  video %llu samples (%llu key) %lld..%lld us; mic %llu samples %lld..%lld us; system %lld..%lld us\n",
                static_cast<unsigned long long>(v.samples), static_cast<unsigned long long>(v.keyframes), static_cast<long long>(v.first_us),
                static_cast<long long>(v.end_us), static_cast<unsigned long long>(a1.samples), static_cast<long long>(a1.first_us),
                static_cast<long long>(a1.end_us), static_cast<long long>(a2.first_us), static_cast<long long>(a2.end_us));
    CT_CHECK_EQ(v.samples, 120ull);
    CT_CHECK(v.keyframes >= 1);
    CT_CHECK_NEAR(v.first_us, 0, 1000);
    CT_CHECK_NEAR(a1.end_us, 2'000'000, 25'000);
    // Stream 1 is the mic (128 kbps), stream 2 the system track (192 kbps): ~32 KB vs ~48 KB for 2 s.
    std::printf("  mic %llu bytes, system %llu bytes\n", static_cast<unsigned long long>(a1.bytes), static_cast<unsigned long long>(a2.bytes));
    CT_CHECK(a1.bytes * 5 < a2.bytes * 4);
    // The encoder read the textures: decoded luma follows the fill values (16 + 7n mod 220, a new value every 2nd frame).
    // Checked with the software decoder and the hardware (DXVA) decoder.
    for (int hw = 0; hw < 2; ++hw) {
        std::vector<double> means;
        CT_REQUIRE(DecodeLumaMeans(path, 8, means, hw ? dev.MfManager() : nullptr));
        CT_REQUIRE(means.size() == 8);
        for (size_t i = 0; i < means.size(); ++i) {
            const int n = static_cast<int>(i) / 2 * 2;   // repeats carry the previous frame
            CT_CHECK_NEAR(means[i], 16 + (n * 7) % 220, 3.0);
        }
    }
}

CT_TEST_DEVICE(mp4_writer_software_encoder) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    TexturePool pool;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 1280, 720, 8, false, nullptr));
    const std::string path = TempPath("writer_sw.mp4");
    Mp4Writer::Config c = BaseConfig(path, 1280, 720, 30, 1);
    c.video.prefer_hardware = false;
    {
        Mp4Writer w;
        CT_REQUIRE(SUCCEEDED(w.Init(dev, c, &pool)));
        std::printf("  encoder %s (%s)\n", w.EncoderName().c_str(), w.IsHardware() ? "hardware" : "software");
        CT_CHECK(!w.IsHardware());
        CT_REQUIRE(w.Start());
        const SyntheticRun r = Feed(dev, w, &pool, 30, 30, 1, 0, 1, 0);
        CT_CHECK_HR(w.Finalize());
        CT_CHECK_EQ(r.frames_rejected, 0);
        CT_CHECK_EQ(w.GetStats().video_written.load(), 30ull);
    }
    StreamScan v;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_CHECK_EQ(v.samples, 30ull);
    std::vector<double> means;
    CT_REQUIRE(DecodeLumaMeans(path, 4, means));
    CT_REQUIRE(means.size() == 4);
    for (size_t i = 0; i < means.size(); ++i) CT_CHECK_NEAR(means[i], 16 + (static_cast<int>(i) * 7) % 220, 3.0);
}

namespace {
// First frame index whose |sample| exceeds `thr` (channel 0), or -1.
int64_t Onset(const std::vector<float>& pcm, int ch, float thr) {
    for (size_t i = 0; i < pcm.size(); i += static_cast<size_t>(ch))
        if (std::fabs(pcm[i]) > thr) return static_cast<int64_t>(i / static_cast<size_t>(ch));
    return -1;
}
double RmsDb(const std::vector<float>& pcm, int ch, int64_t from, int64_t to) {
    double s = 0;
    int64_t n = 0;
    for (int64_t f = from; f < to && static_cast<size_t>(f * ch) < pcm.size(); ++f, ++n) s += double(pcm[static_cast<size_t>(f * ch)]) * pcm[static_cast<size_t>(f * ch)];
    return n ? 10.0 * std::log10(std::max(s / static_cast<double>(n), 1e-12)) : -120.0;
}
} // namespace

// Audio starts at 300 ms, video at 500 ms: audio is padded with silence from 0, the video keeps its offset, and the
// decoded tone starts where it was captured.
CT_TEST_DEVICE(mp4_writer_audio_lead_pad_and_video_offset) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    TexturePool pool;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 1280, 720, 8, false, nullptr));
    const std::string path = TempPath("writer_offsets.mp4");
    {
        Mp4Writer w;
        CT_REQUIRE(SUCCEEDED(w.Init(dev, BaseConfig(path, 1280, 720, 60, 2), &pool)));
        CT_REQUIRE(w.Start());
        const SyntheticRun r = Feed(dev, w, &pool, 60, 60, 1, 30, 2, 300'000);   // video ticks 30..89 = 0.5..1.5 s
        CT_CHECK_HR(w.Finalize());
        CT_CHECK_EQ(r.audio_rejected, 0);
        const auto& st = w.GetStats();
        std::printf("  lead pad %llu / %llu frames, audio written %llu frames\n",
                    static_cast<unsigned long long>(st.audio_lead_pad_frames[0].load()),
                    static_cast<unsigned long long>(st.audio_lead_pad_frames[1].load()),
                    static_cast<unsigned long long>(st.audio_written[0].load()));
        CT_CHECK_EQ(st.audio_lead_pad_frames[0].load(), 14'400ull);
        CT_CHECK_EQ(st.audio_lead_pad_frames[1].load(), 14'400ull);
        CT_CHECK_EQ(st.audio_written[0].load(), 57'600ull);   // 0.3 .. 1.5 s
    }
    StreamScan v, a;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 1, a)));
    std::printf("  video %llu frames first %lld us end %lld us; audio first %lld us end %lld us\n",
                static_cast<unsigned long long>(v.samples), static_cast<long long>(v.first_us), static_cast<long long>(v.end_us),
                static_cast<long long>(a.first_us), static_cast<long long>(a.end_us));
    CT_CHECK_EQ(v.samples, 60ull);
    CT_CHECK_NEAR(v.first_us, 500'000, 1'000);   // the video's start offset survives in the file
    CT_CHECK_NEAR(a.first_us, 0, 1'000);
    std::vector<float> pcm;
    int ch = 0;
    int64_t first = 0;
    CT_REQUIRE(DecodeAudio(path, 0, pcm, ch, first));
    CT_REQUIRE(ch == 2);
    const int64_t onset = Onset(pcm, ch, 0.05f);
    const double tone_db = RmsDb(pcm, ch, 24'000, 48'000);   // 0.5 .. 1.0 s
    const double quiet_db = RmsDb(pcm, ch, 0, 13'000);
    std::printf("  decoded %zu frames from %lld us; tone onset at %.2f ms; tone %.2f dB RMS (expect -15.05), lead %.1f dB\n",
                pcm.size() / 2, static_cast<long long>(first), onset * 1000.0 / 48000, tone_db, quiet_db);
    CT_CHECK_NEAR(static_cast<double>(onset) + static_cast<double>(first) * 0.048, 14'400, 48);   // within 1 ms
    CT_CHECK_NEAR(tone_db, -15.05, 0.5);   // -12 dBFS sine = -15.05 dB RMS
    CT_CHECK(quiet_db < -80);
}

// Audio-only file with a 50 ms hole in the packets (a producer that dropped data): the gap becomes silence at the
// right position and nothing after it shifts.
CT_TEST_DEVICE(mp4_writer_audio_only_gap_fill) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    const std::string path = TempPath("audio_gap.m4a");
    Mp4Writer::Config c;
    c.path = path;
    c.has_video = false;
    c.audio.push_back(AudioEncodeConfig{});
    Mp4Writer w;
    CT_REQUIRE(SUCCEEDED(w.Init(dev, c, nullptr)));
    CT_REQUIRE(w.Start());
    std::vector<float> pcm(480 * 2);
    for (int k = 0; k < 100; ++k) {   // 1 s of 10 ms packets, packets 40..44 missing
        if (k >= 40 && k < 45) continue;
        Sine(pcm.data(), 480, 1000.0, 0.25f, k * 480ll);
        audio::AudioPacket p;
        p.t_us = k * 10'000;
        p.frames = 480;
        p.data = pcm.data();
        CT_CHECK(w.OnAudio(0, p));
        if (k % 10 == 9) Sleep(5);
    }
    CT_CHECK_HR(w.Finalize());
    CT_CHECK_EQ(w.GetStats().audio_written[0].load(), 45'600ull);
    ProbeResult p;
    CT_REQUIRE(SUCCEEDED(Probe(path, p)));
    CT_CHECK_EQ(p.video_streams, 0);
    CT_CHECK_EQ(p.audio_streams, 1);
    std::vector<float> dec;
    int ch = 0;
    int64_t first = 0;
    CT_REQUIRE(DecodeAudio(path, 0, dec, ch, first));
    const double before = RmsDb(dec, ch, 9'600, 19'000), gap = RmsDb(dec, ch, 19'700, 21'300), after = RmsDb(dec, ch, 22'000, 40'000);
    std::printf("  %zu frames decoded (first %lld us); RMS before %.1f, gap %.1f, after %.1f dB; duration %lld us\n", dec.size() / 2,
                static_cast<long long>(first), before, gap, after, static_cast<long long>(p.duration_us));
    CT_CHECK_NEAR(before, -15.05, 0.5);
    CT_CHECK(gap < -40);
    CT_CHECK_NEAR(after, -15.05, 0.5);
    CT_CHECK_NEAR(p.duration_us, 1'000'000, 25'000);
}
