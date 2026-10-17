#pragma once
// tests/common/nv12_readback.h
// Reads an NV12 texture back to the CPU through a staging texture (tests only; blocks on the GPU).

#include <ct/platform/d3d_device.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace ct::test {

struct Nv12Image {
    int width = 0, height = 0;
    std::vector<uint8_t> y;    // width * height
    std::vector<uint8_t> uv;   // width * height / 2 (interleaved U,V)
};

inline bool ReadbackNv12(platform::D3DDevice& dev, ID3D11Texture2D* tex, Nv12Image& out) {
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12) return false;
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev.Device()->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) return false;
    platform::D3DDevice::Lock lock(dev);
    dev.Context()->CopyResource(staging.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(dev.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.width = static_cast<int>(desc.Width);
    out.height = static_cast<int>(desc.Height);
    out.y.resize(static_cast<size_t>(out.width) * out.height);
    out.uv.resize(static_cast<size_t>(out.width) * out.height / 2);
    const uint8_t* src = static_cast<const uint8_t*>(m.pData);
    for (int r = 0; r < out.height; ++r) std::memcpy(&out.y[static_cast<size_t>(r) * out.width], src + static_cast<size_t>(r) * m.RowPitch, out.width);
    const uint8_t* uv = src + static_cast<size_t>(m.RowPitch) * desc.Height;
    for (int r = 0; r < out.height / 2; ++r)
        std::memcpy(&out.uv[static_cast<size_t>(r) * out.width], uv + static_cast<size_t>(r) * m.RowPitch, out.width);
    dev.Context()->Unmap(staging.Get(), 0);
    return true;
}

} // namespace ct::test
