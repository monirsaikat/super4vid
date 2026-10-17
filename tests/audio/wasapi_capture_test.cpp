#include <ct/audio/wasapi_capture.h>

#include "audio_test_util.h"

#include <ct/core/time.h>
#include <ct/test/ct_test.h>

#include <cstdio>

using namespace ct::audio;

CT_TEST_DEVICE(wasapi_capture_default_mic_1s) {
    test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    if (devices.DefaultId(true).empty()) CT_SKIP("no capture endpoint");
    WasapiCapture mic;
    CaptureConfig cfg;
    cfg.stream_id = 7;
    const HRESULT hr = mic.Init(cfg, devices);
    CT_CHECK_HR(hr);
    CT_REQUIRE(SUCCEEDED(hr));
    CT_CHECK(!mic.DeviceName().empty());
    test::CheckingSink sink;
    const int64_t epoch = ct::Clock::QpcNow();
    CT_REQUIRE(mic.Start(epoch, sink));
    Sleep(1000);
    const ct::TimeUs stop_us = ct::Clock::QpcToUs(ct::Clock::QpcNow(), epoch);
    mic.Stop();
    const CaptureStats st = mic.GetStats();
    if (ct::test::Verbose())
        std::printf("  mic \"%s\": %lld frames, %llu device packets, pad %llu, inserted %llu, dropped %llu, drift %.3f ms\n",
                    mic.DeviceName().c_str(), static_cast<long long>(sink.frames.load()),
                    static_cast<unsigned long long>(st.device_packets),
                    static_cast<unsigned long long>(st.leading_pad_frames),
                    static_cast<unsigned long long>(st.frames_inserted),
                    static_cast<unsigned long long>(st.frames_dropped), st.drift_ms);
    CT_CHECK(sink.contiguous.load());
    CT_CHECK(!sink.bad_format.load());
    CT_CHECK_EQ(sink.errors.load(), 0L);
    CT_CHECK(st.device_packets > 50);
    // Everything up to the last device frame was delivered (minus at most one partially filled packet).
    const int64_t expected = st.last_device_end_us * 48 / 1000;
    CT_CHECK_NEAR(static_cast<double>(sink.frames.load()), static_cast<double>(expected), 480.0 + 48.0);
    CT_CHECK(stop_us - st.last_device_end_us < 60'000);   // latency from capture to delivery
    CT_CHECK(mic.LastError() == S_OK);
}
