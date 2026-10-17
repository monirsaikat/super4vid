#define CT_LOG_CATEGORY "media"
#include <ct/media/video_decoder.h>

#include "mf_util_internal.h"

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/core/strings.h>
#include <ct/core/time.h>

#include <mferror.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace ct::media {

namespace {
constexpr TimeUs kStaleAfterSeekUs = 2'000'000;
} // namespace


VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() { Shutdown(); }

RectI VideoDecoder::ComputeApertureBox(SizeI frame, RectI aperture) {
    RectI full{0, 0, frame.w & ~1, frame.h & ~1};
    if (aperture.IsEmpty()) return full;
    RectI r = aperture.Intersect(RectI{0, 0, frame.w, frame.h});
    if (r.IsEmpty()) return full;
    const int32_t x0 = (r.x + 1) & ~1;   // an odd origin moves inward so the box stays inside the aperture
    const int32_t y0 = (r.y + 1) & ~1;
    RectI out{x0, y0, (r.Right() - x0) & ~1, (r.Bottom() - y0) & ~1};
    return out.IsEmpty() ? full : out;
}

TimeUs VideoDecoder::FrameDurationUs() const {
    return m_fps_num > 0 ? static_cast<TimeUs>(1'000'000ll * m_fps_den / m_fps_num) : 33'333;
}

HRESULT VideoDecoder::Init(platform::D3DDevice& dev, const Config& cfg) {
    Shutdown();
    m_dev = &dev;
    m_cfg = cfg;
    CT_RETURN_IF_FAILED(detail::EnsureMfStarted());

    ComPtr<IMFAttributes> attrs;
    CT_RETURN_IF_FAILED(MFCreateAttributes(attrs.GetAddressOf(), 4));
    if (cfg.allow_hardware) {
        CT_RETURN_IF_FAILED(dev.InitMfManager());
        attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dev.MfManager());
        // No MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING: the decoder already outputs NV12 and the extra video
        // processor stage tripled the CPU cost per frame (DECISIONS.md P06 6.2).
    } else {
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // software colour conversion to NV12
    }
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, cfg.allow_hardware ? TRUE : FALSE);
    attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, cfg.allow_hardware ? FALSE : TRUE);
    // Low-latency decoding emits each frame as soon as it is decoded instead of filling the reorder buffer first
    // (seek-to-keyframe 8 ms -> 2-4 ms). Hardware only: the software decoder then restarts its output timestamps at 0
    // after a seek (observed on Win11 26100).
    if (cfg.allow_hardware) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

    const std::wstring wpath = Utf8ToWide(cfg.path);
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), m_reader.GetAddressOf());
    if (FAILED(hr)) {
        CT_LOG_WARN("VideoDecoder: cannot open %s: %s", cfg.path.c_str(), HrToString(hr).c_str());
        return hr;
    }
    m_stream = cfg.stream_index >= 0 ? static_cast<DWORD>(cfg.stream_index) : detail::kFirstVideo;
    CT_RETURN_IF_FAILED(m_reader->SetStreamSelection(detail::kAllStreams, FALSE));
    hr = m_reader->SetStreamSelection(m_stream, TRUE);
    if (FAILED(hr)) {
        CT_LOG_WARN("VideoDecoder: %s has no video stream %d: %s", cfg.path.c_str(), cfg.stream_index, HrToString(hr).c_str());
        return hr;
    }
    // Native type: rotation and frame rate as the container states them.
    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(m_reader->GetNativeMediaType(m_stream, 0, native.GetAddressOf()))) {
        const detail::VideoTypeInfo vt = detail::ReadVideoType(native.Get());
        m_rotation = vt.rotation_deg;
        if (vt.fps_num > 0) {
            m_fps_num = vt.fps_num;
            m_fps_den = vt.fps_den;
        }
    }
    ComPtr<IMFMediaType> want;
    CT_RETURN_IF_FAILED(MFCreateMediaType(want.GetAddressOf()));
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    hr = m_reader->SetCurrentMediaType(m_stream, nullptr, want.Get());
    if (FAILED(hr)) {
        CT_LOG_WARN("VideoDecoder: NV12 output rejected for %s: %s", cfg.path.c_str(), HrToString(hr).c_str());
        return hr;
    }
    CT_RETURN_IF_FAILED(ApplyCurrentType());
    m_duration_us = detail::ReaderDurationUs(m_reader.Get());
    CT_LOG_INFO("VideoDecoder: %s %dx%d (frame %dx%d) %d/%d fps rot %d, duration %.3f s, hw allowed %d", cfg.path.c_str(),
                m_size.w, m_size.h, m_frame.w, m_frame.h, m_fps_num, m_fps_den, m_rotation, m_duration_us / 1e6,
                cfg.allow_hardware ? 1 : 0);
    return S_OK;
}

void VideoDecoder::Shutdown() {
    m_reader.Reset();
    m_y_srv.clear();
    m_uv_srv.clear();
    m_pool.Shutdown();
    m_scratch.clear();
    m_scratch.shrink_to_fit();
    m_size = {};
    m_frame = {};
    m_eos = false;
    m_seek_target = -1;
    m_first_after_seek = true;
    m_keyframes.clear();
    m_frames_copied = m_frames_skipped = 0;
}

HRESULT VideoDecoder::ApplyCurrentType() {
    ComPtr<IMFMediaType> type;
    CT_RETURN_IF_FAILED(m_reader->GetCurrentMediaType(m_stream, type.GetAddressOf()));
    const detail::VideoTypeInfo vt = detail::ReadVideoType(type.Get());
    if (vt.frame.IsEmpty()) return MF_E_INVALIDMEDIATYPE;
    m_frame = vt.frame;
    m_stride = vt.stride;
    if (vt.fps_num > 0) {
        m_fps_num = vt.fps_num;
        m_fps_den = vt.fps_den;
    }
    m_box = ComputeApertureBox(vt.frame, vt.aperture);
    const SizeI size{m_box.w, m_box.h};
    if (size.w != m_size.w || size.h != m_size.h) CT_RETURN_IF_FAILED(CreatePool(size));
    return S_OK;
}

HRESULT VideoDecoder::CreatePool(SizeI size) {
    if (m_pool.InUse() > 0) CT_LOG_WARN("VideoDecoder: pool recreated with %u frames still held", m_pool.InUse());
    m_y_srv.clear();
    m_uv_srv.clear();
    m_pool.Shutdown();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(size.w);
    desc.Height = static_cast<UINT>(size.h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint32_t count = std::clamp<uint32_t>(m_cfg.pool_size, 1u, 64u);
    CT_RETURN_IF_FAILED(m_pool.Init(*m_dev, desc, count, "decode-nv12"));
    m_y_srv.resize(count);
    m_uv_srv.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8_UNORM;
        CT_RETURN_IF_FAILED(m_dev->Device()->CreateShaderResourceView(m_pool.At(i), &sd, m_y_srv[i].GetAddressOf()));
        sd.Format = DXGI_FORMAT_R8G8_UNORM;
        CT_RETURN_IF_FAILED(m_dev->Device()->CreateShaderResourceView(m_pool.At(i), &sd, m_uv_srv[i].GetAddressOf()));
    }
    m_size = size;
    m_scratch.assign(static_cast<size_t>(size.w) * size.h * 3 / 2, 0);
    return S_OK;
}

HRESULT VideoDecoder::Seek(TimeUs t_us) {
    if (!m_reader) return E_ILLEGAL_METHOD_CALL;
    CT_RETURN_IF_FAILED(detail::SetReaderPosition(m_reader.Get(), t_us));
    m_seek_target = t_us < 0 ? 0 : t_us;
    m_first_after_seek = true;
    m_eos = false;
    return S_OK;
}

HRESULT VideoDecoder::ReadFrame(DecodedFrame& out) { return ReadFrame(out, INT64_MIN); }

HRESULT VideoDecoder::ReadFrame(DecodedFrame& out, TimeUs skip_before_us) { return ReadFrame(out, skip_before_us, nullptr, 0); }

HRESULT VideoDecoder::ReadFrame(DecodedFrame& out, TimeUs skip_before_us, const std::atomic<uint64_t>* abort_gen, uint64_t gen) {
    if (!m_reader) return E_ILLEGAL_METHOD_CALL;
    if (m_eos) return S_FALSE;
    uint32_t index = 0;
    if (!m_pool.Acquire(index)) return E_PENDING;   // nothing read: the caller releases frames and retries
    for (;;) {
        DWORD actual = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = m_reader->ReadSample(m_stream, 0, &actual, &flags, &ts, sample.GetAddressOf());
        if (FAILED(hr)) {
            m_pool.Release(index);
            CT_LOG_WARN("VideoDecoder: ReadSample failed: %s", HrToString(hr).c_str());
            return hr;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            m_pool.Release(index);
            CT_RETURN_IF_FAILED(ApplyCurrentType());
            if (!m_pool.Acquire(index)) return E_PENDING;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            m_eos = true;
            m_pool.Release(index);
            return S_FALSE;
        }
        if (!sample) continue;   // MF_SOURCE_READERF_STREAMTICK / gap
        const TimeUs pts = ts / 10;
        LONGLONG dur_hns = 0;
        const TimeUs dur = SUCCEEDED(sample->GetSampleDuration(&dur_hns)) && dur_hns > 0 ? dur_hns / 10 : FrameDurationUs();
        if (m_seek_target >= 0) {
            if (pts < m_seek_target - kStaleAfterSeekUs) continue;   // a frame from before the seek (some sources)
            m_seek_target = -1;
        }
        // Decoders flag every output sample as a clean point, so only seek landings identify keyframes.
        if (m_first_after_seek) NoteKeyframe(pts);
        m_first_after_seek = false;
        if (pts + dur <= skip_before_us) {
            ++m_frames_skipped;
            if (abort_gen && abort_gen->load(std::memory_order_relaxed) != gen) {
                m_pool.Release(index);
                return E_ABORT;
            }
            continue;
        }
        const HRESULT copy_hr = CopyToPool(sample.Get(), index);
        if (FAILED(copy_hr)) {
            m_pool.Release(index);
            return copy_hr;
        }
        ++m_frames_copied;
        out.nv12 = m_pool.At(index);
        out.pool_index = index;
        out.pts_us = pts;
        out.duration_us = dur;
        out.size = m_size;
        return S_OK;
    }
}

HRESULT VideoDecoder::CopyToPool(IMFSample* sample, uint32_t index) {
    ComPtr<IMFMediaBuffer> buffer;
    CT_RETURN_IF_FAILED(sample->GetBufferByIndex(0, buffer.GetAddressOf()));
    ID3D11Texture2D* dst = m_pool.At(index);
    ComPtr<IMFDXGIBuffer> dxgi;
    if (SUCCEEDED(buffer.As(&dxgi))) {
        ComPtr<ID3D11Texture2D> tex;
        CT_RETURN_IF_FAILED(dxgi->GetResource(IID_PPV_ARGS(tex.GetAddressOf())));
        UINT sub = 0;
        CT_RETURN_IF_FAILED(dxgi->GetSubresourceIndex(&sub));
        D3D11_BOX box{static_cast<UINT>(m_box.x), static_cast<UINT>(m_box.y), 0, static_cast<UINT>(m_box.Right()),
                      static_cast<UINT>(m_box.Bottom()), 1};
        platform::D3DDevice::Lock lock(*m_dev);
        m_dev->Context()->CopySubresourceRegion(dst, 0, 0, 0, 0, tex.Get(), sub, &box);
        m_hardware = true;
        return S_OK;
    }
    // System memory (software decoder).
    m_hardware = false;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer2> b2;
    ComPtr<IMF2DBuffer> b1;
    bool locked_2d2 = false, locked_2d = false, locked_1d = false;
    if (SUCCEEDED(buffer.As(&b2))) {
        BYTE* start = nullptr;
        DWORD len = 0;
        CT_RETURN_IF_FAILED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len));
        locked_2d2 = true;
    } else if (SUCCEEDED(buffer.As(&b1))) {
        CT_RETURN_IF_FAILED(b1->Lock2D(&scan0, &pitch));
        locked_2d = true;
    } else {
        DWORD max_len = 0, cur_len = 0;
        CT_RETURN_IF_FAILED(buffer->Lock(&scan0, &max_len, &cur_len));
        pitch = m_stride > 0 ? m_stride : m_frame.w;
        locked_1d = true;
    }
    // Rebuild the aperture as one contiguous NV12 image (Y rows then UV rows, pitch = width): the UV plane of the
    // buffer starts after the *padded* frame height, which UpdateSubresource cannot express.
    const int w = m_size.w, h = m_size.h;
    const BYTE* uv_base = scan0 + static_cast<size_t>(pitch) * m_frame.h;
    uint8_t* d = m_scratch.data();
    for (int y = 0; y < h; ++y) std::memcpy(d + static_cast<size_t>(y) * w, scan0 + static_cast<size_t>(m_box.y + y) * pitch + m_box.x, w);
    uint8_t* duv = d + static_cast<size_t>(w) * h;
    for (int y = 0; y < h / 2; ++y)
        std::memcpy(duv + static_cast<size_t>(y) * w, uv_base + static_cast<size_t>(m_box.y / 2 + y) * pitch + m_box.x, w);
    if (locked_2d2) b2->Unlock2D();
    if (locked_2d) b1->Unlock2D();
    if (locked_1d) buffer->Unlock();
    platform::D3DDevice::Lock lock(*m_dev);
    m_dev->Context()->UpdateSubresource(dst, 0, nullptr, m_scratch.data(), static_cast<UINT>(w), 0);
    return S_OK;
}

void VideoDecoder::ReleaseFrame(uint32_t pool_index) { m_pool.Release(pool_index); }

void VideoDecoder::NoteKeyframe(TimeUs pts) {
    auto it = std::lower_bound(m_keyframes.begin(), m_keyframes.end(), pts);
    if (it != m_keyframes.end() && *it == pts) return;
    if (m_keyframes.size() >= 8192) return;   // ~4.5 h of 2 s GOPs; beyond that the heuristic just seeks more
    m_keyframes.insert(it, pts);
}

ID3D11ShaderResourceView* VideoDecoder::YSrv(uint32_t i) const { return i < m_y_srv.size() ? m_y_srv[i].Get() : nullptr; }
ID3D11ShaderResourceView* VideoDecoder::UvSrv(uint32_t i) const { return i < m_uv_srv.size() ? m_uv_srv[i].Get() : nullptr; }

} // namespace ct::media
