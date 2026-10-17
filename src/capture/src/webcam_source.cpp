#define CT_LOG_CATEGORY "webcam"
#include <ct/capture/webcam_source.h>

#include "webcam_internal.h"

#include <ct/core/assert.h>
#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>
#include <ct/platform/mf_runtime.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace ct::capture {

namespace internal {

void FillNv12(uint8_t* y, size_t y_pitch, uint8_t* uv, size_t uv_pitch, SizeI size, uint8_t y_val, uint8_t u_val,
              uint8_t v_val) {
    for (int r = 0; r < size.h; ++r) std::memset(y + r * y_pitch, y_val, static_cast<size_t>(size.w));
    for (int r = 0; r < size.h / 2; ++r) {
        uint8_t* row = uv + r * uv_pitch;
        for (int c = 0; c < size.w / 2; ++c) {
            row[2 * c] = u_val;
            row[2 * c + 1] = v_val;
        }
    }
}

HRESULT UploadNv12(platform::D3DDevice& dev, ID3D11Texture2D* staging, ID3D11Texture2D* dst, const uint8_t* src,
                   size_t src_pitch, SizeI size) {
    // Caller holds D3DDevice::Lock. Staging NV12 layout: the UV plane follows the Y plane at RowPitch * Height.
    D3D11_TEXTURE2D_DESC d{};
    staging->GetDesc(&d);
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = dev.Context()->Map(staging, 0, D3D11_MAP_WRITE, 0, &m);
    if (FAILED(hr)) return hr;
    auto* dst_y = static_cast<uint8_t*>(m.pData);
    uint8_t* dst_uv = dst_y + static_cast<size_t>(m.RowPitch) * d.Height;
    const size_t w = static_cast<size_t>(std::min<int>(size.w, static_cast<int>(d.Width)));
    const int h = std::min<int>(size.h, static_cast<int>(d.Height));
    for (int r = 0; r < h; ++r) std::memcpy(dst_y + r * m.RowPitch, src + r * src_pitch, w);
    const uint8_t* src_uv = src + src_pitch * static_cast<size_t>(size.h);
    for (int r = 0; r < h / 2; ++r) std::memcpy(dst_uv + r * m.RowPitch, src_uv + r * src_pitch, w);
    dev.Context()->Unmap(staging, 0);
    dev.Context()->CopyResource(dst, staging);
    return S_OK;
}

} // namespace internal

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

std::string FourCCString(uint32_t f) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((f >> (8 * i)) & 0xFF);
        s.push_back(c >= 32 && c < 127 ? c : '?');
    }
    return s;
}

bool ModeFromType(IMFMediaType* type, uint32_t index, WebcamMode& out) {
    GUID major{}, sub{};
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) return false;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &sub))) return false;
    UINT32 w = 0, h = 0, num = 0, den = 0;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h))) return false;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den))) { num = 0; den = 1; }
    out.size = SizeI{static_cast<int32_t>(w), static_cast<int32_t>(h)};
    out.fps_num = num;
    out.fps_den = den ? den : 1;
    out.fourcc = sub.Data1;
    out.native_index = index;
    return true;
}

void ListNativeModes(IMFSourceReader* reader, std::vector<WebcamMode>& modes) {
    modes.clear();
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i,
                                                      type.GetAddressOf());
        if (FAILED(hr)) break;   // MF_E_NO_MORE_TYPES
        WebcamMode m;
        if (ModeFromType(type.Get(), i, m)) modes.push_back(m);
    }
}

std::string AllocatedString(IMFActivate* act, const GUID& key) {
    WCHAR* s = nullptr;
    UINT32 len = 0;
    std::string out;
    if (SUCCEEDED(act->GetAllocatedString(key, &s, &len)) && s) {
        out = WideToUtf8(std::wstring_view(s, len));
        CoTaskMemFree(s);
    }
    return out;
}

std::wstring AllocatedWString(IMFActivate* act, const GUID& key) {
    WCHAR* s = nullptr;
    UINT32 len = 0;
    std::wstring out;
    if (SUCCEEDED(act->GetAllocatedString(key, &s, &len)) && s) {
        out.assign(s, len);
        CoTaskMemFree(s);
    }
    return out;
}

D3D11_TEXTURE2D_DESC Nv12Desc(SizeI size, UINT bind) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(size.w);
    d.Height = static_cast<UINT>(size.h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = bind;
    return d;
}

// The MF source reader over a device source.
class MfWebcamReader final : public WebcamReader {
public:
    MfWebcamReader(ComPtr<IMFSourceReader> reader, ComPtr<IMFMediaSource> source)
        : m_reader(std::move(reader)), m_source(std::move(source)) {}
    ~MfWebcamReader() override {
        Shutdown();
        m_reader.Reset();
    }
    HRESULT ReadSample(DWORD* flags, LONGLONG* ts, IMFSample** sample) override {
        DWORD stream = 0;
        return m_reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, flags, ts, sample);
    }
    HRESULT Flush() override { return m_reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM)); }
    void Shutdown() override {
        if (!m_shut.exchange(true) && m_source) CT_LOG_IF_FAILED(m_source->Shutdown());
    }

private:
    ComPtr<IMFSourceReader> m_reader;
    ComPtr<IMFMediaSource> m_source;
    std::atomic<bool> m_shut{false};
};

} // namespace

// --- Pure helpers -------------------------------------------------------------------------------------------------

TimeUs QpcMediaTimeSource::QpcHnsToMediaUs(uint64_t qpc_hns) const {
    return static_cast<TimeUs>(qpc_hns / 10) - Clock::QpcToUs(m_epoch_qpc, 0);
}

TimeUs QpcMediaTimeSource::NowMediaUs() const { return Clock::QpcToUs(Clock::QpcNow(), m_epoch_qpc); }

int WebcamSubtypeRank(uint32_t fourcc) {
    if (fourcc == FourCC('N', 'V', '1', '2')) return 0;
    if (fourcc == FourCC('Y', 'U', 'Y', '2')) return 1;
    if (fourcc == FourCC('M', 'J', 'P', 'G')) return 2;
    return 3;
}

int PickWebcamMode(const std::vector<WebcamMode>& modes, SizeI preferred_size, int preferred_fps) {
    const int64_t want_area = static_cast<int64_t>(preferred_size.w) * preferred_size.h;
    const int64_t want_mfps = static_cast<int64_t>(preferred_fps) * 1000;
    int best = -1;
    int64_t best_area = 0, best_fps = 0;
    int best_rank = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
        const WebcamMode& m = modes[i];
        if (m.size.IsEmpty() || (m.size.w & 1) || (m.size.h & 1) || m.fps_num == 0 || m.fps_den == 0) continue;
        const int64_t area = static_cast<int64_t>(m.size.w) * m.size.h;
        const int64_t da = area > want_area ? area - want_area : want_area - area;
        const int64_t mfps = static_cast<int64_t>(m.fps_num) * 1000 / m.fps_den;
        const int64_t df = mfps > want_mfps ? mfps - want_mfps : want_mfps - mfps;
        const int rank = WebcamSubtypeRank(m.fourcc);
        const bool better = best < 0 || da < best_area || (da == best_area && df < best_fps) ||
                            (da == best_area && df == best_fps && rank < best_rank);
        if (better) {
            best = static_cast<int>(i);
            best_area = da;
            best_fps = df;
            best_rank = rank;
        }
    }
    return best;
}

TimeUs WebcamFrameTimeUs(const MediaTimeSource& clock, bool has_device_ts, uint64_t device_ts_hns, bool* used_device) {
    const TimeUs now = clock.NowMediaUs();
    if (used_device) *used_device = false;
    if (has_device_ts && device_ts_hns != 0) {
        const TimeUs t = clock.QpcHnsToMediaUs(device_ts_hns);
        const TimeUs diff = t > now ? t - now : now - t;
        if (diff <= kWebcamMaxDeviceSkewUs) {
            if (used_device) *used_device = true;
            return t > now ? now : t;
        }
    }
    return now;
}

// --- Enumerate ----------------------------------------------------------------------------------------------------

HRESULT WebcamSource::Enumerate(std::vector<WebcamInfo>& out, bool with_modes) {
    out.clear();
    CT_RETURN_IF_FAILED(platform::MfRuntime::EnsureForProcess());
    ComPtr<IMFAttributes> attr;
    CT_RETURN_IF_FAILED(MFCreateAttributes(attr.GetAddressOf(), 1));
    CT_RETURN_IF_FAILED(attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    CT_RETURN_IF_FAILED(MFEnumDeviceSources(attr.Get(), &acts, &count));
    for (UINT32 i = 0; i < count; ++i) {
        WebcamInfo info;
        info.name = AllocatedString(acts[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        info.symbolic_link = AllocatedWString(acts[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        if (with_modes) {
            ComPtr<IMFMediaSource> source;
            HRESULT hr = acts[i]->ActivateObject(IID_PPV_ARGS(source.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                ComPtr<IMFSourceReader> reader;
                hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, reader.GetAddressOf());
                if (SUCCEEDED(hr)) ListNativeModes(reader.Get(), info.modes);
                reader.Reset();
                acts[i]->ShutdownObject();
            }
            if (FAILED(hr)) CT_LOG_WARN("webcam \"%s\": cannot open to list modes: %s", info.name.c_str(), HrToString(hr).c_str());
            for (const WebcamMode& m : info.modes) {
                const bool known = std::any_of(info.sizes.begin(), info.sizes.end(),
                                               [&](SizeI s) { return s.w == m.size.w && s.h == m.size.h; });
                if (!known) info.sizes.push_back(m.size);
            }
            std::stable_sort(info.sizes.begin(), info.sizes.end(), [](SizeI a, SizeI b) {
                return static_cast<int64_t>(a.w) * a.h > static_cast<int64_t>(b.w) * b.h;
            });
        }
        out.push_back(std::move(info));
    }
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    CT_LOG_INFO("webcam: %u device(s)", count);
    return S_OK;
}

// --- Init / Shutdown ----------------------------------------------------------------------------------------------

WebcamSource::~WebcamSource() { Shutdown(); }

HRESULT WebcamSource::Init(platform::D3DDevice& dev, const Config& cfg) {
    if (m_thread.IsRunning()) return E_UNEXPECTED;
    Shutdown();
    CT_RETURN_IF_FAILED(platform::MfRuntime::EnsureForProcess());
    CT_RETURN_IF_FAILED(dev.InitMfManager());

    std::vector<WebcamInfo> devices;
    CT_RETURN_IF_FAILED(Enumerate(devices, false));
    std::wstring link = cfg.symbolic_link;
    std::string name;
    if (link.empty()) {
        if (devices.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        link = devices[0].symbolic_link;
    }
    for (const WebcamInfo& d : devices)
        if (d.symbolic_link == link) name = d.name;
    if (name.empty()) {
        // The device may still open (symbolic links compare case-insensitively); keep going with a placeholder name.
        name = "webcam";
    }

    const int64_t t0 = Clock::QpcNow();
    ComPtr<IMFAttributes> src_attr;
    CT_RETURN_IF_FAILED(MFCreateAttributes(src_attr.GetAddressOf(), 2));
    CT_RETURN_IF_FAILED(src_attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    CT_RETURN_IF_FAILED(src_attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str()));
    ComPtr<IMFMediaSource> source;
    CT_RETURN_IF_FAILED(MFCreateDeviceSource(src_attr.Get(), source.GetAddressOf()));

    // From here on every failure shuts the device source down.
    auto fail = [&](HRESULT hr, const char* what) {
        LogHr(hr, what, __FILE__, __LINE__);
        source->Shutdown();
        return hr;
    };
    ComPtr<IMFAttributes> rd_attr;
    HRESULT hr = MFCreateAttributes(rd_attr.GetAddressOf(), 4);
    if (FAILED(hr)) return fail(hr, "MFCreateAttributes(reader)");
    rd_attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dev.MfManager());
    rd_attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    rd_attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    rd_attr->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), rd_attr.Get(), reader.GetAddressOf());
    if (FAILED(hr)) return fail(hr, "MFCreateSourceReaderFromMediaSource");
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr)) return fail(hr, "SetStreamSelection(video)");

    std::vector<WebcamMode> modes;
    ListNativeModes(reader.Get(), modes);
    const int pick = PickWebcamMode(modes, cfg.preferred_size, cfg.preferred_fps);
    if (pick < 0) return fail(MF_E_INVALIDMEDIATYPE, "PickWebcamMode (no usable native type)");
    const WebcamMode native = modes[static_cast<size_t>(pick)];

    ComPtr<IMFMediaType> native_type;
    hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), native.native_index,
                                    native_type.GetAddressOf());
    if (FAILED(hr)) return fail(hr, "GetNativeMediaType");
    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, native_type.Get());
    if (FAILED(hr)) return fail(hr, "SetCurrentMediaType(native)");

    ComPtr<IMFMediaType> nv12;
    hr = MFCreateMediaType(nv12.GetAddressOf());
    if (FAILED(hr)) return fail(hr, "MFCreateMediaType");
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    nv12->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(nv12.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(native.size.w), static_cast<UINT32>(native.size.h));
    MFSetAttributeRatio(nv12.Get(), MF_MT_FRAME_RATE, native.fps_num, native.fps_den);
    hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nv12.Get());
    if (FAILED(hr)) return fail(hr, "SetCurrentMediaType(NV12)");

    ComPtr<IMFMediaType> current;
    hr = reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), current.GetAddressOf());
    if (FAILED(hr)) return fail(hr, "GetCurrentMediaType");
    WebcamMode out_mode;
    if (!ModeFromType(current.Get(), 0, out_mode) || out_mode.fourcc != MFVideoFormat_NV12.Data1 || out_mode.size.IsEmpty() ||
        (out_mode.size.w & 1) || (out_mode.size.h & 1))
        return fail(MF_E_INVALIDMEDIATYPE, "reader output is not an even-sized NV12 type");
    if (out_mode.fps_num == 0) {
        out_mode.fps_num = native.fps_num;
        out_mode.fps_den = native.fps_den;
    }

    hr = InitWithReader(dev, std::make_unique<MfWebcamReader>(std::move(reader), std::move(source)), out_mode.size,
                        out_mode.fps_num, out_mode.fps_den, cfg.pool_size);
    if (FAILED(hr)) return hr;   // the reader (and with it the device source) was released by InitWithReader
    m_native = native;
    m_name = name;
    CT_LOG_INFO("webcam \"%s\": native %dx%d %.2f fps %s (type %u of %zu) -> NV12 %dx%d %.2f fps; init %.0f ms",
                name.c_str(), native.size.w, native.size.h, native.Fps(), FourCCString(native.fourcc).c_str(),
                native.native_index, modes.size(), m_size.w, m_size.h, out_mode.Fps(),
                static_cast<double>(Clock::QpcToUs(Clock::QpcNow(), t0)) / 1000.0);
    return S_OK;
}

HRESULT WebcamSource::InitWithReader(platform::D3DDevice& dev, std::unique_ptr<WebcamReader> reader, SizeI size,
                                     uint32_t fps_num, uint32_t fps_den, uint32_t pool_size) {
    if (m_thread.IsRunning()) return E_UNEXPECTED;
    if (!reader || size.IsEmpty() || (size.w & 1) || (size.h & 1) || pool_size == 0) return E_INVALIDARG;
    Shutdown();
    m_dev = &dev;
    m_reader = std::move(reader);
    m_size = size;
    m_fps_num = fps_num ? fps_num : 30;
    m_fps_den = fps_den ? fps_den : 1;
    m_native = WebcamMode{};
    m_name = "synthetic";
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
    if (dev.SupportsNv12RenderTarget()) bind |= D3D11_BIND_RENDER_TARGET;
    const D3D11_TEXTURE2D_DESC desc = Nv12Desc(size, bind);
    HRESULT hr = m_pool.Init(dev, desc, pool_size, "webcam_nv12");
    if (FAILED(hr)) {
        LogHr(hr, "webcam NV12 pool", __FILE__, __LINE__);
        Shutdown();
        return hr;
    }
    // Preview copy + plane views. Missing NV12 SRV support only disables the preview.
    const D3D11_TEXTURE2D_DESC pdesc = Nv12Desc(size, D3D11_BIND_SHADER_RESOURCE);
    hr = dev.Device()->CreateTexture2D(&pdesc, nullptr, m_preview.GetAddressOf());
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        v.Format = DXGI_FORMAT_R8_UNORM;
        hr = dev.Device()->CreateShaderResourceView(m_preview.Get(), &v, m_preview_y.GetAddressOf());
        if (SUCCEEDED(hr)) {
            v.Format = DXGI_FORMAT_R8G8_UNORM;
            hr = dev.Device()->CreateShaderResourceView(m_preview.Get(), &v, m_preview_uv.GetAddressOf());
        }
    }
    if (FAILED(hr)) {
        CT_LOG_WARN("webcam preview unavailable: %s", HrToString(hr).c_str());
        m_preview_y.Reset();
        m_preview_uv.Reset();
        m_preview.Reset();
    }
    D3D11_TEXTURE2D_DESC sdesc = Nv12Desc(size, 0);
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = dev.Device()->CreateTexture2D(&sdesc, nullptr, m_upload.GetAddressOf());
    if (FAILED(hr)) CT_LOG_WARN("webcam upload texture unavailable (memory buffers will be dropped): %s", HrToString(hr).c_str());
    return S_OK;
}

void WebcamSource::Shutdown() {
    Stop();
    m_reader.reset();
    m_preview_y.Reset();
    m_preview_uv.Reset();
    m_preview.Reset();
    m_upload.Reset();
    m_pool.Shutdown();
    m_preview_serial.store(0, std::memory_order_release);
    m_dev = nullptr;
}

int WebcamSource::ActualFps() const { return static_cast<int>((m_fps_num + m_fps_den / 2) / m_fps_den); }

bool WebcamSource::LatestPreviewPlanes(ID3D11ShaderResourceView** y, ID3D11ShaderResourceView** uv, SizeI* size) const {
    if (!m_preview_y || m_preview_serial.load(std::memory_order_acquire) == 0) return false;
    if (y) *y = m_preview_y.Get();
    if (uv) *uv = m_preview_uv.Get();
    if (size) *size = m_size;
    return true;
}

// --- Thread -------------------------------------------------------------------------------------------------------

bool WebcamSource::Start(const MediaTimeSource& clock, FrameSink& sink) {
    if (!m_reader || !m_dev || m_thread.IsRunning()) return false;
    m_thread.Join();   // a thread that ended on an error
    m_clock = &clock;
    m_sink = &sink;
    for (std::atomic<uint64_t>* c : {&m_stats.samples, &m_stats.frames, &m_stats.dropped_pool_empty, &m_stats.dropped_sink,
                                     &m_stats.dropped_paused, &m_stats.dropped_late, &m_stats.dropped_bad_buffer,
                                     &m_stats.stream_ticks, &m_stats.device_timestamps, &m_stats.fallback_timestamps,
                                     &m_stats.cpu_uploads})
        c->store(0);
    m_stats.first_frame_us.store(-1);
    m_stats.last_frame_us.store(-1);
    m_last_t_us = -1;
    m_stop.store(false, std::memory_order_release);
    Thread::Options opt;
    opt.name = "ct-webcam";
    opt.priority = THREAD_PRIORITY_ABOVE_NORMAL;
    opt.com_mta = true;
    return m_thread.Start(opt, [this] { Run(); });
}

void WebcamSource::Stop() {
    if (!m_thread.Handle()) return;
    m_stop.store(true, std::memory_order_release);
    const int64_t t0 = Clock::QpcNow();
    bool shut = false;
    for (;;) {
        // A Flush that lands between the stop check and ReadSample is lost, so repeat it until the thread exits.
        if (m_reader && !shut) m_reader->Flush();
        if (m_thread.Join(100)) break;
        const TimeUs waited = Clock::QpcToUs(Clock::QpcNow(), t0);
        if (waited > 3'000'000 && !shut && m_reader) {
            CT_LOG_WARN("ct-webcam ignores Flush for 3 s; shutting the device source down");
            m_reader->Shutdown();
            shut = true;
        }
        if (waited > 5'000'000) CT_FATAL("ct-webcam did not stop within 5 s");
    }
    if (shut) m_reader.reset();   // the device is gone; Init again to restart
    const WebcamStats& s = m_stats;
    CT_LOG_INFO("webcam stopped: %llu frames, %llu samples, drops pool %llu sink %llu paused %llu late %llu buffer %llu, "
                "ticks %llu, device ts %llu, fallback ts %llu",
                (unsigned long long)s.frames.load(), (unsigned long long)s.samples.load(),
                (unsigned long long)s.dropped_pool_empty.load(), (unsigned long long)s.dropped_sink.load(),
                (unsigned long long)s.dropped_paused.load(), (unsigned long long)s.dropped_late.load(),
                (unsigned long long)s.dropped_bad_buffer.load(), (unsigned long long)s.stream_ticks.load(),
                (unsigned long long)s.device_timestamps.load(), (unsigned long long)s.fallback_timestamps.load());
}

void WebcamSource::Run() {
    while (!m_stop.load(std::memory_order_acquire)) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = m_reader->ReadSample(&flags, &ts, sample.GetAddressOf());
        if (m_stop.load(std::memory_order_acquire)) break;
        if (SUCCEEDED(hr) && (flags & MF_SOURCE_READERF_ERROR)) hr = E_FAIL;
        if (SUCCEEDED(hr) && (flags & MF_SOURCE_READERF_ENDOFSTREAM)) hr = MF_E_END_OF_STREAM;
        if (FAILED(hr)) {
            CT_LOG_ERROR("webcam ReadSample failed (flags 0x%lx): %s", flags, HrToString(hr).c_str());
            platform::D3DDevice::Lock lock(*m_dev);
            m_sink->OnError(hr);
            return;
        }
        if (flags & MF_SOURCE_READERF_STREAMTICK) m_stats.stream_ticks.fetch_add(1, std::memory_order_relaxed);
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) CT_LOG_WARN("webcam output type changed mid-stream");
        if (!sample) continue;   // flush, stream tick
        m_stats.samples.fetch_add(1, std::memory_order_relaxed);
        HandleSample(sample.Get());
    }
}

void WebcamSource::HandleSample(IMFSample* sample) {
    if (m_paused.load(std::memory_order_acquire)) {
        m_stats.dropped_paused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    UINT64 dts = 0;
    const bool has_dts = SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &dts));
    bool used_device = false;
    const TimeUs t = WebcamFrameTimeUs(*m_clock, has_dts, dts, &used_device);
    if (t <= m_last_t_us) {
        m_stats.dropped_late.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint32_t index = 0;
    ID3D11Texture2D* dst = m_pool.Acquire(index);
    if (!dst) {
        m_stats.dropped_pool_empty.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    platform::D3DDevice::Lock lock(*m_dev);
    if (!CopyIntoPool(sample, dst)) {
        m_pool.Release(index);
        m_stats.dropped_bad_buffer.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (m_preview) {
        m_dev->Context()->CopyResource(m_preview.Get(), dst);
        m_preview_serial.fetch_add(1, std::memory_order_acq_rel);
    }
    if (!m_sink->OnFrame(dst, index, t)) {
        m_pool.Release(index);
        m_stats.dropped_sink.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_last_t_us = t;
    m_stats.frames.fetch_add(1, std::memory_order_relaxed);
    (used_device ? m_stats.device_timestamps : m_stats.fallback_timestamps).fetch_add(1, std::memory_order_relaxed);
    int64_t expected = -1;
    m_stats.first_frame_us.compare_exchange_strong(expected, t);
    m_stats.last_frame_us.store(t, std::memory_order_relaxed);
}

bool WebcamSource::CopyIntoPool(IMFSample* sample, ID3D11Texture2D* dst) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, buffer.GetAddressOf()))) return false;
    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(buffer.As(&dxgi))) {
        ComPtr<ID3D11Texture2D> src;
        UINT sub = 0;
        if (FAILED(dxgi->GetResource(IID_PPV_ARGS(src.GetAddressOf()))) || FAILED(dxgi->GetSubresourceIndex(&sub))) return false;
        D3D11_TEXTURE2D_DESC sd{};
        src->GetDesc(&sd);
        if (sd.Format != DXGI_FORMAT_NV12) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) CT_LOG_ERROR("webcam delivered DXGI format %d, expected NV12", static_cast<int>(sd.Format));
            return false;
        }
        D3D11_BOX box{};
        box.right = std::min<UINT>(sd.Width, static_cast<UINT>(m_size.w)) & ~1u;
        box.bottom = std::min<UINT>(sd.Height, static_cast<UINT>(m_size.h)) & ~1u;
        box.back = 1;
        m_dev->Context()->CopySubresourceRegion(dst, 0, 0, 0, 0, src.Get(), sub, &box);
        return true;
    }
    // Memory buffer (software pipeline): NV12 rows, the UV plane right after `height` luma rows.
    if (!m_upload) return false;
    m_stats.cpu_uploads.fetch_add(1, std::memory_order_relaxed);
    ComPtr<IMF2DBuffer> b2;
    if (SUCCEEDED(buffer.As(&b2))) {
        BYTE* p = nullptr;
        LONG pitch = 0;
        if (FAILED(b2->Lock2D(&p, &pitch))) return false;
        HRESULT hr = E_FAIL;
        if (pitch >= m_size.w) hr = internal::UploadNv12(*m_dev, m_upload.Get(), dst, p, static_cast<size_t>(pitch), m_size);
        b2->Unlock2D();
        return SUCCEEDED(hr);
    }
    BYTE* p = nullptr;
    DWORD max_len = 0, cur_len = 0;
    if (FAILED(buffer->Lock(&p, &max_len, &cur_len))) return false;
    HRESULT hr = E_FAIL;
    if (cur_len >= static_cast<DWORD>(m_size.w) * m_size.h * 3 / 2)
        hr = internal::UploadNv12(*m_dev, m_upload.Get(), dst, p, static_cast<size_t>(m_size.w), m_size);
    buffer->Unlock();
    return SUCCEEDED(hr);
}

} // namespace ct::capture
