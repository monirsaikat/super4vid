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

bool DecodeLumaMeans(const std::string& path, int max_frames, std::vector<double>& means, IMFDXGIDeviceManager* hw) {
    means.clear();
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(attr.GetAddressOf(), 2))) { std::printf("decode fail line %d\n", __LINE__); return false; }
    if (hw) attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, hw);
    ComPtr<IMFSourceReader> r;
    const std::wstring w = Utf8ToWide(path);
    if (FAILED(MFCreateSourceReaderFromURL(w.c_str(), attr.Get(), r.GetAddressOf()))) { std::printf("decode fail line %d\n", __LINE__); return false; }
    const DWORD vs = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    r->SetStreamSelection(vs, TRUE);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(t.GetAddressOf());
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(r->SetCurrentMediaType(vs, nullptr, t.Get()))) { std::printf("decode fail line %d\n", __LINE__); return false; }
    ComPtr<IMFMediaType> cur;
    r->GetCurrentMediaType(vs, cur.GetAddressOf());
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &width, &height);
    const LONG stride = static_cast<LONG>(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, width));
    while (static_cast<int>(means.size()) < max_frames) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample(vs, 0, nullptr, &flags, &ts, s.GetAddressOf()))) { std::printf("decode fail line %d\n", __LINE__); return false; }
        if (ct::test::Verbose()) std::printf("    decode: flags 0x%lx sample %p t %lld\n", flags, static_cast<void*>(s.Get()), ts);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        ComPtr<IMFMediaBuffer> b;
        if (FAILED(s->ConvertToContiguousBuffer(b.GetAddressOf()))) { std::printf("decode fail line %d\n", __LINE__); return false; }
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(b->Lock(&p, nullptr, &len))) { std::printf("decode fail line %d\n", __LINE__); return false; }
        const LONG pitch = stride > 0 ? stride : static_cast<LONG>(width);
        double sum = 0;
        uint64_t n = 0;
        for (UINT32 yy = 0; yy < height; yy += 4) {
            if (static_cast<DWORD>((yy + 1) * pitch) > len) break;
            const BYTE* row = p + static_cast<size_t>(yy) * pitch;
            for (UINT32 xx = 0; xx < width; xx += 4) {
                sum += row[xx];
                ++n;
            }
        }
        b->Unlock();
        means.push_back(n ? sum / static_cast<double>(n) : 0.0);
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
