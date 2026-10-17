#pragma once
// tests/common/rgba_readback.h
// Reads a 4-byte-per-pixel texture (RGBA8 / BGRA8) back to the CPU, rows tightly packed (tests only; blocks).

#include <ct/platform/d3d_device.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace ct::test {

inline bool ReadbackRgba8(platform::D3DDevice& dev, ID3D11Texture2D* tex, std::vector<uint8_t>& out, int& w, int& h) {
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
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
    w = static_cast<int>(desc.Width);
    h = static_cast<int>(desc.Height);
    out.resize(static_cast<size_t>(w) * h * 4);
    for (int r = 0; r < h; ++r)
        std::memcpy(&out[static_cast<size_t>(r) * w * 4], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(r) * m.RowPitch, static_cast<size_t>(w) * 4);
    dev.Context()->Unmap(staging.Get(), 0);
    return true;
}

// The texture behind a shader resource view (AddRef'd into `out`).
inline ID3D11Texture2D* TextureOf(ID3D11ShaderResourceView* srv, Microsoft::WRL::ComPtr<ID3D11Texture2D>& out) {
    if (!srv) return nullptr;
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    srv->GetResource(res.GetAddressOf());
    if (FAILED(res.As(&out))) return nullptr;
    return out.Get();
}

} // namespace ct::test
