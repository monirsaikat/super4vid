#pragma once
// ct/platform/image_io.h
// Image files through WIC. Minimal in P02 (PNG writing for capture_bench --dump-frame and test diagnostics);
// P06 adds decoding and thumbnails.
// Thread model: any thread; the caller's thread must have COM initialized (either apartment).

#include <cstdint>

namespace ct::platform {

struct ImageIo {
    // rgba: 8-bit R,G,B,A rows, `stride` bytes apart. Writes a 32-bpp PNG (alpha kept). Returns false on any failure
    // (logged with the HRESULT).
    static bool WritePngRgba(const char* path_utf8, const uint8_t* rgba, int width, int height, int stride);
};

} // namespace ct::platform
