#pragma once
// webcam_internal.h (ct_capture private)
// Helpers shared by webcam_source.cpp and webcam_synthetic.cpp.

#include <ct/core/types.h>
#include <ct/platform/d3d_device.h>

#include <cstddef>
#include <cstdint>

namespace ct::capture::internal {

// Solid NV12 fill (tests and the synthetic source build on it).
void FillNv12(uint8_t* y, size_t y_pitch, uint8_t* uv, size_t uv_pitch, SizeI size, uint8_t y_val, uint8_t u_val,
              uint8_t v_val);

// Copies NV12 rows (`src_pitch` apart, UV plane after size.h luma rows) through a CPU-write staging NV12 texture into
// `dst`. The caller holds D3DDevice::Lock.
HRESULT UploadNv12(platform::D3DDevice& dev, ID3D11Texture2D* staging, ID3D11Texture2D* dst, const uint8_t* src,
                   size_t src_pitch, SizeI size);

} // namespace ct::capture::internal
