#include <ct/audio/meter.h>

#include <ct/test/ct_test.h>

#include <cmath>
#include <vector>

using ct::audio::Meter;

CT_TEST(meter_db_values) {
    Meter m;
    CT_CHECK_NEAR(m.PeakDb(0), -120.0, 1e-4);
    CT_CHECK_NEAR(m.RmsDb(1), -120.0, 1e-4);
    // Left: 1 kHz sine at 0.5 (-6.02 dBFS peak, -9.03 dBFS RMS). Right: constant 0.1 (-20 dB peak and RMS).
    const int frames = 4800;
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; ++i) {
        buf[2 * i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 * i / 48000.0 + 0.3));
        buf[2 * i + 1] = 0.1f;
    }
    m.Push(buf.data(), frames, 2);
    CT_CHECK_EQ(m.Channels(), 2);
    CT_CHECK_NEAR(m.PeakDb(0), -6.0206, 0.01);
    CT_CHECK_NEAR(m.RmsDb(0), -9.0309, 0.01);
    CT_CHECK_NEAR(m.PeakDb(1), -20.0, 1e-3);
    CT_CHECK_NEAR(m.RmsDb(1), -20.0, 1e-3);
    // Silence goes to the floor, never -inf; out-of-range channels report the floor.
    std::vector<float> zeros(frames * 2, 0.0f);
    m.Push(zeros.data(), frames, 2);
    CT_CHECK_NEAR(m.PeakDb(0), -120.0, 1e-4);
    CT_CHECK_NEAR(m.RmsDb(1), -120.0, 1e-4);
    CT_CHECK_NEAR(m.PeakDb(9), -120.0, 1e-4);
    CT_CHECK_NEAR(Meter::ToDb(1.0f), 0.0, 1e-6);
    CT_CHECK_NEAR(Meter::ToDb(0.0f), -120.0, 1e-4);
}
