#include <ct/audio/wav.h>

#include <ct/core/fs.h>
#include <ct/test/ct_test.h>

#include <cmath>
#include <vector>

using namespace ct::audio;

CT_TEST(wav_round_trip_float_and_pcm16) {
    const int frames = 12345;
    std::vector<float> data(frames * 2);
    for (int i = 0; i < frames; ++i) {
        data[2 * i] = 0.8f * static_cast<float>(std::sin(i * 0.01));
        data[2 * i + 1] = -0.3f * static_cast<float>(std::cos(i * 0.02));
    }
    for (bool is_float : {true, false}) {
        const std::string path = ct::fs::TempPath(".wav");
        WavWriter w;
        CT_REQUIRE(w.Open(path, 48000, 2, is_float));
        // Written in uneven pieces, larger than the internal buffer in total.
        int at = 0;
        for (int n : {1, 999, 5000, 6345}) {
            CT_CHECK(w.Write(data.data() + 2 * at, n));
            at += n;
        }
        CT_CHECK_EQ(w.FramesWritten(), uint64_t(frames));
        CT_CHECK(w.Close());
        CT_CHECK_EQ(ct::fs::FileSize(path), uint64_t(46 + frames * 2 * (is_float ? 4 : 2)));

        WavReader r;
        CT_REQUIRE(r.Open(path));
        CT_CHECK_EQ(r.Rate(), 48000);
        CT_CHECK_EQ(r.Channels(), 2);
        CT_CHECK_EQ(r.Frames(), size_t(frames));
        std::vector<float> back(frames * 2);
        size_t got = 0;
        while (size_t n = r.Read(back.data() + 2 * got, 1000)) got += n;
        CT_CHECK_EQ(got, size_t(frames));
        double max_err = 0;
        for (size_t i = 0; i < back.size(); ++i) max_err = std::fmax(max_err, std::fabs(back[i] - data[i]));
        CT_CHECK(max_err <= (is_float ? 0.0 : 1.0 / 32768.0));
        r.Seek(frames - 10);
        CT_CHECK_EQ(r.Read(back.data(), 100), size_t(10));
        CT_CHECK(ct::fs::Delete(path));
    }
}

CT_TEST(wav_reader_rejects_garbage) {
    const std::string path = ct::fs::TempPath(".wav");
    CT_REQUIRE(ct::fs::WriteAtomic(path, "RIFF\x04\x00\x00\x00WAVEjunk"));
    WavReader r;
    CT_CHECK(!r.Open(path));
    CT_CHECK(!r.Open(path + ".missing"));
    CT_CHECK(ct::fs::Delete(path));
}
