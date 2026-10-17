// ct_gen_test_media: test-only helper that writes the P06 test patterns until P04's tools/gen_test_media exists.
// It follows the P04 pattern spec (moving colour bars, the 12-digit 7-segment counter from tests/common/frame_index.h,
// a white full-frame flash on every 60th frame, 1 kHz tone at -12 dBFS with a 2 kHz 20 ms beep at every flash)
// but draws on the CPU and feeds system-memory NV12 + PCM16 to an IMFSinkWriter. Not part of the product.
//
//   ct_gen_test_media --out <dir> [--only 1080|4k] [--name file.mp4 --width W --height H --fps N --seconds S]
#include "../common/frame_index.h"

#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/win.h>

#include <initguid.h>   // CODECAPI_* GUIDs for this one translation unit
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct Yuv { uint8_t y, u, v; };

Yuv RgbToYuv709(float r, float g, float b) {
    const float y = 0.1826f * r + 0.6142f * g + 0.0620f * b + 16.0f / 255.0f;
    const float u = -0.1006f * r - 0.3386f * g + 0.4392f * b + 128.0f / 255.0f;
    const float v = 0.4392f * r - 0.3989f * g - 0.0403f * b + 128.0f / 255.0f;
    auto q = [](float x) { return static_cast<uint8_t>(std::lround(std::fmin(std::fmax(x, 0.0f), 1.0f) * 255.0f)); };
    return {q(y), q(u), q(v)};
}

#define GEN_CHECK(expr)                                                                       \
    do {                                                                                      \
        const HRESULT hr_ = (expr);                                                           \
        if (FAILED(hr_)) {                                                                    \
            std::fprintf(stderr, "%s failed: 0x%08lX\n", #expr, static_cast<unsigned long>(hr_)); \
            return hr_;                                                                       \
        }                                                                                     \
    } while (0)

HRESULT Generate(const std::string& path, int width, int height, int fps, int seconds) {
    const uint32_t bitrate = width * height >= 3840 * 2160 ? 40'000'000u : 12'000'000u;
    ComPtr<IMFAttributes> attrs;
    GEN_CHECK(MFCreateAttributes(attrs.GetAddressOf(), 2));
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSinkWriter> writer;
    DeleteFileW(ct::Utf8ToWide(path).c_str());
    GEN_CHECK(MFCreateSinkWriterFromURL(ct::Utf8ToWide(path).c_str(), nullptr, attrs.Get(), writer.GetAddressOf()));

    // Video: H.264 High, CBR-ish average bitrate, GOP 2 s, no B-frames, BT.709 limited.
    ComPtr<IMFMediaType> vout;
    GEN_CHECK(MFCreateMediaType(vout.GetAddressOf()));
    vout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vout->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    vout->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    vout->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    vout->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(vout.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(vout.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(vout.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    vout->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    vout->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    DWORD vstream = 0;
    GEN_CHECK(writer->AddStream(vout.Get(), &vstream));
    ComPtr<IMFMediaType> vin;
    GEN_CHECK(MFCreateMediaType(vin.GetAddressOf()));
    vin->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vin->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    vin->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    vin->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
    MFSetAttributeSize(vin.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(vin.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(vin.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    vin->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    vin->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    ComPtr<IMFAttributes> enc;
    GEN_CHECK(MFCreateAttributes(enc.GetAddressOf(), 2));
    enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(fps * 2));
    enc->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    // P04's recording default: quality mode 70 with a peak cap (1080p60 12 Mbps, 4K60 40 Mbps).
    enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
    enc->SetUINT32(CODECAPI_AVEncCommonQuality, 70);
    enc->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, bitrate);
    GEN_CHECK(writer->SetInputMediaType(vstream, vin.Get(), enc.Get()));

    // Audio: AAC 48 kHz stereo 192 kbps from PCM16.
    ComPtr<IMFMediaType> aout;
    GEN_CHECK(MFCreateMediaType(aout.GetAddressOf()));
    aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    DWORD astream = 0;
    GEN_CHECK(writer->AddStream(aout.Get(), &astream));
    ComPtr<IMFMediaType> ain;
    GEN_CHECK(MFCreateMediaType(ain.GetAddressOf()));
    ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
    GEN_CHECK(writer->SetInputMediaType(astream, ain.Get(), nullptr));
    GEN_CHECK(writer->BeginWriting());

    static const float kBars[8][3] = {{0.75f, 0.75f, 0.75f}, {0.75f, 0.75f, 0}, {0, 0.75f, 0.75f}, {0, 0.75f, 0},
                                      {0.75f, 0, 0.75f},     {0.75f, 0, 0},     {0, 0, 0.75f},     {0.1f, 0.1f, 0.1f}};
    Yuv bars[8];
    for (int i = 0; i < 8; ++i) bars[i] = RgbToYuv709(kBars[i][0], kBars[i][1], kBars[i][2]);

    const int64_t frames = static_cast<int64_t>(fps) * seconds;
    const size_t frame_bytes = static_cast<size_t>(width) * height * 3 / 2;
    const int64_t audio_total = 48000ll * seconds;
    int64_t audio_written = 0;
    const int bar_w = width / 8;
    for (int64_t n = 0; n < frames; ++n) {
        ComPtr<IMFMediaBuffer> buf;
        GEN_CHECK(MFCreateMemoryBuffer(static_cast<DWORD>(frame_bytes), buf.GetAddressOf()));
        BYTE* p = nullptr;
        GEN_CHECK(buf->Lock(&p, nullptr, nullptr));
        uint8_t* yp = p;
        uint8_t* uvp = p + static_cast<size_t>(width) * height;
        const bool flash = (n % 60) == 0;
        const int shift = static_cast<int>((n * 8) % width);
        if (flash) {
            std::memset(yp, 235, static_cast<size_t>(width) * height);
            std::memset(uvp, 128, static_cast<size_t>(width) * height / 2);
        } else {
            std::vector<uint8_t> row_y(width), row_uv(width);
            for (int x = 0; x < width; ++x) row_y[x] = bars[(((x + shift) % width) / bar_w) & 7].y;
            for (int yy = 0; yy < height; ++yy) std::memcpy(yp + static_cast<size_t>(yy) * width, row_y.data(), width);
            // Chroma pairs must use the bar of the even pixel so U/V belong together.
            for (int x = 0; x < width; x += 2) {
                const int b = (((x + shift) % width) / bar_w) & 7;
                row_uv[x] = bars[b].u;
                row_uv[x + 1] = bars[b].v;
            }
            for (int yy = 0; yy < height / 2; ++yy) std::memcpy(uvp + static_cast<size_t>(yy) * width, row_uv.data(), width);
        }
        ct::test::DrawFrameIndexY(yp, width, n);
        for (int yy = ct::test::FrameCounterLayout::kBoxY / 2; yy < (ct::test::FrameCounterLayout::kBoxY + ct::test::FrameCounterLayout::kBoxH) / 2; ++yy)
            std::memset(uvp + static_cast<size_t>(yy) * width + ct::test::FrameCounterLayout::kBoxX, 128, ct::test::FrameCounterLayout::kBoxW);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(frame_bytes));
        ComPtr<IMFSample> sample;
        GEN_CHECK(MFCreateSample(sample.GetAddressOf()));
        sample->AddBuffer(buf.Get());
        const int64_t t0 = ct::FrameTimeUs(n, fps, 1), t1 = ct::FrameTimeUs(n + 1, fps, 1);
        sample->SetSampleTime(t0 * 10);
        sample->SetSampleDuration((t1 - t0) * 10);
        GEN_CHECK(writer->WriteSample(vstream, sample.Get()));

        // Audio up to the end of this frame, in 10 ms packets.
        const int64_t audio_target = std::min<int64_t>(audio_total, (t1 * 48000) / 1'000'000);
        while (audio_written + 480 <= audio_target || (n == frames - 1 && audio_written < audio_total)) {
            const int64_t count = std::min<int64_t>(480, audio_total - audio_written);
            ComPtr<IMFMediaBuffer> abuf;
            GEN_CHECK(MFCreateMemoryBuffer(static_cast<DWORD>(count * 4), abuf.GetAddressOf()));
            BYTE* ap = nullptr;
            GEN_CHECK(abuf->Lock(&ap, nullptr, nullptr));
            int16_t* s = reinterpret_cast<int16_t*>(ap);
            for (int64_t i = 0; i < count; ++i) {
                const int64_t f = audio_written + i;
                const int64_t in_second = f % 48000;
                const bool beep = in_second < 960;   // 20 ms at every flash (every second)
                const double freq = beep ? 2000.0 : 1000.0;
                const double v = 0.25118864 * std::sin(2.0 * 3.14159265358979323846 * freq * static_cast<double>(beep ? in_second : f) / 48000.0);
                const int16_t q = static_cast<int16_t>(std::lround(v * 32767.0));
                s[2 * i] = q;
                s[2 * i + 1] = q;
            }
            abuf->Unlock();
            abuf->SetCurrentLength(static_cast<DWORD>(count * 4));
            ComPtr<IMFSample> asample;
            GEN_CHECK(MFCreateSample(asample.GetAddressOf()));
            asample->AddBuffer(abuf.Get());
            asample->SetSampleTime(audio_written * 10'000'000 / 48000);
            asample->SetSampleDuration(count * 10'000'000 / 48000);
            GEN_CHECK(writer->WriteSample(astream, asample.Get()));
            audio_written += count;
        }
    }
    GEN_CHECK(writer->Finalize());
    std::printf("wrote %s: %dx%d@%d, %lld frames, %lld audio frames\n", path.c_str(), width, height, fps,
                static_cast<long long>(frames), static_cast<long long>(audio_written));
    return S_OK;
}

} // namespace

int main(int argc, char** argv) {
    std::string out_dir = ".", only, name;
    int width = 0, height = 0, fps = 60, seconds = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--out") out_dir = next();
        else if (a == "--only") only = next();
        else if (a == "--name") name = next();
        else if (a == "--width") width = std::atoi(next().c_str());
        else if (a == "--height") height = std::atoi(next().c_str());
        else if (a == "--fps") fps = std::atoi(next().c_str());
        else if (a == "--seconds") seconds = std::atoi(next().c_str());
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return 3;
    CreateDirectoryW(ct::Utf8ToWide(out_dir).c_str(), nullptr);
    HRESULT hr = S_OK;
    if (!name.empty()) {
        hr = Generate(ct::PathJoin(out_dir, name), width, height, fps, seconds);
    } else {
        if (only.empty() || only == "1080") hr = Generate(ct::PathJoin(out_dir, "pattern_1080p60_5s.mp4"), 1920, 1080, 60, 5);
        if (SUCCEEDED(hr) && (only.empty() || only == "4k")) hr = Generate(ct::PathJoin(out_dir, "pattern_4k60_3s.mp4"), 3840, 2160, 60, 3);
    }
    MFShutdown();
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}
