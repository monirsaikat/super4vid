// Device switching without timeline gaps or shifts (task 3.5). Real unplug / default-change events need a human
// (see PROGRESS.md); these tests drive the same reopen path through SwitchDevice(), which the IMMNotificationClient
// listener and AUDCLNT_E_DEVICE_INVALIDATED also use.
#include <ct/audio/wasapi_loopback.h>
#include <ct/audio/wasapi_render.h>

#include "audio_test_util.h"

#include <ct/audio/meter.h>
#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdio>

using namespace ct::audio;

namespace {
struct Tone : WasapiRender::Callback {
    double phase = 0;
    void Render(float* p, uint32_t n, int ch, int64_t) override {
        for (uint32_t i = 0; i < n; ++i) {
            const float v = 0.25118864f * static_cast<float>(std::sin(phase));
            phase = std::fmod(phase + 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0, 2.0 * 3.14159265358979323846);
            for (int c = 0; c < ch; ++c) p[i * ch + c] = v;
        }
    }
};
} // namespace

CT_TEST_DEVICE(hotswap_loopback_reopen_keeps_timeline) {
    test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    if (devices.DefaultId(false).empty()) CT_SKIP("no render endpoint");
    WasapiRender out;
    CT_REQUIRE(SUCCEEDED(out.Init(L"", 40)));
    Tone tone;
    CT_REQUIRE(out.Start(tone));
    Sleep(150);
    WasapiLoopback loop;
    CT_REQUIRE(SUCCEEDED(loop.Init(CaptureConfig{}, devices)));
    test::CheckingSink sink;
    sink.peak_after_frames = 4800;
    const int64_t epoch = ct::Clock::QpcNow();
    CT_REQUIRE(loop.Start(epoch, sink));
    Sleep(600);
    loop.SwitchDevice(L"");   // what a default-device change does
    Sleep(600);
    loop.Stop();
    out.Stop();
    const CaptureStats st = loop.GetStats();
    if (ct::test::Verbose())
        std::printf("  reopen took %.1f ms; %lld frames, inserted %llu (silence %lld), dropped %llu, reinits %llu\n",
                    st.last_reopen_ms, static_cast<long long>(sink.frames.load()),
                    static_cast<unsigned long long>(st.frames_inserted),
                    static_cast<long long>(sink.silence_frames.load()),
                    static_cast<unsigned long long>(st.frames_dropped), static_cast<unsigned long long>(st.reinits));
    CT_CHECK(sink.contiguous.load());
    CT_CHECK_EQ(st.reinits, 1ull);
    CT_CHECK_EQ(sink.errors.load(), 0L);
    // The switch gap became silence (bounded by the switch time plus device start-up), and the stream position still
    // equals the device clock afterwards: no shift.
    CT_CHECK(st.frames_inserted < 200 * 48);
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(st.last_device_end_us) * 0.048, 480.0);
    // Loopback records everything the endpoint plays. Other processes (e.g. parallel test runs) may add sound, which
    // only raises the peak, so require our -12 dBFS tone to be present and warn when something else was mixed in.
    const double peak_db = Meter::ToDb(sink.peak.load());
    if (peak_db > -11.0) std::printf("    WARN: peak %.2f dBFS: other audio was playing during the test\n", peak_db);
    CT_CHECK(peak_db >= -13.0);
}

CT_TEST_DEVICE(hotswap_missing_device_fills_silence_and_recovers) {
    test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    if (devices.DefaultId(false).empty()) CT_SKIP("no render endpoint");
    WasapiLoopback loop;
    CaptureConfig cfg;
    cfg.follow_default_device = false;   // stay on the (missing) endpoint and keep retrying
    CT_REQUIRE(SUCCEEDED(loop.Init(cfg, devices)));
    test::CheckingSink sink;
    const int64_t epoch = ct::Clock::QpcNow();
    CT_REQUIRE(loop.Start(epoch, sink));
    Sleep(300);
    loop.SwitchDevice(L"{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}");   // like an unplugged device
    Sleep(800);
    const bool lost = FAILED(loop.LastError()) && !loop.GetStats().device_open;
    const int64_t frames_while_lost = sink.frames.load();
    loop.SwitchDevice(L"");   // the device comes back (or the user picks another one)
    Sleep(400);
    const ct::TimeUs stop_us = ct::Clock::QpcToUs(ct::Clock::QpcNow(), epoch);
    loop.Stop();
    const CaptureStats st = loop.GetStats();
    if (ct::test::Verbose())
        std::printf("  lost=%d, %lld frames at 1.1 s, silence %lld, final %lld frames / stop %.1f ms, reinits %llu\n",
                    lost ? 1 : 0, static_cast<long long>(frames_while_lost),
                    static_cast<long long>(sink.silence_frames.load()), static_cast<long long>(sink.frames.load()),
                    stop_us / 1000.0, static_cast<unsigned long long>(st.reinits));
    CT_CHECK(lost);
    CT_CHECK_EQ(sink.errors.load(), 1L);   // reported once, not per retry
    CT_CHECK(sink.contiguous.load());
    // While no device delivered, time kept running with silence (starvation fill lags real time by <= 150 ms).
    CT_CHECK(frames_while_lost > (1100 - 200) * 48);
    CT_CHECK(sink.silence_frames.load() > 500 * 48);
    CT_CHECK(SUCCEEDED(loop.LastError()));
    CT_CHECK(st.device_open);
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(st.last_device_end_us) * 0.048, 480.0);
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(stop_us) * 0.048, 100 * 48.0);
}
