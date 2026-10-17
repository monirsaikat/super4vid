#include <ct/media/audio_decoder.h>

#include "../common/test_media.h"

#include <ct/audio/wav.h>
#include <ct/core/fs.h>
#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ct;
using namespace ct::media;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Decodes everything after the current position (left channel and all frames).
bool DecodeAll(AudioDecoder& dec, std::vector<float>& left, TimeUs& first_pts, uint64_t& frames) {
    std::vector<float> buf(4096 * 2);
    frames = 0;
    first_pts = -1;
    for (;;) {
        uint32_t n = 0;
        TimeUs pts = 0;
        const HRESULT hr = dec.Read(buf.data(), 4096, n, pts);
        if (FAILED(hr)) return false;
        if (hr == S_FALSE) return true;
        if (first_pts < 0) first_pts = pts;
        for (uint32_t i = 0; i < n; ++i) left.push_back(buf[2 * i]);
        frames += n;
    }
}

// Frequency from positive-going zero crossings (linear interpolation) in [from, to).
double MeasureFrequency(const std::vector<float>& x, size_t from, size_t to, int rate) {
    double first = -1, last = -1;
    int crossings = 0;
    for (size_t i = from + 1; i < to && i < x.size(); ++i) {
        if (x[i - 1] < 0 && x[i] >= 0) {
            const double t = (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0) first = t;
            last = t;
            ++crossings;
        }
    }
    if (crossings < 2) return 0;
    return (crossings - 1) * rate / (last - first);
}

// Start of the first 2 ms window (96 frames) with >= 6 sign changes (2 kHz gives 8, 1 kHz 4), moved back by the
// quarter window that must already be inside the beep for the count to reach 6.
int64_t FindBeepOnset(const std::vector<float>& x) {
    for (size_t w = 0; w + 96 < x.size(); w += 4) {
        int zc = 0;
        for (size_t i = w + 1; i < w + 96; ++i)
            if ((x[i - 1] < 0) != (x[i] < 0)) ++zc;
        if (zc >= 6) return static_cast<int64_t>(w) + 48;
    }
    return -1;
}

std::string WriteToneWav(int rate, int channels, double seconds, double freq) {
    const std::string path = fs::TempPath(".wav");
    audio::WavWriter w;
    if (!w.Open(path, rate, channels, false)) return std::string();
    const size_t frames = static_cast<size_t>(rate * seconds);
    std::vector<float> buf(static_cast<size_t>(channels) * 1024);
    for (size_t f = 0; f < frames; f += 1024) {
        const size_t n = std::min<size_t>(1024, frames - f);
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c) buf[i * channels + c] = 0.5f * static_cast<float>(std::sin(2 * kPi * freq * (f + i) / rate));
        w.Write(buf.data(), n);
    }
    w.Close();
    return path;
}

} // namespace

CT_TEST_DEVICE(audio_decoder_pattern_frames_and_tone) {
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    AudioDecoder dec;
    AudioDecoder::Config cfg;
    cfg.path = path;
    CT_REQUIRE(SUCCEEDED(dec.Init(cfg)));
    CT_CHECK_EQ(dec.SourceRate(), 48000);
    std::vector<float> left;
    TimeUs first = 0;
    uint64_t frames = 0;
    CT_REQUIRE(DecodeAll(dec, left, first, frames));
    std::printf("    %llu frames, first pts %lld us, reader conversion %d\n", static_cast<unsigned long long>(frames),
                static_cast<long long>(first), dec.UsingResampler() ? 0 : 1);
    // 240000 frames of input; MF's AAC encoder pads the track to whole 1024-frame packets (240640), see DECISIONS 6.1.
    CT_CHECK(frames >= 240000 - 48 && frames <= 240640 + 48);
    CT_CHECK_EQ(first, 0);
    const double f = MeasureFrequency(left, 4800, 43200, 48000);   // 0.1 .. 0.9 s, between beeps
    std::printf("    tone %.3f Hz\n", f);
    CT_CHECK_NEAR(f, 1000.0, 1.0);
}

CT_TEST_DEVICE(audio_decoder_seek_sample_accurate) {
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    AudioDecoder dec;
    AudioDecoder::Config cfg;
    cfg.path = path;
    CT_REQUIRE(SUCCEEDED(dec.Init(cfg)));
    // The beep starts exactly at 2.000 s; seek 100 ms before it.
    CT_REQUIRE(SUCCEEDED(dec.Seek(1'900'000)));
    std::vector<float> left;
    TimeUs first = 0;
    uint64_t frames = 0;
    CT_REQUIRE(DecodeAll(dec, left, first, frames));
    CT_CHECK_NEAR(first, 1'900'000, 21);
    const int64_t onset = FindBeepOnset(left);
    std::printf("    first pts %lld, beep onset at frame %lld (expected 4800)\n", static_cast<long long>(first), static_cast<long long>(onset));
    CT_CHECK_NEAR(onset, 4800, 48);
    CT_CHECK(frames >= 240000 - 91200 - 48 && frames <= 240640 - 91200 + 48);
    // Seek again, to the end: S_FALSE.
    CT_REQUIRE(SUCCEEDED(dec.Seek(10'000'000)));
    float buf[64];
    uint32_t n = 0;
    TimeUs pts = 0;
    CT_CHECK(dec.Read(buf, 32, n, pts) == S_FALSE);
    CT_CHECK_EQ(n, 0u);
}

CT_TEST_DEVICE(audio_decoder_wav_44k1_mono_both_paths) {
    const std::string path = WriteToneWav(44100, 1, 2.0, 1000.0);
    CT_REQUIRE(!path.empty());
    for (int forced = 0; forced < 2; ++forced) {
        AudioDecoder dec;
        AudioDecoder::Config cfg;
        cfg.path = path;
        cfg.force_resampler = forced != 0;
        CT_REQUIRE(SUCCEEDED(dec.Init(cfg)));
        if (forced) CT_CHECK(dec.UsingResampler());
        std::vector<float> left;
        TimeUs first = 0;
        uint64_t frames = 0;
        CT_REQUIRE(DecodeAll(dec, left, first, frames));
        const double f = MeasureFrequency(left, 4800, 86400, 48000);
        std::printf("    %s: %llu frames, tone %.3f Hz\n", dec.UsingResampler() ? "own resampler" : "reader", static_cast<unsigned long long>(frames), f);
        CT_CHECK_NEAR(static_cast<double>(frames), 96000.0, 48.0);
        CT_CHECK_NEAR(f, 1000.0, 1.0);
        CT_CHECK_NEAR(left[24000], std::sin(2 * kPi * 1000.0 * 24000 / 48000) * 0.5, 0.01);
    }
    fs::Delete(path);
}

CT_TEST_DEVICE(audio_decoder_optional_formats) {
    // MP3/M4A/WAV from tests/data when present (docs/06 section 2); skip the missing ones.
    const char* names[] = {"tone_48k_stereo.wav", "tone_44k1_mono.wav", "speech.mp3", "music.m4a"};
    int found = 0;
    for (const char* name : names) {
        const std::string path = test::FindTestMedia(name);
        if (path.empty()) continue;
        ++found;
        AudioDecoder dec;
        AudioDecoder::Config cfg;
        cfg.path = path;
        CT_CHECK_HR(dec.Init(cfg));
        std::vector<float> left;
        TimeUs first = 0;
        uint64_t frames = 0;
        CT_CHECK(DecodeAll(dec, left, first, frames));
        CT_CHECK(frames > 0);
    }
    if (found == 0) CT_SKIP("no tone/speech/music files in tests/data");
}
