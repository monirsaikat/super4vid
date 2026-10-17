#include "media_test_util.h"

#include <ct/test/ct_test.h>

#include <ct/core/strings.h>
#include <ct/platform/win.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace ct::media_test {

bool MakeNv12Pool(platform::D3DDevice& dev, platform::TexturePool& pool, int w, int h, uint32_t count, bool uav, bool* got_uav) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = static_cast<UINT>(w);
    d.Height = static_cast<UINT>(h);
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (got_uav) *got_uav = false;
    if (uav) {
        D3D11_TEXTURE2D_DESC du = d;
        du.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        if (SUCCEEDED(pool.Init(dev, du, count, "test.nv12"))) {
            if (got_uav) *got_uav = true;
            return true;
        }
    }
    return SUCCEEDED(pool.Init(dev, d, count, "test.nv12"));
}

bool FillNv12(platform::D3DDevice& dev, ID3D11Texture2D* tex, uint8_t y, uint8_t u, uint8_t v) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = DXGI_FORMAT_R8_UNORM;
    ComPtr<ID3D11RenderTargetView> ry, ruv;
    if (FAILED(dev.Device()->CreateRenderTargetView(tex, &rd, ry.GetAddressOf()))) return false;
    rd.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(dev.Device()->CreateRenderTargetView(tex, &rd, ruv.GetAddressOf()))) return false;
    const float cy[4] = {y / 255.f, 0, 0, 1};
    const float cuv[4] = {u / 255.f, v / 255.f, 0, 1};
    platform::D3DDevice::Lock lock(dev);
    dev.Context()->ClearRenderTargetView(ry.Get(), cy);
    dev.Context()->ClearRenderTargetView(ruv.Get(), cuv);
    return true;
}

namespace {
bool DecodeLumaImpl(const std::string& path, int max_frames, IMFDXGIDeviceManager* hw, LumaFn fn, void* user) {
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(attr.GetAddressOf(), 2))) return false;
    if (hw) attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, hw);
    ComPtr<IMFSourceReader> r;
    const std::wstring w = Utf8ToWide(path);
    if (FAILED(MFCreateSourceReaderFromURL(w.c_str(), attr.Get(), r.GetAddressOf()))) return false;
    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    r->SetStreamSelection(vs, TRUE);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(t.GetAddressOf());
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(r->SetCurrentMediaType(vs, nullptr, t.Get()))) return false;
    ComPtr<IMFMediaType> cur;
    r->GetCurrentMediaType(vs, cur.GetAddressOf());
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &width, &height);
    const LONG stride = static_cast<LONG>(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, width));
    const LONG pitch = stride > 0 ? stride : static_cast<LONG>(width);
    for (int frame = 0; frame < max_frames;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample(vs, 0, nullptr, &flags, &ts, s.GetAddressOf()))) return false;
        if (ct::test::Verbose()) std::printf("    decode: flags 0x%lx sample %p t %lld\n", flags, static_cast<void*>(s.Get()), ts);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        ComPtr<IMFMediaBuffer> b;
        if (FAILED(s->ConvertToContiguousBuffer(b.GetAddressOf()))) return false;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(b->Lock(&p, nullptr, &len))) return false;
        const bool more = len >= static_cast<DWORD>(pitch) * height &&
                          fn(user, frame, p, static_cast<int>(pitch), static_cast<int>(width), static_cast<int>(height));
        b->Unlock();
        ++frame;
        if (!more) break;
    }
    return true;
}
} // namespace

bool DecodeLuma(const std::string& path, int max_frames, LumaFn fn, void* user) {
    return DecodeLumaImpl(path, max_frames, nullptr, fn, user);
}

bool DecodeLumaMeans(const std::string& path, int max_frames, std::vector<double>& means, IMFDXGIDeviceManager* hw) {
    means.clear();
    return DecodeLumaImpl(path, max_frames, hw, [](void* u, int, const uint8_t* y, int pitch, int w, int h) {
        double sum = 0;
        uint64_t n = 0;
        for (int yy = 0; yy < h; yy += 4)
            for (int xx = 0; xx < w; xx += 4) {
                sum += y[static_cast<size_t>(yy) * pitch + xx];
                ++n;
            }
        static_cast<std::vector<double>*>(u)->push_back(n ? sum / static_cast<double>(n) : 0.0);
        return true;
    }, &means);
}

bool DecodeAudio(const std::string& path, int audio_track, std::vector<float>& pcm, int& channels, int64_t& first_us) {
    pcm.clear();
    channels = 0;
    first_us = -1;
    ComPtr<IMFSourceReader> r;
    const std::wstring w = Utf8ToWide(path);
    if (FAILED(MFCreateSourceReaderFromURL(w.c_str(), nullptr, r.GetAddressOf()))) return false;
    // The MPEG-4 source lists tracks in reverse file order: collect the audio streams and reverse them.
    std::vector<DWORD> audio;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(r->GetNativeMediaType(i, 0, t.GetAddressOf()))) break;
        GUID major{};
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio) audio.insert(audio.begin(), i);
    }
    if (audio_track < 0 || audio_track >= static_cast<int>(audio.size())) return false;
    const DWORD s = audio[static_cast<size_t>(audio_track)];
    r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    r->SetStreamSelection(s, TRUE);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(t.GetAddressOf());
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(r->SetCurrentMediaType(s, nullptr, t.Get()))) return false;
    ComPtr<IMFMediaType> cur;
    r->GetCurrentMediaType(s, cur.GetAddressOf());
    channels = static_cast<int>(MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> smp;
        if (FAILED(r->ReadSample(s, 0, nullptr, &flags, &ts, smp.GetAddressOf()))) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!smp) continue;
        if (first_us < 0) first_us = ts / 10;
        ComPtr<IMFMediaBuffer> b;
        if (FAILED(smp->ConvertToContiguousBuffer(b.GetAddressOf()))) return false;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(b->Lock(&p, nullptr, &len))) return false;
        const auto* f = reinterpret_cast<const float*>(p);
        pcm.insert(pcm.end(), f, f + len / sizeof(float));
        b->Unlock();
    }
    return true;
}

std::string TempPath(const char* name) {
    // Next to the test exe (the build tree), not %TEMP%: recordings are large and the system drive may be full.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    dir += L"media_test_tmp";
    CreateDirectoryW(dir.c_str(), nullptr);
    return WideToUtf8(dir) + "\\" + name;
}

uint64_t FileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(Utf8ToWide(path).c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

void Sine(float* out, uint32_t frames, double hz, float amp, int64_t start_frame) {
    constexpr double kPi = 3.14159265358979323846;
    for (uint32_t i = 0; i < frames; ++i) {
        const double ph = 2.0 * kPi * hz * static_cast<double>(start_frame + i) / 48000.0;
        const float v = amp * static_cast<float>(std::sin(ph));
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
}

} // namespace ct::media_test
