// ct/media/mp4_writer.h: HEVC, missing folder, write failure mid-file (disk full), finalize/abort behaviour.
#include "media_test_util.h"

#include <ct/core/error.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/media/encoder_caps.h>
#include <ct/media/mf_runtime.h>
#include <ct/media/mp4_writer.h>
#include <ct/media/probe.h>
#include <ct/platform/win.h>
#include <ct/test/ct_test.h>

#include <mfapi.h>
#include <mftransform.h>

#include <crtdbg.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <vector>

using namespace ct;
using namespace ct::media;
using namespace ct::media_test;
using ct::platform::D3DDevice;
using ct::platform::TexturePool;

namespace {

struct Listener final : Mp4Writer::Listener {
    std::atomic<int> calls{0};
    std::atomic<HRESULT> hr{S_OK};
    void OnWriterError(HRESULT e) override {
        hr.store(e);
        calls.fetch_add(1);
    }
};

// Sends `frames` new frames at 60 fps as fast as the queue allows (retrying a full queue); returns frames accepted.
int SendFrames(D3DDevice& dev, Mp4Writer& w, TexturePool& pool, int frames, int* rejected_after_error = nullptr) {
    int accepted = 0;
    for (int n = 0; n < frames; ++n) {
        uint32_t idx = 0;
        ID3D11Texture2D* tex = nullptr;
        for (int tries = 0; !(tex = pool.Acquire(idx)) && tries < 2000; ++tries) Sleep(1);
        if (!tex) break;
        FillNv12(dev, tex, static_cast<uint8_t>(16 + n % 200));
        bool ok = false;
        for (int tries = 0; tries < 2000; ++tries) {
            ok = w.OnFrame(tex, idx, FrameTimeUs(n, 60, 1));
            if (ok || !w.Accepting()) break;
            Sleep(1);
        }
        if (ok) {
            ++accepted;
        } else {
            pool.Release(idx);
            if (rejected_after_error && !w.Accepting()) ++*rejected_after_error;
        }
    }
    return accepted;
}

} // namespace

CT_TEST_DEVICE(mp4_writer_hevc_round_trip) {
    if (!EncoderCaps::HasHardware(VideoCodec::Hevc)) CT_SKIP("no hardware HEVC encoder");
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    TexturePool pool;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 1920, 1080, 12, true, nullptr));
    const std::string path = TempPath("hevc.mp4");
    Mp4Writer::Config c;
    c.path = path;
    c.video.codec = VideoCodec::Hevc;
    c.video.size = SizeI{1920, 1080};
    c.video.fps_num = 60;
    c.audio.push_back(AudioEncodeConfig{});
    Mp4Writer w;
    CT_REQUIRE(SUCCEEDED(w.Init(dev, c, &pool)));
    std::printf("  encoder %s (%s)\n", w.EncoderName().c_str(), w.IsHardware() ? "hardware" : "software");
    CT_CHECK(w.IsHardware());
    CT_REQUIRE(w.Start());
    CT_CHECK_EQ(SendFrames(dev, w, pool, 60), 60);
    CT_CHECK_HR(w.Finalize());   // the audio stream got no packets: the writer pads it with silence
    ProbeResult p;
    CT_REQUIRE(SUCCEEDED(Probe(path, p)));
    CT_CHECK_EQ(p.video_codec, std::string("hevc"));
    CT_CHECK_EQ(p.audio_streams, 1);
    StreamScan v;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    CT_CHECK_EQ(v.samples, 60ull);
    // Decoding HEVC needs the "HEVC Video Extensions" decoder, which is not part of Windows.
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_HEVC};
    IMFActivate** acts = nullptr;
    UINT32 decoders = 0;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_ALL, &in, nullptr, &acts, &decoders))) {
        for (UINT32 i = 0; i < decoders; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    }
    if (decoders == 0) {
        std::printf("  no HEVC decoder installed: decode check skipped (encode, frame count and codec verified)\n");
        return;
    }
    std::vector<double> means;
    CT_REQUIRE(DecodeLumaMeans(path, 4, means, dev.MfManager()));
    CT_REQUIRE(means.size() == 4);
    for (size_t i = 0; i < 4; ++i) CT_CHECK_NEAR(means[i], 16 + static_cast<double>(i), 3.0);
}

CT_TEST_DEVICE(mp4_writer_missing_folder_fails_clearly) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    TexturePool pool;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 640, 360, 4, false, nullptr));
    Mp4Writer::Config c;
    c.path = TempPath("no_such_folder\\deeper\\out.mp4");
    c.video.size = SizeI{640, 360};
    Mp4Writer w;
    const HRESULT hr = w.Init(dev, c, &pool);
    std::printf("  Init: %s; message: %s\n", HrToString(hr).c_str(), w.ErrorMessage().c_str());
    CT_CHECK(FAILED(hr));
    CT_CHECK_EQ(w.LastError(), hr);
    CT_CHECK(hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
    CT_CHECK(w.ErrorMessage().find("folder does not exist") != std::string::npos);
    CT_CHECK(!w.Start());
    CT_CHECK_EQ(w.Finalize(), hr);   // Finalize reports the same error
    CT_CHECK(!w.OnFrame(pool.At(0), 0, 0));
    // Odd sizes are refused before any file is created.
    Mp4Writer odd;
    c.path = TempPath("odd.mp4");
    c.video.size = SizeI{641, 360};
    CT_CHECK_EQ(odd.Init(dev, c, &pool), E_INVALIDARG);
}

// The sink fails mid-recording (simulated ERROR_DISK_FULL on the 40th WriteSample): input is refused from then on, the
// owner is told once, and the file up to the failure is finalized and playable.
CT_TEST_DEVICE(mp4_writer_write_failure_disk_full) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    TexturePool pool;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 1280, 720, 8, false, nullptr));
    const std::string path = TempPath("disk_full.mp4");
    Listener listener;
    Mp4Writer::Config c;
    c.path = path;
    c.video.size = SizeI{1280, 720};
    c.listener = &listener;
    Mp4Writer w;
    CT_REQUIRE(SUCCEEDED(w.Init(dev, c, &pool)));
    w.InjectWriteErrorForTest(40, HRESULT_FROM_WIN32(ERROR_DISK_FULL));
    CT_REQUIRE(w.Start());
    int rejected = 0;
    const int accepted = SendFrames(dev, w, pool, 120, &rejected);
    const HRESULT hr = w.Finalize();
    std::printf("  accepted %d, rejected after the error %d, written %llu; Finalize %s; listener %d call(s); \"%s\"\n", accepted, rejected,
                static_cast<unsigned long long>(w.GetStats().video_written.load()), HrToString(hr).c_str(), listener.calls.load(),
                w.ErrorMessage().c_str());
    CT_CHECK_EQ(hr, HRESULT_FROM_WIN32(ERROR_DISK_FULL));
    CT_CHECK_EQ(listener.calls.load(), 1);
    CT_CHECK_EQ(listener.hr.load(), HRESULT_FROM_WIN32(ERROR_DISK_FULL));
    CT_CHECK(!w.Accepting());
    CT_CHECK(rejected > 0);
    CT_CHECK_EQ(w.GetStats().video_written.load(), 40ull);
    CT_CHECK_EQ(pool.InUse(), 0u);   // queued frames were released, not leaked
    CT_CHECK(w.ErrorMessage().find("disk is full") != std::string::npos);
    StreamScan v;
    CT_REQUIRE(SUCCEEDED(ScanStream(path, 0, v)));
    std::printf("  file holds %llu frames\n", static_cast<unsigned long long>(v.samples));
    CT_CHECK(v.samples >= 30 && v.samples <= 40);
}

CT_TEST_DEVICE(mp4_writer_abort_returns_quickly) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
    TexturePool pool;
    CT_REQUIRE(MakeNv12Pool(dev, pool, 1280, 720, 8, false, nullptr));
    Mp4Writer::Config c;
    c.path = TempPath("abort.mp4");
    c.video.size = SizeI{1280, 720};
    c.audio.push_back(AudioEncodeConfig{});
    Mp4Writer w;
    CT_REQUIRE(SUCCEEDED(w.Init(dev, c, &pool)));
    CT_REQUIRE(w.Start());
    SendFrames(dev, w, pool, 30);
    Stopwatch sw;
    sw.Start();
    w.Abort();
    std::printf("  Abort returned after %.1f ms\n", sw.ElapsedMs());
    CT_CHECK(sw.ElapsedMs() < 3100);
    CT_CHECK(!w.Accepting());
}

// Review fix: Abort while ct-encode is inside a slow IMFSinkWriter::Finalize (simulated), then destroy the writer.
// The destructor must neither free the sink writer/events under the live thread nor block; the thread then finishes
// in its own (deliberately leaked) state and never writes into the destroyed Mp4Writer. Debug: freed blocks are kept
// filled (_CRTDBG_DELAY_FREE_MEM_DF), so a late write into freed memory fails _CrtCheckMemory.
CT_TEST_DEVICE(mp4_writer_abort_during_slow_finalize_then_destroy) {
    MfScope mf;
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(false)));
#ifdef _DEBUG
    const int old_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    _CrtSetDbgFlag(old_flags | _CRTDBG_DELAY_FREE_MEM_DF);
#endif
    const std::string path = TempPath("abort_slow_finalize.m4a");
    Mp4Writer::Config c;
    c.path = path;
    c.has_video = false;
    c.audio.push_back(AudioEncodeConfig{});
    auto w = std::make_unique<Mp4Writer>();
    CT_REQUIRE(SUCCEEDED(w->Init(dev, c, nullptr)));
    w->SetFinalizeDelayForTest(4000);
    CT_REQUIRE(w->Start());
    std::vector<float> pcm(480 * 2, 0.1f);
    for (int k = 0; k < 20; ++k) {
        audio::AudioPacket p;
        p.t_us = k * 10'000;
        p.frames = 480;
        p.data = pcm.data();
        w->OnAudio(0, p);
    }
    Stopwatch sw;
    sw.Start();
    w->Abort();
    const double abort_ms = sw.ElapsedMs();
    sw.Start();
    w.reset();   // ct-encode is still sleeping before Finalize
    const double dtor_ms = sw.ElapsedMs();
    std::printf("  Abort %.0f ms, destructor %.1f ms\n", abort_ms, dtor_ms);
    CT_CHECK(abort_ms >= 2900);
    CT_CHECK(dtor_ms < 500);
    // The leaked thread wakes ~1 s later, finalizes the file and exits: the file becomes readable.
    ProbeResult pr;
    bool probed = false;
    for (int i = 0; i < 100 && !probed; ++i) {
        Sleep(50);
        probed = SUCCEEDED(Probe(path, pr));
    }
    std::printf("  file finalized by the leaked thread: %s\n", probed ? "yes" : "no");
    CT_CHECK(probed);
    Sleep(200);
#ifdef _DEBUG
    CT_CHECK(_CrtCheckMemory() != 0);
    _CrtSetDbgFlag(old_flags);
#endif
}
