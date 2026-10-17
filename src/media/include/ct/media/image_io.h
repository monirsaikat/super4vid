#pragma once
// ct/media/image_io.h
// Still images through WIC: decode to premultiplied BGRA (CPU or an immutable D3D11 texture) and encode PNG/JPEG.
// (platform::ImageIo::WritePngRgba from P02 stays for RGBA test dumps; this is the media-facing API.)
// Thread model: any thread with COM initialized (either apartment). LoadToTexture uses only the device (free-threaded).

#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>

#include <wrl/client.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace ct::media {

struct ImageIo {
    // Decodes frame 0 and converts to 32bppPBGRA (B,G,R,A bytes, premultiplied), rows tightly packed (w * 4).
    static HRESULT LoadRgba(std::string_view path, std::vector<uint8_t>& rgba, SizeI&);
    // Same pixels as an immutable B8G8R8A8_UNORM texture with BIND_SHADER_RESOURCE.
    static HRESULT LoadToTexture(platform::D3DDevice&, std::string_view path, Microsoft::WRL::ComPtr<ID3D11Texture2D>&, SizeI&);
    // bgra: B,G,R,A bytes, `stride` bytes per row. PNG keeps alpha; JPEG drops it (quality 0..1).
    static HRESULT SavePng(std::string_view path, const uint8_t* bgra, int w, int h, int stride);
    static HRESULT SaveJpeg(std::string_view path, const uint8_t* bgra, int w, int h, int stride, float quality);
};

} // namespace ct::media
