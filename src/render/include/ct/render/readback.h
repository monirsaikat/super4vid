#pragma once
// ct/render/readback.h
// GPU texture -> CPU bytes through a staging texture: tests (goldens), PNG export, thumbnails. Not for per-frame use.
// Thread model: any thread; takes D3DDevice::Lock itself (the lock is recursive, so callers may already hold it).

#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>

#include <cstdint>
#include <vector>

namespace ct::render {

struct Readback {
    // Copies a 2D texture (B8G8R8A8 or R8G8B8A8, mip 0) to CPU memory as tightly packed BGRA8 rows (R8G8B8A8 sources are
    // swizzled). Staging texture + Map; takes D3DDevice::Lock.
    static HRESULT ToBgra8(platform::D3DDevice&, ID3D11Texture2D* src, std::vector<uint8_t>& out, SizeI& size);
    // Same, as RGBA8 (what PNG writers and the golden tests want).
    static HRESULT ToRgba8(platform::D3DDevice&, ID3D11Texture2D* src, std::vector<uint8_t>& out, SizeI& size);
};

} // namespace ct::render
