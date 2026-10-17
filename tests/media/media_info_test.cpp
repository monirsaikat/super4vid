#include <ct/media/media_info.h>

#include "../common/test_media.h"

#include <ct/core/fs.h>
#include <ct/platform/image_io.h>
#include <ct/test/ct_test.h>

#include <vector>

using namespace ct;
using namespace ct::media;

CT_TEST(media_info_image_extension) {
    CT_CHECK(MediaInfo::IsImagePath("C:\\a\\b.PNG"));
    CT_CHECK(MediaInfo::IsImagePath("x.jpeg"));
    CT_CHECK(MediaInfo::IsImagePath("x.webp"));
    CT_CHECK(!MediaInfo::IsImagePath("x.mp4"));
    CT_CHECK(!MediaInfo::IsImagePath("png"));
}

CT_TEST(media_info_missing_file_fails) {
    MediaInfo info;
    CT_CHECK(FAILED(MediaInfo::Load("C:\\does\\not\\exist.mp4", info)));
    CT_CHECK(FAILED(MediaInfo::Load("C:\\does\\not\\exist.png", info)));
}

CT_TEST_DEVICE(media_info_png) {
    const std::string path = fs::TempPath(".png");
    std::vector<uint8_t> rgba(64 * 32 * 4, 200);
    CT_REQUIRE(platform::ImageIo::WritePngRgba(path.c_str(), rgba.data(), 64, 32, 64 * 4));
    MediaInfo info;
    CT_CHECK_HR(MediaInfo::Load(path, info));
    CT_CHECK(info.is_image);
    CT_CHECK_EQ(info.duration_us, 0);
    CT_REQUIRE(info.streams.size() == 1u);
    CT_CHECK_EQ(info.streams[0].width, 64);
    CT_CHECK_EQ(info.streams[0].height, 32);
    CT_CHECK_EQ(info.streams[0].codec, std::string("PNG"));
    CT_CHECK_EQ(info.FirstVideo(), 0);
    fs::Delete(path);
}

CT_TEST_DEVICE(media_info_windows_wav) {
    const char* path = "C:\\Windows\\Media\\Windows Notify System Generic.wav";
    if (!test::FileExistsUtf8(path)) CT_SKIP("system sound missing");
    MediaInfo info;
    CT_CHECK_HR(MediaInfo::Load(path, info));
    CT_CHECK(!info.is_image);
    CT_CHECK_EQ(info.FirstVideo(), -1);
    CT_REQUIRE(info.AudioStreams().size() == 1u);
    const StreamInfo* s = info.Find(info.AudioStreams()[0]);
    CT_REQUIRE(s != nullptr);
    CT_CHECK(s->sample_rate > 0);
    CT_CHECK(s->channels > 0);
    CT_CHECK(info.duration_us > 0);
    CT_CHECK(info.seekable);
}

CT_TEST_DEVICE(media_info_pattern_1080p) {
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    MediaInfo info;
    CT_CHECK_HR(MediaInfo::Load(path, info));
    // MF_PD_DURATION is the longest track. The video track is 5.000 s; MF's AAC encoder pads the audio track up to a
    // whole 1024-frame packet (240000 -> 240640 frames = 5.0133 s), so allow +1 AAC packet (DECISIONS.md P06 6.1).
    CT_CHECK(info.duration_us >= 4'999'000 && info.duration_us <= 5'000'000 + 21'334 + 1'000);
    CT_CHECK(info.seekable);
    const int v = info.FirstVideo();
    CT_REQUIRE(v >= 0);
    const StreamInfo* vs = info.Find(v);
    CT_REQUIRE(vs != nullptr);
    CT_CHECK_EQ(vs->codec, std::string("H264"));
    CT_CHECK_EQ(vs->width, 1920);
    CT_CHECK_EQ(vs->height, 1080);
    CT_CHECK_EQ(vs->fps_num / vs->fps_den, 60);
    CT_CHECK_EQ(vs->rotation_deg, 0);
    const std::vector<int> audio = info.AudioStreams();
    CT_REQUIRE(audio.size() == 1u);
    CT_CHECK_EQ(info.Find(audio[0])->sample_rate, 48000);
    CT_CHECK_EQ(info.Find(audio[0])->codec, std::string("AAC"));
}
