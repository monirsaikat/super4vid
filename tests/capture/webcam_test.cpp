// ct/capture/webcam_source.h
#include "test_util.h"

#include <ct/capture/webcam_source.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/mf_runtime.h>
#include <ct/test/ct_test.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

using namespace ct;
using namespace ct::capture;
using ct::platform::D3DDevice;
using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kNV12 = 0x3231564E;   // 'NV12'
constexpr uint32_t kYUY2 = 0x32595559;   // 'YUY2'
constexpr uint32_t kMJPG = 0x47504A4D;   // 'MJPG'

WebcamMode Mode(int w, int h, uint32_t num, uint32_t den, uint32_t fourcc, uint32_t index) {
    WebcamMode m;
    m.size = SizeI{w, h};
    m.fps_num = num;
    m.fps_den = den;
    m.fourcc = fourcc;
    m.native_index = index;
    return m;
}

// Media time = a fixed offset from the QPC epoch, with an adjustable "now".
struct FakeClock : MediaTimeSource {
    TimeUs now = 0;
    TimeUs QpcHnsToMediaUs(uint64_t hns) const override { return static_cast<TimeUs>(hns / 10) - 1'000'000; }
    TimeUs NowMediaUs() const override { return now; }
};

void ComInit() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;   // S_FALSE / RPC_E_CHANGED_MODE are fine: COM is usable either way
    (void)platform::MfRuntime::EnsureForProcess();
}

// --- Scripted reader ----------------------------------------------------------------------------------------------

struct Step {
    enum Kind { Dxgi, Cpu, Fail, Flags, NoBuffer } kind = Dxgi;
    uint8_t y = 0, u = 128, v = 128;
    bool has_dts = true;
    TimeUs dts_back_us = 0;   // device timestamp = now - dts_back_us
    HRESULT hr = S_OK;
    DWORD flags = 0;
};

class ScriptedReader : public WebcamReader {
public:
    ScriptedReader(D3DDevice& dev, SizeI size, bool honor_flush = true) : m_dev(dev), m_size(size), m_honor_flush(honor_flush) {}

    void Push(const Step& s) {
        std::lock_guard<std::mutex> l(m_mu);
        m_steps.push_back(s);
        m_cv.notify_all();
    }

    HRESULT ReadSample(DWORD* flags, LONGLONG* ts, IMFSample** out) override {
        *flags = 0;
        *ts = 0;
        *out = nullptr;
        Step s;
        {
            std::unique_lock<std::mutex> l(m_mu);
            m_cv.wait(l, [&] { return m_shut || (m_honor_flush && m_flush_pending) || !m_steps.empty(); });
            if (m_shut) return MF_E_SHUTDOWN;
            if (m_honor_flush && m_flush_pending) {
                m_flush_pending = false;
                return S_OK;
            }
            s = m_steps.front();
            m_steps.pop_front();
        }
        reads.fetch_add(1);
        if (s.kind == Step::Fail) return s.hr;
        if (s.kind == Step::Flags) {
            *flags = s.flags;
            return S_OK;
        }
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(sample.GetAddressOf()))) return E_FAIL;
        const int w = m_size.w, h = m_size.h;
        std::vector<uint8_t> frame(static_cast<size_t>(w) * h * 3 / 2, s.y);
        for (size_t i = static_cast<size_t>(w) * h; i < frame.size(); i += 2) {
            frame[i] = s.u;
            frame[i + 1] = s.v;
        }
        ComPtr<IMFMediaBuffer> buf;
        if (s.kind == Step::Dxgi) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(w);
            d.Height = static_cast<UINT>(h);
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_NV12;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{frame.data(), static_cast<UINT>(w), 0};
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(m_dev.Device()->CreateTexture2D(&d, &init, tex.GetAddressOf()))) return E_FAIL;
            if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex.Get(), 0, FALSE, buf.GetAddressOf()))) return E_FAIL;
        } else if (s.kind == Step::Cpu) {
            if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), buf.GetAddressOf()))) return E_FAIL;
            BYTE* p = nullptr;
            buf->Lock(&p, nullptr, nullptr);
            std::memcpy(p, frame.data(), frame.size());
            buf->Unlock();
            buf->SetCurrentLength(static_cast<DWORD>(frame.size()));
        }
        if (buf) sample->AddBuffer(buf.Get());
        if (s.has_dts) {
            const uint64_t now_hns = static_cast<uint64_t>(Clock::QpcToUs(Clock::QpcNow(), 0)) * 10;
            sample->SetUINT64(MFSampleExtension_DeviceTimestamp, now_hns - static_cast<uint64_t>(s.dts_back_us) * 10);
        }
        *out = sample.Detach();
        return S_OK;
    }
    HRESULT Flush() override {
        flushes.fetch_add(1);
        std::lock_guard<std::mutex> l(m_mu);
        m_flush_pending = true;
        m_cv.notify_all();
        return S_OK;
    }
    void Shutdown() override {
        shutdowns.fetch_add(1);
        std::lock_guard<std::mutex> l(m_mu);
        m_shut = true;
        m_cv.notify_all();
    }

    std::atomic<int> reads{0}, flushes{0}, shutdowns{0};

private:
    D3DDevice& m_dev;
    SizeI m_size;
    bool m_honor_flush;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Step> m_steps;
    bool m_flush_pending = false;
    bool m_shut = false;
};

struct Frame { uint32_t index; TimeUs t_us; };

// Keeps (hold = true) or releases every frame; optionally rejects.
struct TestSink : FrameSink {
    platform::TexturePool* pool = nullptr;
    bool hold = false, reject = false;
    std::mutex mu;
    std::vector<Frame> frames;
    std::atomic<int> errors{0};
    std::atomic<long> last_error{S_OK};
    bool OnFrame(ID3D11Texture2D*, uint32_t index, TimeUs t_us) override {
        if (reject) return false;
        {
            std::lock_guard<std::mutex> l(mu);
            frames.push_back(Frame{index, t_us});
        }
        if (!hold) pool->Release(index);
        return true;
    }
    void OnError(HRESULT hr) override {
        last_error.store(hr);
        errors.fetch_add(1);
    }
    size_t Count() {
        std::lock_guard<std::mutex> l(mu);
        return frames.size();
    }
};

template <typename F>
bool WaitFor(F cond, int timeout_ms) {
    const int64_t end = Clock::QpcNow() + Clock::QpcFrequency() * timeout_ms / 1000;
    while (!cond()) {
        if (Clock::QpcNow() > end) return false;
        Sleep(2);
    }
    return true;
}

TimeUs ElapsedUs(int64_t t0) { return Clock::QpcToUs(Clock::QpcNow(), t0); }

bool RefuseAll(void*, uint32_t) { return false; }

} // namespace

// --- Pure parts ---------------------------------------------------------------------------------------------------

CT_TEST(webcam_subtype_rank) {
    CT_CHECK_EQ(WebcamSubtypeRank(kNV12), 0);
    CT_CHECK_EQ(WebcamSubtypeRank(kYUY2), 1);
    CT_CHECK_EQ(WebcamSubtypeRank(kMJPG), 2);
    CT_CHECK_EQ(WebcamSubtypeRank(0x34363248 /* H264 */), 3);
}

CT_TEST(webcam_pick_mode_area_first) {
    std::vector<WebcamMode> modes = {Mode(640, 480, 30, 1, kNV12, 0), Mode(1920, 1080, 30, 1, kMJPG, 1),
                                     Mode(1280, 720, 10, 1, kYUY2, 2), Mode(1280, 720, 30, 1, kMJPG, 3)};
    CT_CHECK_EQ(PickWebcamMode(modes, SizeI{1280, 720}, 30), 3);   // exact area, exact fps beats NV12 640x480
    CT_CHECK_EQ(PickWebcamMode(modes, SizeI{1920, 1080}, 60), 1);
    CT_CHECK_EQ(PickWebcamMode(modes, SizeI{320, 240}, 30), 0);
}

CT_TEST(webcam_pick_mode_fps_then_subtype) {
    std::vector<WebcamMode> modes = {Mode(1280, 720, 15, 1, kNV12, 0), Mode(1280, 720, 30, 1, kMJPG, 1),
                                     Mode(1280, 720, 30, 1, kYUY2, 2), Mode(1280, 720, 30000, 1001, kNV12, 3)};
    // 29.97 is 30 ms-fps away from 30; 30 fps YUY2 beats 30 fps MJPG on subtype.
    CT_CHECK_EQ(PickWebcamMode(modes, SizeI{1280, 720}, 30), 2);
    CT_CHECK_EQ(PickWebcamMode(modes, SizeI{1280, 720}, 15), 0);
    // Equal everything: the first one wins.
    std::vector<WebcamMode> same = {Mode(640, 480, 30, 1, kMJPG, 7), Mode(640, 480, 30, 1, kMJPG, 8)};
    CT_CHECK_EQ(PickWebcamMode(same, SizeI{640, 480}, 30), 0);
}

CT_TEST(webcam_pick_mode_skips_invalid) {
    std::vector<WebcamMode> modes = {Mode(1281, 720, 30, 1, kNV12, 0), Mode(1280, 720, 0, 1, kNV12, 1),
                                     Mode(0, 0, 30, 1, kNV12, 2), Mode(320, 240, 30, 1, kMJPG, 3)};
    CT_CHECK_EQ(PickWebcamMode(modes, SizeI{1280, 720}, 30), 3);
    CT_CHECK_EQ(PickWebcamMode({}, SizeI{1280, 720}, 30), -1);
    std::vector<WebcamMode> bad = {Mode(1281, 721, 30, 1, kNV12, 0)};
    CT_CHECK_EQ(PickWebcamMode(bad, SizeI{1280, 720}, 30), -1);
}

CT_TEST(webcam_frame_time_mapping) {
    FakeClock clock;
    clock.now = 5'000'000;
    bool dev = false;
    // Device timestamp for media time 4.98 s: hns = (4.98 s + 1 s offset) * 1e7.
    const uint64_t hns = (4'980'000ull + 1'000'000ull) * 10;
    CT_CHECK_EQ(WebcamFrameTimeUs(clock, true, hns, &dev), TimeUs{4'980'000});
    CT_CHECK(dev);
    CT_CHECK_EQ(WebcamFrameTimeUs(clock, false, hns, &dev), TimeUs{5'000'000});
    CT_CHECK(!dev);
    CT_CHECK_EQ(WebcamFrameTimeUs(clock, true, 0, &dev), TimeUs{5'000'000});   // zero = absent
    CT_CHECK(!dev);
    // Older than the 1 s skew limit: implausible, use now.
    const uint64_t old_hns = (3'900'000ull + 1'000'000ull) * 10;
    CT_CHECK_EQ(WebcamFrameTimeUs(clock, true, old_hns, &dev), TimeUs{5'000'000});
    CT_CHECK(!dev);
    // Slightly in the future: clamped to now, still counted as a device timestamp.
    const uint64_t future_hns = (5'010'000ull + 1'000'000ull) * 10;
    CT_CHECK_EQ(WebcamFrameTimeUs(clock, true, future_hns, &dev), TimeUs{5'000'000});
    CT_CHECK(dev);
    // Exactly at the skew limit is accepted.
    const uint64_t edge_hns = (4'000'000ull + 1'000'000ull) * 10;
    CT_CHECK_EQ(WebcamFrameTimeUs(clock, true, edge_hns, &dev), TimeUs{4'000'000});
}

CT_TEST(webcam_qpc_media_time_source) {
    const int64_t epoch = Clock::QpcNow() - Clock::QpcFrequency();   // 1 s ago
    QpcMediaTimeSource clock(epoch);
    const TimeUs now = clock.NowMediaUs();
    CT_CHECK(now >= 1'000'000 && now < 1'100'000);
    const uint64_t now_hns = static_cast<uint64_t>(Clock::QpcToUs(Clock::QpcNow(), 0)) * 10;
    const TimeUs mapped = clock.QpcHnsToMediaUs(now_hns);
    const TimeUs diff = mapped > clock.NowMediaUs() ? mapped - clock.NowMediaUs() : clock.NowMediaUs() - mapped;
    CT_CHECK(diff < 2'000);
}

// --- Source loop over a scripted reader (needs a D3D device, no camera) -------------------------------------------

CT_TEST_DEVICE(webcam_scripted_frames_dxgi_and_cpu) {
    ComInit();
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    const SizeI size{64, 32};
    auto reader = std::make_unique<ScriptedReader>(dev, size);
    ScriptedReader* r = reader.get();
    WebcamSource cam;
    CT_REQUIRE(SUCCEEDED(cam.InitWithReader(dev, std::move(reader), size, 30, 1)));
    CT_CHECK_EQ(cam.Nv12Pool().Count(), 6u);
    CT_CHECK_EQ(cam.ActualFps(), 30);
    CT_CHECK(!cam.LatestPreviewPlanes(nullptr, nullptr, nullptr));   // nothing yet
    TestSink sink;
    sink.pool = &cam.Nv12Pool();
    sink.hold = true;
    QpcMediaTimeSource clock(Clock::QpcNow() - Clock::QpcFrequency());
    CT_REQUIRE(cam.Start(clock, sink));
    const uint8_t ys[5] = {40, 80, 120, 160, 200};
    for (int i = 0; i < 5; ++i) {
        Step s;
        s.kind = i < 3 ? Step::Dxgi : Step::Cpu;
        s.y = ys[i];
        s.u = static_cast<uint8_t>(60 + i);
        s.v = static_cast<uint8_t>(190 - i);
        s.has_dts = i != 1;   // frame 1 uses the fallback clock
        s.dts_back_us = 5'000;
        r->Push(s);
        Sleep(15);
    }
    CT_CHECK(WaitFor([&] { return sink.Count() == 5; }, 2000));
    // The reader now blocks with nothing to deliver: Stop must unblock it with Flush.
    const int64_t t0 = Clock::QpcNow();
    cam.Stop();
    const TimeUs stop_us = ElapsedUs(t0);
    std::printf("    stop with a blocked reader: %.1f ms, %d flush(es)\n", stop_us / 1000.0, r->flushes.load());
    CT_CHECK(stop_us < 500'000);
    CT_CHECK(r->flushes.load() >= 1);
    CT_CHECK_EQ(r->shutdowns.load(), 0);
    const WebcamStats& st = cam.Stats();
    CT_CHECK_EQ(st.frames.load(), 5u);
    CT_CHECK_EQ(st.device_timestamps.load(), 4u);
    CT_CHECK_EQ(st.fallback_timestamps.load(), 1u);
    CT_CHECK_EQ(st.cpu_uploads.load(), 2u);
    CT_CHECK_EQ(st.dropped_bad_buffer.load(), 0u);
    CT_REQUIRE(sink.frames.size() == 5);
    for (size_t i = 1; i < sink.frames.size(); ++i) CT_CHECK(sink.frames[i].t_us > sink.frames[i - 1].t_us);
    CT_CHECK(sink.frames[0].t_us > 900'000 && sink.frames[0].t_us < 2'000'000);
    CT_CHECK_EQ(st.first_frame_us.load(), sink.frames[0].t_us);
    // Content: every held pool texture has the scripted Y / UV values.
    for (size_t i = 0; i < sink.frames.size(); ++i) {
        std::vector<uint8_t> px;
        UINT w = 0, h = 0;
        CT_REQUIRE(test_util::ReadbackTexture(dev, cam.Nv12Pool().At(sink.frames[i].index), px, w, h));
        CT_CHECK_EQ(w, 64u);
        CT_CHECK_EQ(h, 32u);
        CT_CHECK_EQ(px[0], ys[i]);
        CT_CHECK_EQ(px[static_cast<size_t>(w) * h - 1], ys[i]);
        CT_CHECK_EQ(px[static_cast<size_t>(w) * h], static_cast<uint8_t>(60 + i));
        CT_CHECK_EQ(px[px.size() - 1], static_cast<uint8_t>(190 - i));
    }
    for (const Frame& f : sink.frames) cam.Nv12Pool().Release(f.index);
    CT_CHECK_EQ(cam.Nv12Pool().InUse(), 0u);
    ID3D11ShaderResourceView* y = nullptr;
    ID3D11ShaderResourceView* uv = nullptr;
    SizeI psize{};
    if (dev.IsWarp() || cam.PreviewSerial() == 0) {
        std::printf("    preview planes unavailable on this device\n");
    } else {
        CT_CHECK(cam.LatestPreviewPlanes(&y, &uv, &psize));
        CT_CHECK(y != nullptr && uv != nullptr);
        CT_CHECK_EQ(psize.w, 64);
        CT_CHECK_EQ(cam.PreviewSerial(), 5u);
    }
    CT_CHECK_EQ(sink.errors.load(), 0);
}

CT_TEST_DEVICE(webcam_scripted_pool_predicate_and_sink_reject) {
    ComInit();
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    const SizeI size{32, 16};
    auto reader = std::make_unique<ScriptedReader>(dev, size);
    ScriptedReader* r = reader.get();
    WebcamSource cam;
    CT_REQUIRE(SUCCEEDED(cam.InitWithReader(dev, std::move(reader), size, 30, 1)));
    TestSink sink;
    sink.pool = &cam.Nv12Pool();
    QpcMediaTimeSource clock(Clock::QpcNow());
    // The cam writer keeps every texture busy: frames are dropped, never written into a held texture.
    cam.Nv12Pool().SetReusePredicate(&RefuseAll, nullptr);
    CT_REQUIRE(cam.Start(clock, sink));
    for (int i = 0; i < 3; ++i) {
        r->Push(Step{});
        Sleep(5);
    }
    CT_CHECK(WaitFor([&] { return cam.Stats().dropped_pool_empty.load() == 3; }, 2000));
    cam.Stop();
    CT_CHECK_EQ(sink.Count(), 0u);
    CT_CHECK_EQ(cam.Nv12Pool().InUse(), 0u);
    // A full sink: the source releases the texture and counts the drop.
    cam.Nv12Pool().SetReusePredicate(nullptr, nullptr);
    sink.reject = true;
    CT_REQUIRE(cam.Start(clock, sink));   // restart after Stop reuses the reader
    for (int i = 0; i < 4; ++i) {
        r->Push(Step{});
        Sleep(5);
    }
    CT_CHECK(WaitFor([&] { return cam.Stats().dropped_sink.load() == 4; }, 2000));
    cam.Stop();
    CT_CHECK_EQ(cam.Nv12Pool().InUse(), 0u);
    CT_CHECK_EQ(cam.Stats().frames.load(), 0u);
}

CT_TEST_DEVICE(webcam_scripted_pause_late_ticks_bad_buffers) {
    ComInit();
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    const SizeI size{32, 16};
    auto reader = std::make_unique<ScriptedReader>(dev, size);
    ScriptedReader* r = reader.get();
    WebcamSource cam;
    CT_REQUIRE(SUCCEEDED(cam.InitWithReader(dev, std::move(reader), size, 30, 1)));
    TestSink sink;
    sink.pool = &cam.Nv12Pool();
    QpcMediaTimeSource clock(Clock::QpcNow());
    cam.SetPaused(true);
    CT_REQUIRE(cam.Start(clock, sink));
    r->Push(Step{});
    r->Push(Step{});
    CT_CHECK(WaitFor([&] { return cam.Stats().dropped_paused.load() == 2; }, 2000));
    cam.SetPaused(false);
    Step a;
    a.dts_back_us = 0;
    r->Push(a);
    CT_CHECK(WaitFor([&] { return sink.Count() == 1; }, 2000));
    Step late;
    late.dts_back_us = 500'000;   // captured before the frame already delivered
    r->Push(late);
    Step tick;
    tick.kind = Step::Flags;
    tick.flags = MF_SOURCE_READERF_STREAMTICK;
    r->Push(tick);
    Step nobuf;
    nobuf.kind = Step::NoBuffer;
    r->Push(nobuf);
    Sleep(20);
    r->Push(Step{});
    CT_CHECK(WaitFor([&] { return sink.Count() == 2; }, 2000));
    cam.Stop();
    const WebcamStats& st = cam.Stats();
    CT_CHECK_EQ(st.dropped_late.load(), 1u);
    CT_CHECK_EQ(st.stream_ticks.load(), 1u);
    CT_CHECK_EQ(st.dropped_bad_buffer.load(), 1u);
    CT_CHECK_EQ(st.frames.load(), 2u);
    CT_CHECK_EQ(cam.Nv12Pool().InUse(), 0u);
}

CT_TEST_DEVICE(webcam_scripted_reader_error_reports_once) {
    ComInit();
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    const SizeI size{32, 16};
    for (int variant = 0; variant < 2; ++variant) {
        auto reader = std::make_unique<ScriptedReader>(dev, size);
        ScriptedReader* r = reader.get();
        WebcamSource cam;
        CT_REQUIRE(SUCCEEDED(cam.InitWithReader(dev, std::move(reader), size, 30, 1)));
        TestSink sink;
        sink.pool = &cam.Nv12Pool();
        QpcMediaTimeSource clock(Clock::QpcNow());
        CT_REQUIRE(cam.Start(clock, sink));
        r->Push(Step{});
        Step err;
        if (variant == 0) {
            err.kind = Step::Fail;
            err.hr = MF_E_HW_MFT_FAILED_START_STREAMING;   // e.g. the camera was unplugged / taken by another app
        } else {
            err.kind = Step::Flags;
            err.flags = MF_SOURCE_READERF_ENDOFSTREAM;
        }
        r->Push(err);
        r->Push(Step{});   // never read: the thread exits
        CT_CHECK(WaitFor([&] { return sink.errors.load() == 1 && !cam.IsRunning(); }, 2000));
        CT_CHECK_EQ(sink.last_error.load(), variant == 0 ? static_cast<long>(MF_E_HW_MFT_FAILED_START_STREAMING)
                                                         : static_cast<long>(MF_E_END_OF_STREAM));
        CT_CHECK_EQ(sink.Count(), 1u);
        cam.Stop();   // joins the exited thread
        CT_CHECK_EQ(sink.errors.load(), 1);
        CT_CHECK_EQ(r->reads.load(), 2);
    }
}

CT_TEST_DEVICE(webcam_stop_shuts_down_a_reader_that_ignores_flush) {
    ComInit();
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    const SizeI size{32, 16};
    auto reader = std::make_unique<ScriptedReader>(dev, size, /*honor_flush=*/false);
    WebcamSource cam;
    CT_REQUIRE(SUCCEEDED(cam.InitWithReader(dev, std::move(reader), size, 30, 1)));
    TestSink sink;
    sink.pool = &cam.Nv12Pool();
    QpcMediaTimeSource clock(Clock::QpcNow());
    CT_REQUIRE(cam.Start(clock, sink));
    Sleep(20);
    const int64_t t0 = Clock::QpcNow();
    cam.Stop();
    const TimeUs us = ElapsedUs(t0);
    std::printf("    stop with a flush-deaf reader: %.0f ms\n", us / 1000.0);
    CT_CHECK(us >= 2'900'000 && us < 4'500'000);
    CT_CHECK(!cam.IsRunning());
    CT_CHECK(!cam.Start(clock, sink));   // the device is gone until Init
    CT_CHECK_EQ(sink.errors.load(), 0);   // a stop is not an error
}

CT_TEST_DEVICE(webcam_synthetic_source_1s) {
    ComInit();
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    for (int dxgi = 1; dxgi >= 0; --dxgi) {
        const SizeI size{640, 360};
        WebcamSource cam;
        auto reader = CreateSyntheticWebcamReader(dev, size, 30, dxgi != 0);
        CT_REQUIRE(reader != nullptr);
        CT_REQUIRE(SUCCEEDED(cam.InitWithReader(dev, std::move(reader), size, 30, 1)));
        TestSink sink;
        sink.pool = &cam.Nv12Pool();
        QpcMediaTimeSource clock(Clock::QpcNow());
        const int64_t t0 = Clock::QpcNow();
        CT_REQUIRE(cam.Start(clock, sink));
        Sleep(1000);
        cam.Stop();
        const uint64_t max_frames = static_cast<uint64_t>(ElapsedUs(t0) * 30 / 1'000'000) + 2;   // Sleep can overshoot
        const WebcamStats& st = cam.Stats();
        std::printf("    synthetic %s: %llu frames, %llu device ts, drops %llu\n", dxgi ? "dxgi" : "memory",
                    (unsigned long long)st.frames.load(), (unsigned long long)st.device_timestamps.load(),
                    (unsigned long long)(st.dropped_pool_empty.load() + st.dropped_bad_buffer.load() + st.dropped_late.load()));
        // Paced with Sleep: under load (parallel ctest, other builds) the generator falls behind; this is a smoke check.
        CT_CHECK(st.frames.load() >= 10 && st.frames.load() <= max_frames);
        CT_CHECK_EQ(st.device_timestamps.load(), st.frames.load());
        CT_CHECK_EQ(st.cpu_uploads.load(), dxgi ? 0u : st.frames.load());
        CT_CHECK_EQ(cam.Nv12Pool().InUse(), 0u);
        // Frame spacing ~33 ms.
        if (sink.frames.size() > 2) {
            const double avg = static_cast<double>(sink.frames.back().t_us - sink.frames.front().t_us) / (sink.frames.size() - 1);
            CT_CHECK(avg > 30'000 && avg < 100'000);
        }
    }
}

// --- Real camera ----------------------------------------------------------------------------------------------------

CT_TEST_DEVICE(webcam_enumerate) {
    ComInit();
    std::vector<WebcamInfo> cams;
    CT_CHECK_HR(WebcamSource::Enumerate(cams, true));
    std::printf("    %zu webcam(s)\n", cams.size());
    for (const WebcamInfo& c : cams) {
        std::printf("    \"%s\": %zu modes, %zu sizes\n", c.name.c_str(), c.modes.size(), c.sizes.size());
        CT_CHECK(!c.symbolic_link.empty());
        for (size_t i = 1; i < c.sizes.size(); ++i)
            CT_CHECK(static_cast<int64_t>(c.sizes[i - 1].w) * c.sizes[i - 1].h >= static_cast<int64_t>(c.sizes[i].w) * c.sizes[i].h);
    }
}

CT_TEST_DEVICE(webcam_real_device_2s) {
    ComInit();
    std::vector<WebcamInfo> cams;
    CT_REQUIRE(SUCCEEDED(WebcamSource::Enumerate(cams, false)));
    if (cams.empty()) CT_SKIP("no webcam on this machine");
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    WebcamSource cam;
    WebcamSource::Config cfg;
    cfg.symbolic_link = cams[0].symbolic_link;
    const HRESULT hr = cam.Init(dev, cfg);
    if (hr == E_ACCESSDENIED) CT_SKIP("camera access denied (Settings > Privacy > Camera)");
    CT_REQUIRE(SUCCEEDED(hr));
    std::printf("    \"%s\" %dx%d @ %d fps\n", cam.DeviceName().c_str(), cam.ActualSize().w, cam.ActualSize().h, cam.ActualFps());
    TestSink sink;
    sink.pool = &cam.Nv12Pool();
    QpcMediaTimeSource clock(Clock::QpcNow());
    CT_REQUIRE(cam.Start(clock, sink));
    Sleep(2000);
    const int64_t t0 = Clock::QpcNow();
    cam.Stop();
    std::printf("    %llu frames, stop %.1f ms\n", (unsigned long long)cam.Stats().frames.load(), ElapsedUs(t0) / 1000.0);
    CT_CHECK(cam.Stats().frames.load() >= 10);
    CT_CHECK(ElapsedUs(t0) < 1'000'000);
    CT_CHECK_EQ(sink.errors.load(), 0);
}

// Init without a camera fails cleanly.
CT_TEST_DEVICE(webcam_init_without_camera) {
    ComInit();
    std::vector<WebcamInfo> cams;
    CT_REQUIRE(SUCCEEDED(WebcamSource::Enumerate(cams, false)));
    if (!cams.empty()) CT_SKIP("a webcam is present");
    D3DDevice dev;
    CT_REQUIRE(SUCCEEDED(dev.Init(true)));
    WebcamSource cam;
    CT_CHECK_EQ(cam.Init(dev, WebcamSource::Config{}), HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    TestSink sink;
    QpcMediaTimeSource clock(Clock::QpcNow());
    CT_CHECK(!cam.Start(clock, sink));
    cam.Stop();
}
