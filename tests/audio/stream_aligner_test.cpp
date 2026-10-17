#include <ct/audio/stream_aligner.h>

#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdint>
#include <vector>

using ct::TimeUs;
using ct::audio::StreamAligner;

namespace {

constexpr int kRate = 48000;
constexpr uint32_t kPacket = 480;

// Records the emitted stream and checks that it is exactly contiguous.
struct Collector {
    int64_t frames = 0;
    int64_t silence_frames = 0;
    int64_t calls = 0;
    bool contiguous = true;
    std::vector<float> first_samples;   // first sample of every emitted chunk

    TimeUs Expected() const { return frames * 1'000'000 / kRate; }
    auto Fn() {
        return [this](TimeUs t, const float* p, uint32_t n, bool fill) {
            if (t != Expected()) contiguous = false;
            frames += n;
            if (fill) silence_frames += n;
            ++calls;
            first_samples.push_back(p[0]);
        };
    }
};

std::vector<float> Packet(float value) { return std::vector<float>(kPacket * 2, value); }

} // namespace

CT_TEST(aligner_perfect_stream_is_unchanged) {
    StreamAligner a;
    a.Reset(kRate, 2);
    Collector c;
    const auto data = Packet(0.5f);
    for (int k = 0; k < 1000; ++k) a.Push(TimeUs(k) * 10'000, data.data(), kPacket, false, c.Fn());
    CT_CHECK(c.contiguous);
    CT_CHECK_EQ(c.frames, int64_t(1000) * kPacket);
    CT_CHECK_EQ(c.silence_frames, int64_t(0));
    CT_CHECK_EQ(a.TotalInserted(), int64_t(0));
    CT_CHECK_EQ(a.TotalDropped(), int64_t(0));
    CT_CHECK_EQ(a.LeadingPadFrames(), int64_t(0));
    CT_CHECK_EQ(a.NextExpectedUs(), TimeUs(10'000'000));
    CT_CHECK_NEAR(a.DriftMs(), 0.0, 1e-9);
}

CT_TEST(aligner_pads_from_zero_before_first_packet) {
    StreamAligner a;
    a.Reset(kRate, 2);
    Collector c;
    const auto data = Packet(0.5f);
    a.Push(25'000, data.data(), kPacket, false, c.Fn());   // first packet 25 ms after the epoch
    CT_CHECK(c.contiguous);
    CT_CHECK_EQ(a.LeadingPadFrames(), int64_t(1200));
    CT_CHECK_EQ(a.TotalInserted(), int64_t(0));
    CT_CHECK_EQ(c.silence_frames, int64_t(1200));
    CT_CHECK_EQ(c.frames, int64_t(1200 + kPacket));
    // Data before the epoch is dropped exactly.
    StreamAligner b;
    b.Reset(kRate, 2);
    Collector d;
    b.Push(-5'000, data.data(), kPacket, false, d.Fn());
    CT_CHECK_EQ(b.TotalDropped(), int64_t(240));
    CT_CHECK_EQ(d.frames, int64_t(240));
    CT_CHECK(d.contiguous);
}

CT_TEST(aligner_fills_15ms_gap_exactly) {
    StreamAligner a;
    a.Reset(kRate, 2);
    Collector c;
    const auto data = Packet(0.5f);
    for (int k = 0; k < 10; ++k) a.Push(TimeUs(k) * 10'000, data.data(), kPacket, false, c.Fn());
    a.Push(100'000 + 15'000, data.data(), kPacket, false, c.Fn());   // 15 ms of packets missing
    CT_CHECK(c.contiguous);
    CT_CHECK_EQ(a.TotalInserted(), int64_t(720));
    CT_CHECK_EQ(c.silence_frames, int64_t(720));
    CT_CHECK_EQ(a.NextExpectedUs(), TimeUs(115'000 + 10'000));
    // The stream continues normally afterwards.
    a.Push(125'000, data.data(), kPacket, false, c.Fn());
    CT_CHECK_EQ(a.TotalInserted(), int64_t(720));
    CT_CHECK_EQ(c.frames, int64_t(12) * kPacket + 720);
}

CT_TEST(aligner_drops_3ms_overlap) {
    StreamAligner a;
    a.Reset(kRate, 2);
    Collector c;
    const auto data = Packet(0.5f);
    for (int k = 0; k < 10; ++k) a.Push(TimeUs(k) * 10'000, data.data(), kPacket, false, c.Fn());
    a.Push(100'000 - 3'000, data.data(), kPacket, false, c.Fn());   // repeats the last 3 ms
    CT_CHECK(c.contiguous);
    CT_CHECK_EQ(a.TotalDropped(), int64_t(144));
    CT_CHECK_EQ(c.frames, int64_t(11) * kPacket - 144);
    CT_CHECK_EQ(c.silence_frames, int64_t(0));
    // A packet entirely inside the past is dropped completely.
    a.Push(0, data.data(), kPacket, false, c.Fn());
    CT_CHECK_EQ(a.TotalDropped(), int64_t(144 + kPacket));
}

namespace {
void RunDrift(double ppm, int64_t* inserted, int64_t* dropped, double* final_drift_ms, bool* smooth) {
    StreamAligner a;
    a.Reset(kRate, 2);
    Collector c;
    const auto data = Packet(0.25f);
    *smooth = true;
    const int packets = 60'000;   // 10 minutes of 10 ms packets
    for (int k = 0; k < packets; ++k) {
        // The device clock runs at (1 + ppm) relative to QPC: each 480-frame packet spans 10 ms * (1 + ppm).
        const TimeUs t = static_cast<TimeUs>(std::llround(k * 10'000.0 * (1.0 + ppm * 1e-6)));
        const int64_t before = c.frames;
        a.Push(t, data.data(), kPacket, false, c.Fn());
        const int64_t emitted = c.frames - before;
        // Never more than one frame of correction per 10 ms packet, never silence.
        if (k > 0 && (emitted < kPacket - 1 || emitted > kPacket + 1)) *smooth = false;
    }
    if (!c.contiguous || c.silence_frames != 0) *smooth = false;
    *inserted = a.TotalInserted();
    *dropped = a.TotalDropped();
    *final_drift_ms = a.DriftMs();
}
} // namespace

CT_TEST(aligner_slow_drift_converges_smoothly) {
    int64_t inserted = 0, dropped = 0;
    double drift = 0;
    bool smooth = false;
    RunDrift(+50.0, &inserted, &dropped, &drift, &smooth);
    CT_CHECK(smooth);
    // 50 ppm over 600 s = 30 ms = 1440 frames; the aligner holds |drift| near 1 ms, so it inserts ~29 ms.
    CT_CHECK_NEAR(static_cast<double>(inserted), 1392.0, 60.0);
    CT_CHECK_EQ(dropped, int64_t(0));
    CT_CHECK(std::fabs(drift) <= 1.1);

    RunDrift(-50.0, &inserted, &dropped, &drift, &smooth);
    CT_CHECK(smooth);
    CT_CHECK_NEAR(static_cast<double>(dropped), 1392.0, 60.0);
    CT_CHECK_EQ(inserted, int64_t(0));
    CT_CHECK(std::fabs(drift) <= 1.1);
}

CT_TEST(aligner_discontinuity_resyncs_exactly) {
    const auto data = Packet(0.5f);
    {
        // 1.5 ms late without the flag: treated as drift (one frame per 10 ms).
        StreamAligner a;
        a.Reset(kRate, 2);
        Collector c;
        a.Push(0, data.data(), kPacket, false, c.Fn());
        a.Push(10'000 + 1'500, data.data(), kPacket, false, c.Fn());
        CT_CHECK_EQ(a.TotalInserted(), int64_t(1));
        CT_CHECK_EQ(c.silence_frames, int64_t(0));
    }
    {
        // The same packet with DATA_DISCONTINUITY: exact silence of 72 frames.
        StreamAligner a;
        a.Reset(kRate, 2);
        Collector c;
        a.Push(0, data.data(), kPacket, false, c.Fn());
        a.Push(10'000 + 1'500, data.data(), kPacket, true, c.Fn());
        CT_CHECK_EQ(a.TotalInserted(), int64_t(72));
        CT_CHECK_EQ(c.silence_frames, int64_t(72));
        CT_CHECK(c.contiguous);
        CT_CHECK_NEAR(a.DriftMs(), 0.0, 1e-9);
    }
}

CT_TEST(aligner_fill_silence_until_then_resume) {
    StreamAligner a;
    a.Reset(kRate, 2);
    Collector c;
    const auto data = Packet(0.5f);
    a.Push(0, data.data(), kPacket, false, c.Fn());
    a.FillSilenceUntil(200'000, c.Fn());   // starvation: no packets for a while
    CT_CHECK_EQ(a.NextExpectedUs(), TimeUs(200'000));
    CT_CHECK_EQ(a.TotalInserted(), int64_t(190 * 48));
    // Device packets resume at 230 ms: the 30 ms remainder is filled exactly, not drift-corrected.
    a.Push(230'000, data.data(), kPacket, false, c.Fn());
    CT_CHECK_EQ(a.TotalInserted(), int64_t(220 * 48));
    CT_CHECK(c.contiguous);
    CT_CHECK_EQ(c.frames, int64_t(240 * 48));
    a.FillSilenceUntil(100'000, c.Fn());   // in the past: nothing
    CT_CHECK_EQ(c.frames, int64_t(240 * 48));
}
