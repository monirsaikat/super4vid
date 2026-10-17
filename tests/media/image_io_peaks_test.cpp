#include <ct/media/image_io.h>
#include <ct/media/peaks.h>

#include "../common/test_device.h"
#include "../common/test_media.h"

#include <ct/core/fs.h>
#include <ct/test/ct_test.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace ct;
using namespace ct::media;

CT_TEST_DEVICE(image_io_png_round_trip_exact) {
    const int w = 37, h = 23;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &bgra[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x * 7);
            p[1] = static_cast<uint8_t>(y * 11);
            p[2] = static_cast<uint8_t>((x * y) & 0xFF);
            p[3] = 255;
        }
    const std::string path = fs::TempPath(".png");
    CT_REQUIRE(SUCCEEDED(ImageIo::SavePng(path, bgra.data(), w, h, w * 4)));
    std::vector<uint8_t> back;
    SizeI size;
    CT_REQUIRE(SUCCEEDED(ImageIo::LoadRgba(path, back, size)));
    CT_CHECK_EQ(size.w, w);
    CT_CHECK_EQ(size.h, h);
    CT_CHECK(back == bgra);
    // Premultiplied output for translucent pixels.
    bgra[0] = 200;
    bgra[1] = 100;
    bgra[2] = 50;
    bgra[3] = 128;
    CT_REQUIRE(SUCCEEDED(ImageIo::SavePng(path, bgra.data(), w, h, w * 4)));
    CT_REQUIRE(SUCCEEDED(ImageIo::LoadRgba(path, back, size)));
    CT_CHECK_NEAR(back[0], 200 * 128 / 255.0, 1.0);
    CT_CHECK_EQ(static_cast<int>(back[3]), 128);
    fs::Delete(path);
}

CT_TEST_DEVICE(image_io_jpeg_and_texture) {
    CT_REQUIRE_DEVICE(dev);
    const int w = 64, h = 48;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i] = 40;
        bgra[i + 1] = 120;
        bgra[i + 2] = 220;
        bgra[i + 3] = 255;
    }
    const std::string path = fs::TempPath(".jpg");
    CT_REQUIRE(SUCCEEDED(ImageIo::SaveJpeg(path, bgra.data(), w, h, w * 4, 0.95f)));
    std::vector<uint8_t> back;
    SizeI size;
    CT_REQUIRE(SUCCEEDED(ImageIo::LoadRgba(path, back, size)));
    CT_CHECK_EQ(size.w, w);
    CT_CHECK_NEAR(back[(10 * w + 10) * 4 + 2], 220, 4);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    CT_REQUIRE(SUCCEEDED(ImageIo::LoadToTexture(*dev, path, tex, size)));
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    CT_CHECK_EQ(d.Width, static_cast<UINT>(w));
    CT_CHECK(d.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    fs::Delete(path);
    CT_CHECK(FAILED(ImageIo::LoadRgba("C:\\does\\not\\exist.png", back, size)));
}

CT_TEST_DEVICE(peaks_build_and_cache) {
    CT_REQUIRE_MEDIA(path, "pattern_1080p60_5s.mp4");
    Peaks p;
    CT_REQUIRE(SUCCEEDED(Peaks::Build(path, -1, p)));
    std::printf("    %zu windows, duration %lld us\n", p.min.size(), static_cast<long long>(p.duration_us));
    // 5 s at 100 windows/s; MF's AAC padding adds up to 1024 frames (2 windows), see DECISIONS 6.1.
    CT_CHECK(p.min.size() >= 500 && p.min.size() <= 502);
    CT_CHECK_EQ(p.min.size(), p.max.size());
    CT_CHECK_EQ(p.per_second, 100);
    // -12 dBFS tone: peaks ~ +-0.251.
    CT_CHECK_NEAR(p.max[250], 0.251, 0.01);
    CT_CHECK_NEAR(p.min[250], -0.251, 0.01);
    Peaks::SaveCache(path, -1, p);
    Peaks q;
    CT_REQUIRE(Peaks::LoadCache(path, -1, q));
    CT_CHECK(q.min == p.min && q.max == p.max && q.duration_us == p.duration_us);
    CT_CHECK(!Peaks::LoadCache(path, 7, q));   // another stream has no cache file
    fs::Delete(Peaks::CachePath(path, -1));
}
