// Pause/resume decided by capture time (review fix). Synthetic device packets go through PauseTimeline::Map and the
// StreamAligner exactly as CaptureEngine::ProcessDeviceFrames does; Pause()/Resume() calls are interleaved with the
// packet reads at the moment they would happen in real time (a packet is read ~15 ms after its last frame), so a
// packet captured before a call but read after it is the common case, not an exception.
#include <ct/audio/pause_timeline.h>
#include <ct/audio/stream_aligner.h>

#include <ct/test/ct_test.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using ct::TimeUs;
using ct::audio::PauseTimeline;
using ct::audio::StreamAligner;

namespace {

constexpr int kRate = 48000;
constexpr uint32_t kPacket = 480;
constexpr int64_t kEpochAbs = 5'000'000'000;   // arbitrary absolute QPC microseconds
constexpr int64_t kReadLatencyUs = 15'000;

struct Action {
    int64_t call_abs;   // when the owner thread calls it
    bool pause;
    int64_t at_abs;     // the time it records (at_qpc)
};

struct Result {
    std::vector<float> values;   // input frame index carried by every emitted frame (-1 = inserted silence)
    std::vector<TimeUs> times;   // media time of every emitted frame
    int64_t inserted = 0, dropped = 0, leading = 0;
    bool contiguous = true;
};

int64_t CaptureAbs(int64_t i) { return kEpochAbs + i * 1'000'000 / kRate; }   // frame i of the synthetic device

Result Run(int packets, std::vector<Action> actions) {
    PauseTimeline pauses;
    pauses.Reset();
    StreamAligner aligner;
    aligner.Reset(kRate, 2);
    Result res;
    std::vector<float> pkt(kPacket * 2);
    size_t next_action = 0;
    bool force_resync = false;
    auto emit = [&](TimeUs t, const float* p, uint32_t n, bool fill) {
        if (t != static_cast<TimeUs>(res.values.size()) * 1'000'000 / kRate) res.contiguous = false;
        for (uint32_t i = 0; i < n; ++i) {
            res.times.push_back(static_cast<TimeUs>(res.values.size()) * 1'000'000 / kRate);
            res.values.push_back(fill ? -1.0f : p[2 * i]);
        }
    };
    for (int k = 0; k < packets; ++k) {
        const int64_t first = static_cast<int64_t>(k) * kPacket;
        const int64_t r0 = CaptureAbs(first);
        const int64_t read_abs = CaptureAbs(first + kPacket) + kReadLatencyUs;
        while (next_action < actions.size() && actions[next_action].call_abs <= read_abs) {
            const Action& a = actions[next_action++];
            if (a.pause) pauses.Pause(a.at_abs);
            else pauses.Resume(a.at_abs);
        }
        for (uint32_t i = 0; i < kPacket; ++i) pkt[2 * i] = pkt[2 * i + 1] = static_cast<float>(first + i);
        uint32_t next_expected = 0;
        pauses.Map(r0, kPacket, kRate, [&](uint32_t f, uint32_t count, int64_t media_abs) {
            const bool resync = force_resync || f != next_expected;
            force_resync = false;
            aligner.Push(media_abs - kEpochAbs, pkt.data() + 2 * f, count, resync, emit);
            next_expected = f + count;
        });
        if (next_expected < kPacket) force_resync = true;
    }
    res.inserted = aligner.TotalInserted();
    res.dropped = aligner.TotalDropped();
    res.leading = aligner.LeadingPadFrames();
    return res;
}

// Every input frame captured outside [b, e) must appear exactly once, in order, at media time capture - offset.
void CheckAgainstIntervals(const Result& r, int64_t total_frames, const std::vector<std::pair<int64_t, int64_t>>& iv) {
    CT_CHECK(r.contiguous);
    CT_CHECK_EQ(r.leading, int64_t(0));
    CT_CHECK(r.inserted + r.dropped <= 1);   // at most a sub-frame rounding at a boundary
    std::vector<int64_t> expected;
    for (int64_t i = 0; i < total_frames; ++i) {
        const int64_t c = CaptureAbs(i);
        bool in_pause = false;
        for (const auto& p : iv) in_pause = in_pause || (c >= p.first && c < p.second);
        if (!in_pause) expected.push_back(i);
    }
    // Kept frames must be strictly increasing; compared with `expected`, each pause boundary may lose or repeat at
    // most one frame (the aligner snaps a resumed run that lands between two frame slots).
    std::vector<int64_t> got;
    bool increasing = true;
    int64_t max_time_err = 0;
    for (size_t j = 0; j < r.values.size(); ++j) {
        if (r.values[j] < 0) continue;
        const int64_t idx = static_cast<int64_t>(r.values[j]);
        if (!got.empty() && idx <= got.back()) increasing = false;
        got.push_back(idx);
        int64_t offset = 0;
        for (const auto& p : iv)
            if (p.second <= CaptureAbs(idx)) offset += p.second - p.first;
        const int64_t err = r.times[j] - (CaptureAbs(idx) - kEpochAbs - offset);
        max_time_err = err < 0 ? (-err > max_time_err ? -err : max_time_err) : (err > max_time_err ? err : max_time_err);
    }
    size_t a = 0, b = 0, missing = 0, extra = 0;
    while (a < expected.size() || b < got.size()) {
        if (b == got.size() || (a < expected.size() && expected[a] < got[b])) { ++missing; ++a; }
        else if (a == expected.size() || got[b] < expected[a]) { ++extra; ++b; }
        else { ++a; ++b; }
    }
    if (ct::test::Verbose())
        std::printf("  kept %zu of %zu expected, missing %zu, extra %zu, max time error %lld us\n", got.size(),
                    expected.size(), missing, extra, static_cast<long long>(max_time_err));
    CT_CHECK(increasing);
    CT_CHECK_EQ(extra, size_t(0));
    CT_CHECK(missing <= 2 * iv.size());    CT_CHECK(max_time_err <= 21 + 21);   // within one frame (+1 frame for the boundary correction)
}

} // namespace

CT_TEST(pause_keeps_frames_captured_before_pause_and_after_resume) {
    // Pause at 1.234567 s and resume at 3.456789 s (both mid-packet). Each call happens at the time it records, so
    // the packet straddling the pause start is read after Pause() and the one straddling the resume after Resume().
    const int64_t b = kEpochAbs + 1'234'567, e = kEpochAbs + 3'456'789;
    const Result r = Run(600, {{b, true, b}, {e, false, e}});
    CheckAgainstIntervals(r, 600 * kPacket, {{b, e}});
    // Nothing lost before the pause: the old code dropped the whole straddling packet (and the next one).
    CT_CHECK(r.values.size() >= static_cast<size_t>(600 * kPacket - (e - b) * 48 / 1000 - 1));
}

CT_TEST(pause_resume_call_racing_packet_reads) {
    // The resume is recorded at e but the call reaches the engine 25 ms later (the session passes one QPC to every
    // stream): packets captured after e were already read while the pause still looked open. Media time must still
    // not shift: the frames read too early are dropped, everything later lands at capture - (e - b).
    const int64_t b = kEpochAbs + 500'003, e = kEpochAbs + 900'017;
    const Result r = Run(200, {{b, true, b}, {e + 25'000, false, e}});
    CT_CHECK(r.contiguous);
    int64_t max_time_err = 0;
    for (size_t j = 0; j < r.values.size(); ++j) {
        if (r.values[j] < 0) continue;
        const int64_t idx = static_cast<int64_t>(r.values[j]);
        const int64_t c = CaptureAbs(idx);
        CT_CHECK(c < b || c >= e);
        const int64_t want = c - kEpochAbs - (c >= e ? e - b : 0);
        const int64_t err = r.times[j] - want;
        if ((err < 0 ? -err : err) > max_time_err) max_time_err = err < 0 ? -err : err;
    }
    // The late call costs the frames read in those 25 ms (silence), but nothing after them moves.
    CT_CHECK(max_time_err <= 42);
    CT_CHECK(r.inserted <= 25 * 48 + 48);
    CT_CHECK_EQ(r.dropped, int64_t(0));
    // Without a late call there is no silence at all (previous test); here the stream length is still exact.
    CT_CHECK_NEAR(static_cast<double>(r.values.size()), 200.0 * kPacket - (e - b) * 0.048, 2.0);
}

CT_TEST(pause_short_pauses_inside_one_packet_and_back_to_back) {
    // A 3 ms pause inside one 10 ms packet, then two pauses 40 ms apart.
    const int64_t p1 = kEpochAbs + 100'002, q1 = kEpochAbs + 103'005;
    const int64_t p2 = kEpochAbs + 200'000, q2 = kEpochAbs + 260'000;
    const int64_t p3 = kEpochAbs + 300'000, q3 = kEpochAbs + 1'300'000;
    const Result r = Run(200, {{p1, true, p1}, {q1, false, q1}, {p2, true, p2}, {q2, false, q2}, {p3, true, p3},
                               {q3, false, q3}});
    CheckAgainstIntervals(r, 200 * kPacket, {{p1, q1}, {p2, q2}, {p3, q3}});
}

CT_TEST(pause_timeline_peek_and_ignored_calls) {
    PauseTimeline t;
    t.Reset();
    int64_t m = 0;
    CT_CHECK(t.PeekMedia(1000, &m));
    CT_CHECK_EQ(m, int64_t(1000));
    t.Resume(50);   // not paused: ignored
    t.Pause(2000);
    t.Pause(2500);  // already paused: ignored
    CT_CHECK(t.IsPaused());
    CT_CHECK(!t.PeekMedia(3000, &m));   // open pause
    t.Resume(4000);
    CT_CHECK(!t.IsPaused());
    CT_CHECK(!t.PeekMedia(3000, &m));   // inside the closed pause
    CT_CHECK(t.PeekMedia(5000, &m));
    CT_CHECK_EQ(m, int64_t(3000));
    CT_CHECK(t.PeekMedia(1500, &m));    // before the pause: unchanged
    CT_CHECK_EQ(m, int64_t(1500));
}
