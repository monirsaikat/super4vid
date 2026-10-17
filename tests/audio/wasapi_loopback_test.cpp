#include <ct/audio/wasapi_loopback.h>
#include <ct/audio/wasapi_render.h>

#include "audio_test_util.h"

#include <ct/audio/meter.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ct::audio;

namespace {

struct SilenceSource : WasapiRender::Callback {
    std::atomic<int64_t> frames{0};
    std::atomic<bool> position_ok{true};
    void Render(float* p, uint32_t n, int ch, int64_t pos) override {
        if (pos != frames.load()) position_ok.store(false);
        std::memset(p, 0, sizeof(float) * n * ch);
        frames.fetch_add(n);
    }
};

struct ToneSource : WasapiRender::Callback {
    double phase = 0;
    float amplitude = 0.25118864f;   // -12 dBFS
    void Render(float* p, uint32_t n, int ch, int64_t) override {
        const double step = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
        for (uint32_t i = 0; i < n; ++i) {
            const float v = amplitude * static_cast<float>(std::sin(phase));
            phase += step;
            if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
            for (int c = 0; c < ch; ++c) p[i * ch + c] = v;
        }
    }
};

bool HasRenderEndpoint(AudioDevices& devices) { return !devices.DefaultId(false).empty(); }

} // namespace

CT_TEST_DEVICE(wasapi_render_plays_500ms_silence) {
    test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    if (!HasRenderEndpoint(devices)) CT_SKIP("no render endpoint");
    WasapiRender render;
    const HRESULT hr = render.Init(L"", 20);
    CT_CHECK_HR(hr);
    CT_REQUIRE(SUCCEEDED(hr));
    CT_CHECK_EQ(render.SampleRate(), 48000u);
    CT_CHECK_EQ(render.Channels(), 2);
    CT_CHECK(render.LatencyMs() >= 10.0 && render.LatencyMs() < 200.0);
    SilenceSource src;
    CT_REQUIRE(render.Start(src));
    Sleep(500);
    int64_t frames = 0, qpc = 0;
    CT_CHECK_HR(render.GetClock(frames, qpc));
    render.Stop();
    if (ct::test::Verbose())
        std::printf("  render \"%s\": latency %.1f ms, written %lld, clock %lld frames\n", render.DeviceName().c_str(),
                    render.LatencyMs(), static_cast<long long>(src.frames.load()), static_cast<long long>(frames));
    CT_CHECK(render.LastError() == S_OK);
    CT_CHECK(src.position_ok.load());
    // ~500 ms played (+ the prefilled buffer); the device clock advanced by about the same.
    CT_CHECK_NEAR(static_cast<double>(src.frames.load()), 24000.0, 4800.0);
    CT_CHECK_NEAR(static_cast<double>(frames), 24000.0, 4800.0);
    CT_CHECK(qpc > 0 && qpc <= ct::Clock::QpcNow());
    // Restartable.
    CT_CHECK(render.Start(src));
    render.Stop();
}

CT_TEST_DEVICE(wasapi_loopback_records_tone_2s) {
    test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    if (!HasRenderEndpoint(devices)) CT_SKIP("no render endpoint");
    WasapiRender tone_out;
    CT_REQUIRE(SUCCEEDED(tone_out.Init(L"", 40)));
    ToneSource tone;
    CT_REQUIRE(tone_out.Start(tone));
    Sleep(200);   // let the tone reach the endpoint before recording

    WasapiLoopback loop;
    CaptureConfig cfg;
    cfg.stream_id = 2;
    const HRESULT hr = loop.Init(cfg, devices);
    CT_CHECK_HR(hr);
    CT_REQUIRE(SUCCEEDED(hr));
    test::CheckingSink sink;
    sink.peak_after_frames = 4800;   // skip the first 100 ms
    const int64_t epoch = ct::Clock::QpcNow();
    CT_REQUIRE(loop.Start(epoch, sink));
    Sleep(2000);
    const ct::TimeUs stop_us = ct::Clock::QpcToUs(ct::Clock::QpcNow(), epoch);
    const float meter_db = loop.Levels().PeakDb(0);
    loop.Stop();
    tone_out.Stop();
    const CaptureStats st = loop.GetStats();
    const double peak_db = Meter::ToDb(sink.peak.load());
    const ct::TimeUs latency_us = stop_us - st.last_device_end_us;
    if (ct::test::Verbose())
        std::printf("  loopback \"%s\": %lld frames in %lld packets (%llu device), pad %llu, inserted %llu, dropped %llu,"
                    " drift %.3f ms, latency %.1f ms, peak %.2f dB (meter %.2f)\n",
                    loop.DeviceName().c_str(), static_cast<long long>(sink.frames.load()),
                    static_cast<long long>(sink.packets.load()), static_cast<unsigned long long>(st.device_packets),
                    static_cast<unsigned long long>(st.leading_pad_frames),
                    static_cast<unsigned long long>(st.frames_inserted),
                    static_cast<unsigned long long>(st.frames_dropped), st.drift_ms, latency_us / 1000.0, peak_db,
                    meter_db);
    CT_CHECK(sink.contiguous.load());
    CT_CHECK(!sink.bad_format.load());
    CT_CHECK_EQ(sink.errors.load(), 0L);
    CT_CHECK(loop.LastError() == S_OK);
    // Delivered frames == the media time of the last device frame, within one packet. Loopback timestamps are the
    // endpoint's presentation times, so the last one can lie up to the render latency *after* the stop time.
    CT_CHECK(latency_us > -100'000 && latency_us < 50'000);
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(st.last_device_end_us) * 0.048, 480.0);
    // ~2 s recorded (Start/Stop overhead and the presentation offset are the only differences).
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(stop_us) * 0.048, 480.0 + 100 * 48.0);
    // No gaps after the first packet.
    CT_CHECK_EQ(st.frames_inserted, 0ull);
    CT_CHECK_EQ(sink.silence_frames.load(), static_cast<int64_t>(st.leading_pad_frames));
    // Loopback records everything the endpoint plays. Other processes (e.g. parallel test runs) may add sound, which
    // only raises the peak, so require our -12 dBFS tone to be present and warn when something else was mixed in.
    if (peak_db > -11.0) std::printf("    WARN: peak %.2f dBFS: other audio was playing during the test\n", peak_db);
    CT_CHECK(peak_db >= -13.0);
}

CT_TEST_DEVICE(wasapi_loopback_keepalive_without_playback) {
    // Nothing else plays: the keep-alive stream keeps loopback packets coming, so no starvation fill is needed.
    test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    if (!HasRenderEndpoint(devices)) CT_SKIP("no render endpoint");
    WasapiLoopback loop;
    CT_REQUIRE(SUCCEEDED(loop.Init(CaptureConfig{}, devices)));
    test::CheckingSink sink;
    CT_REQUIRE(loop.Start(ct::Clock::QpcNow(), sink));
    Sleep(1000);
    loop.Stop();
    const CaptureStats st = loop.GetStats();
    CT_CHECK(sink.contiguous.load());
    CT_CHECK(st.device_packets > 80);
    CT_CHECK_EQ(st.frames_inserted, 0ull);
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(st.last_device_end_us) * 0.048, 480.0);
}

CT_TEST_DEVICE(wasapi_loopback_pause_shifts_timeline) {
    test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    if (!HasRenderEndpoint(devices)) CT_SKIP("no render endpoint");
    WasapiLoopback loop;
    CT_REQUIRE(SUCCEEDED(loop.Init(CaptureConfig{}, devices)));
    test::CheckingSink sink;
    const int64_t epoch = ct::Clock::QpcNow();
    CT_REQUIRE(loop.Start(epoch, sink));
    Sleep(400);
    const int64_t pause_qpc = ct::Clock::QpcNow();
    loop.SetPaused(true, pause_qpc);
    Sleep(300);
    const int64_t resume_qpc = ct::Clock::QpcNow();
    loop.SetPaused(false, resume_qpc);
    Sleep(400);
    const ct::TimeUs stop_us = ct::Clock::QpcToUs(ct::Clock::QpcNow(), epoch);
    loop.Stop();
    const CaptureStats st = loop.GetStats();
    const ct::TimeUs paused_us = ct::Clock::QpcToUs(resume_qpc, pause_qpc);
    if (ct::test::Verbose())
        std::printf("  pause: %lld frames, paused %.1f ms, stop %.1f ms, inserted %llu, dropped %llu\n",
                    static_cast<long long>(sink.frames.load()), paused_us / 1000.0, stop_us / 1000.0,
                    static_cast<unsigned long long>(st.frames_inserted),
                    static_cast<unsigned long long>(st.frames_dropped));
    CT_CHECK(sink.contiguous.load());
    // Media time excludes the pause: ~ (stop - pause) of audio, give or take the presentation offset.
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(stop_us - paused_us) * 0.048, 100 * 48.0);
    // The resume lands exactly (at most the few ms that straddle the boundary are inserted or dropped).
    CT_CHECK(st.frames_inserted + st.frames_dropped < 20 * 48);
}