// ct/media/encode_config.h: bitrate table, AAC byte-rate rounding, interleaving rule.
#include <ct/media/encode_config.h>
#include <ct/test/ct_test.h>

using namespace ct;
using namespace ct::media;

namespace {
VideoEncodeConfig Cfg(int w, int h, int fps, VideoCodec c = VideoCodec::H264) {
    VideoEncodeConfig v;
    v.size = SizeI{w, h};
    v.fps_num = fps;
    v.codec = c;
    return v;
}
} // namespace

CT_TEST(default_bitrate_table) {
    CT_CHECK_EQ(DefaultBitrate(Cfg(1920, 1080, 60)), 11'197'440u);
    CT_CHECK_EQ(DefaultBitrate(Cfg(3840, 2160, 60)), 44'789'760u);
    CT_CHECK_EQ(DefaultBitrate(Cfg(1280, 720, 30)), 2'488'320u);
    CT_CHECK_EQ(DefaultBitrate(Cfg(640, 360, 30)), 2'000'000u);       // clamped low
    CT_CHECK_EQ(DefaultBitrate(Cfg(7680, 4320, 60)), 60'000'000u);    // clamped high
    CT_CHECK_EQ(DefaultBitrate(Cfg(1920, 1080, 60, VideoCodec::Hevc)), 7'278'336u);
    CT_CHECK_EQ(DefaultBitrate(Cfg(7680, 4320, 60, VideoCodec::Hevc)), 39'000'000u);
    VideoEncodeConfig ntsc = Cfg(1920, 1080, 30000);
    ntsc.fps_den = 1001;
    CT_CHECK_EQ(DefaultBitrate(ntsc), 5'593'127u);   // 1920*1080*29.97*0.09
}

CT_TEST(effective_bitrates) {
    VideoEncodeConfig c = Cfg(1920, 1080, 60);
    CT_CHECK_EQ(EffectiveBitrate(c), 11'197'440u);
    CT_CHECK_EQ(EffectiveMaxBitrate(c), 16'796'160u);
    c.bitrate_bps = 8'000'000;
    CT_CHECK_EQ(EffectiveBitrate(c), 8'000'000u);
    CT_CHECK_EQ(EffectiveMaxBitrate(c), 12'000'000u);
    c.max_bitrate_bps = 4'000'000;   // below the mean: raised to the mean
    CT_CHECK_EQ(EffectiveMaxBitrate(c), 8'000'000u);
    c.max_bitrate_bps = 20'000'000;
    CT_CHECK_EQ(EffectiveMaxBitrate(c), 20'000'000u);
}

CT_TEST(aac_byte_rate_rounding) {
    CT_CHECK_EQ(AacBytesPerSecond(192000), 24000u);
    CT_CHECK_EQ(AacBytesPerSecond(160000), 20000u);
    CT_CHECK_EQ(AacBytesPerSecond(128000), 16000u);
    CT_CHECK_EQ(AacBytesPerSecond(96000), 12000u);
    CT_CHECK_EQ(AacBytesPerSecond(320000), 24000u);   // above the encoder's maximum
    CT_CHECK_EQ(AacBytesPerSecond(256000), 24000u);
    CT_CHECK_EQ(AacBytesPerSecond(64000), 12000u);    // below the minimum
    CT_CHECK_EQ(AacBytesPerSecond(0), 12000u);
    CT_CHECK_EQ(AacBytesPerSecond(112000), 16000u);   // 14000 B/s: tie between 12000 and 16000 rounds up
    CT_CHECK_EQ(AacBytesPerSecond(111992), 12000u);   // 13999 B/s
    CT_CHECK_EQ(AacBytesPerSecond(176000), 24000u);   // 22000 B/s: tie rounds up
    CT_CHECK_EQ(AacBytesPerSecond(144000), 20000u);   // 18000 B/s: tie rounds up
}

CT_TEST(interleave_smallest_head_first) {
    MuxStreamState s[3];
    for (auto& x : s) x.active = true;
    CT_CHECK_EQ(ChooseNextStream(s, 3), -1);   // nothing queued
    s[0] = {true, 16666, 33333, true, false};
    s[1] = {true, 0, 100000, true, false};
    s[2] = {true, 10000, 110000, true, false};
    CT_CHECK_EQ(ChooseNextStream(s, 3), 1);
    s[1].head_us = 20000;
    CT_CHECK_EQ(ChooseNextStream(s, 3), 2);
    s[2].head_us = 16666;   // tie with video: the lower index (video) wins
    CT_CHECK_EQ(ChooseNextStream(s, 3), 0);
}

CT_TEST(interleave_one_queue_empty) {
    // Video queued, audio queue empty but its producer is behind by < 500 ms: write the video.
    MuxStreamState s[2];
    s[0] = {true, 400000, 416666, true, false};
    s[1] = {false, 0, 100000, true, false};
    CT_CHECK_EQ(ChooseNextStream(s, 2), 0);
    // 500 ms ahead exactly is still allowed, one microsecond more is not.
    s[0].head_us = 600000;
    CT_CHECK_EQ(ChooseNextStream(s, 2), 0);
    s[0].head_us = 600001;
    CT_CHECK_EQ(ChooseNextStream(s, 2), -1);
    // ... unless that producer stopped (or stalled).
    s[1].active = false;
    CT_CHECK_EQ(ChooseNextStream(s, 2), 0);
    // A custom lead.
    s[1].active = true;
    CT_CHECK_EQ(ChooseNextStream(s, 2, 1'000'000), 0);
}

CT_TEST(interleave_audio_ahead_of_video) {
    // Audio packets arrive well ahead of the first video frame: audio may run at most 500 ms ahead.
    MuxStreamState s[3];
    s[0] = {false, 0, 0, true, false};          // video producer active, nothing yet
    s[1] = {true, 0, 100000, true, false};      // mic
    s[2] = {true, 0, 100000, true, false};      // system
    CT_CHECK_EQ(ChooseNextStream(s, 3), 1);
    s[1] = {true, 500000, 600000, true, false};
    s[2] = {true, 400000, 600000, true, false};
    CT_CHECK_EQ(ChooseNextStream(s, 3), 2);
    s[2].head_us = 500001;
    s[1].head_us = 500001;
    CT_CHECK_EQ(ChooseNextStream(s, 3), -1);    // both 500 ms ahead of video's frontier 0
    s[0].frontier_us = 16667;
    CT_CHECK_EQ(ChooseNextStream(s, 3), 1);
}

CT_TEST(interleave_pressure_and_drain) {
    MuxStreamState s[2];
    s[0] = {true, 900000, 1000000, true, true};   // video queue nearly full, far ahead of audio
    s[1] = {false, 0, 100000, true, false};
    CT_CHECK_EQ(ChooseNextStream(s, 2), 0);       // written anyway rather than dropped
    s[0].pressure = false;
    CT_CHECK_EQ(ChooseNextStream(s, 2), -1);
    // Pressure never overtakes a writable smaller head.
    s[1] = {true, 50000, 150000, true, false};
    s[0].pressure = true;
    CT_CHECK_EQ(ChooseNextStream(s, 2), 1);
    // Finalize: no producer active, everything drains in timestamp order.
    s[0] = {true, 5'000'000, 5'016'666, false, false};
    s[1] = {true, 1'000'000, 1'100'000, false, false};
    CT_CHECK_EQ(ChooseNextStream(s, 2), 1);
    s[1].has_head = false;
    CT_CHECK_EQ(ChooseNextStream(s, 2), 0);
}
