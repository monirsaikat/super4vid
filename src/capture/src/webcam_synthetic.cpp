#define CT_LOG_CATEGORY "webcam"
#include <ct/capture/webcam_source.h>

#include "webcam_internal.h"

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/sync.h>
#include <ct/core/time.h>
#include <ct/platform/mf_runtime.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ct::capture {

namespace {

struct Yuv { uint8_t y, u, v; };

Yuv Bt709Limited(double r, double g, double b) {
    const double yn = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    auto clamp8 = [](double v) { return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L)); };
    return Yuv{clamp8(16.0 + 219.0 * yn), clamp8(128.0 + 224.0 * (b - yn) / 1.8556), clamp8(128.0 + 224.0 * (r - yn) / 1.5748)};
}

// White, yellow, cyan, green, magenta, red, blue, black (75 % bars).
const double kBars[8][3] = {{.75, .75, .75}, {.75, .75, 0}, {0, .75, .75}, {0, .75, 0},
                            {.75, 0, .75},   {.75, 0, 0},   {0, 0, .75},   {0, 0, 0}};

class SyntheticReader final : public WebcamReader {
public:
    SyntheticReader(platform::D3DDevice& dev, SizeI size, int fps, bool dxgi)
        : m_dev(dev), m_size(size), m_fps(std::max(1, fps)), m_dxgi(dxgi) {
        for (int i = 0; i < 8; ++i) m_bar_yuv[i] = Bt709Limited(kBars[i][0], kBars[i][1], kBars[i][2]);
        m_frame.resize(static_cast<size_t>(size.w) * size.h * 3 / 2);
        if (m_dxgi) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = static_cast<UINT>(size.w);
            d.Height = static_cast<UINT>(size.h);
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_NV12;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            for (auto& t : m_tex) CT_LOG_IF_FAILED(dev.Device()->CreateTexture2D(&d, nullptr, t.GetAddressOf()));
            d.Usage = D3D11_USAGE_STAGING;
            d.BindFlags = 0;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            CT_LOG_IF_FAILED(dev.Device()->CreateTexture2D(&d, nullptr, m_staging.GetAddressOf()));
        }
        m_next_qpc = Clock::QpcNow();
    }

    HRESULT ReadSample(DWORD* flags, LONGLONG* ts, IMFSample** out) override {
        *flags = 0;
        *ts = 0;
        *out = nullptr;
        if (m_shut.load()) return MF_E_SHUTDOWN;
        const int64_t now = Clock::QpcNow();
        if (m_next_qpc > now) {
            const DWORD ms = static_cast<DWORD>((m_next_qpc - now) * 1000 / Clock::QpcFrequency());
            if (m_flush.Wait(ms)) return m_shut.load() ? MF_E_SHUTDOWN : S_OK;   // flushed: no sample
            while (Clock::QpcNow() < m_next_qpc) Sleep(0);
        }
        const int64_t capture_qpc = Clock::QpcNow();
        m_next_qpc += Clock::QpcFrequency() / m_fps;
        if (m_next_qpc < capture_qpc) m_next_qpc = capture_qpc + Clock::QpcFrequency() / m_fps;   // fell behind

        Generate(m_count);
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        CT_RETURN_IF_FAILED(MFCreateSample(sample.GetAddressOf()));
        if (m_dxgi) {
            ID3D11Texture2D* tex = m_tex[m_count % 2].Get();
            if (!tex || !m_staging) return E_OUTOFMEMORY;
            {
                platform::D3DDevice::Lock lock(m_dev);
                CT_RETURN_IF_FAILED(internal::UploadNv12(m_dev, m_staging.Get(), tex, m_frame.data(),
                                                         static_cast<size_t>(m_size.w), m_size));
            }
            CT_RETURN_IF_FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), tex, 0, FALSE, buffer.GetAddressOf()));
        } else {
            const DWORD len = static_cast<DWORD>(m_frame.size());
            CT_RETURN_IF_FAILED(MFCreateMemoryBuffer(len, buffer.GetAddressOf()));
            BYTE* p = nullptr;
            CT_RETURN_IF_FAILED(buffer->Lock(&p, nullptr, nullptr));
            std::memcpy(p, m_frame.data(), len);
            buffer->Unlock();
            buffer->SetCurrentLength(len);
        }
        CT_RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
        const uint64_t qpc_hns = static_cast<uint64_t>(Clock::QpcToUs(capture_qpc, 0)) * 10;
        sample->SetUINT64(MFSampleExtension_DeviceTimestamp, qpc_hns);
        *ts = static_cast<LONGLONG>(qpc_hns);
        sample->SetSampleTime(*ts);
        sample->SetSampleDuration(10'000'000 / m_fps);
        ++m_count;
        *out = sample.Detach();
        return S_OK;
    }

    HRESULT Flush() override {
        m_flush.Set();
        return S_OK;
    }
    void Shutdown() override {
        m_shut.store(true);
        m_flush.Set();
    }

private:
    void Generate(uint64_t n) {
        const int w = m_size.w, h = m_size.h;
        const int bar_w = std::max(2, w / 8);
        const int shift = static_cast<int>((n * 4) % static_cast<uint64_t>(w));
        uint8_t* y = m_frame.data();
        uint8_t* uv = y + static_cast<size_t>(w) * h;
        // Top 3/4: moving bars. Bottom 1/4: a static luma ramp (16..235) with neutral chroma.
        const int bars_h = (h * 3 / 4) & ~1;
        for (int x = 0; x < w; ++x) y[x] = m_bar_yuv[((x + shift) / bar_w) % 8].y;
        for (int r = 1; r < bars_h; ++r) std::memcpy(y + static_cast<size_t>(r) * w, y, static_cast<size_t>(w));
        for (int x = 0; x < w; x += 2) {
            const Yuv c = m_bar_yuv[((x + shift) / bar_w) % 8];
            uv[x] = c.u;
            uv[x + 1] = c.v;
        }
        for (int r = 1; r < bars_h / 2; ++r) std::memcpy(uv + static_cast<size_t>(r) * w, uv, static_cast<size_t>(w));
        for (int r = bars_h; r < h; ++r)
            for (int x = 0; x < w; ++x) y[static_cast<size_t>(r) * w + x] = static_cast<uint8_t>(16 + 219 * x / std::max(1, w - 1));
        for (int r = bars_h / 2; r < h / 2; ++r) std::memset(uv + static_cast<size_t>(r) * w, 128, static_cast<size_t>(w));
    }

    platform::D3DDevice& m_dev;
    SizeI m_size;
    int m_fps;
    bool m_dxgi;
    Yuv m_bar_yuv[8]{};
    std::vector<uint8_t> m_frame;
    ComPtr<ID3D11Texture2D> m_tex[2];
    ComPtr<ID3D11Texture2D> m_staging;
    Event m_flush{false, false};
    std::atomic<bool> m_shut{false};
    int64_t m_next_qpc = 0;
    uint64_t m_count = 0;
};

} // namespace

std::unique_ptr<WebcamReader> CreateSyntheticWebcamReader(platform::D3DDevice& dev, SizeI size, int fps, bool dxgi_buffers) {
    if (size.IsEmpty() || (size.w & 1) || (size.h & 1)) return nullptr;
    if (FAILED(platform::MfRuntime::EnsureForProcess())) return nullptr;
    return std::make_unique<SyntheticReader>(dev, size, fps, dxgi_buffers);
}

} // namespace ct::capture
