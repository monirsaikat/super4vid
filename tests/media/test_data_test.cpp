// tests/data/pattern_*.mp4 (written by tools/gen_test_media): stream layout, frame counter, flashes and beeps.
// The counter is read with tests/common/frame_index.h, the one reader for the layout in tests/data/README.md.
#include "media_test_util.h"

#include "../common/frame_index.h"

#include <ct/media/mf_runtime.h>
#include <ct/media/probe.h>
#include <ct/test/ct_test.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace ct;
using namespace ct::media;
using namespace ct::media_test;

namespace {

struct Check {
    int frames = 0, counter_errors = 0, flash_errors = 0;
};

void CheckFile(const char* path, int w, int h, int frames, int64_t duration_us) {
    MfScope mf;
    ProbeResult p;
    if (FAILED(Probe(path, p))) {
        CT_FAIL(std::string("cannot open ") + path + " (run gen_test_media --all tests/data)");
        return;
    }
    CT_CHECK_EQ(p.video_width, w);
    CT_CHECK_EQ(p.video_height, h);
    CT_CHECK_EQ(p.fps_num, 60);
    CT_CHECK_EQ(p.video_streams, 1);
    CT_CHECK_EQ(p.audio_streams, 1);
    CT_CHECK_NEAR(p.duration_us, duration_us, 25'000);
    StreamScan v;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_CHECK_EQ(v.samples, static_cast<uint64_t>(frames));
    Check c;
    CT_REQUIRE(DecodeLuma(path, 125, [](void* u, int frame, const uint8_t* y, int pitch, int w, int h) {
        auto& k = *static_cast<Check*>(u);
        ++k.frames;
        if (test::ReadFrameIndexY(y, w, h, pitch) != frame) ++k.counter_errors;
        const bool flash = frame % 60 == 0;
        const uint8_t probe = y[static_cast<size_t>(500) * pitch + 700];   // outside the counter box
        if (flash != (probe > 230)) ++k.flash_errors;
        return true;
    }, &c));
    std::printf("  %s: %d frames checked, counter errors %d, flash errors %d\n", path, c.frames, c.counter_errors, c.flash_errors);
    CT_CHECK_EQ(c.frames, 125);
    CT_CHECK_EQ(c.counter_errors, 0);
    CT_CHECK_EQ(c.flash_errors, 0);
    // Audio: 2 kHz for 20 ms at each flash, 1 kHz otherwise (count rising zero crossings over 20 ms windows).
    std::vector<float> pcm;
    int ch = 0;
    int64_t first = 0;
    CT_REQUIRE(DecodeAudio(path, 0, pcm, ch, first));
    auto crossings = [&](int64_t from) {
        int n = 0;
        for (int64_t f = from + 1; f < from + 960; ++f)
            if (pcm[static_cast<size_t>((f - 1) * ch)] < 0 && pcm[static_cast<size_t>(f * ch)] >= 0) ++n;
        return n;
    };
    const int beep = crossings(48'000 + 24), tone = crossings(48'000 + 2'400);
    std::printf("  audio: first %lld us; crossings in 20 ms: beep %d (2 kHz -> 40), tone %d (1 kHz -> 20)\n",
                static_cast<long long>(first), beep, tone);
    CT_CHECK(beep >= 37 && beep <= 40);
    CT_CHECK(tone >= 19 && tone <= 20);
}

} // namespace

CT_TEST_DEVICE(test_data_pattern_1080p60_5s) { CheckFile("data/pattern_1080p60_5s.mp4", 1920, 1080, 300, 5'000'000); }
CT_TEST_DEVICE(test_data_pattern_4k60_3s) { CheckFile("data/pattern_4k60_3s.mp4", 3840, 2160, 180, 3'000'000); }
