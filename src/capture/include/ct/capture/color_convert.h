#pragma once
// ct/capture/color_convert.h
// BGRA8 / scRGB FP16 -> NV12 (BT.709 limited range) on the GPU: one compute dispatch writing both NV12 planes through
// UAVs (when supported), else two fullscreen pixel-shader passes into the luma and chroma plane views; rotation back to
// desktop orientation and optional HDR tone-mapping.
// Fallback (WARP or no NV12 render target): ID3D11VideoProcessor (no tone-map).
// Thread model: one owner thread; the caller holds D3DDevice::Lock around Convert. All pipeline objects are created
// in Init; Convert creates views only for textures it has not seen yet (a pool's textures, cached).
// Also the CPU references used by the tests: RgbToNv12Ref and the rotation uv mapping.

#include <ct/capture/display.h>
#include <ct/platform/d3d_device.h>

#include <cstdint>

namespace ct::capture {

class ColorConverter {
public:
    struct Config {
        SizeI src_size;            // source size (native panel orientation): the whole texture, or the ConvertRegion box
        DXGI_FORMAT src_format = DXGI_FORMAT_B8G8R8A8_UNORM;   // or R16G16B16A16_FLOAT (scRGB)
        Rotation rotation = Rotation::Identity;
        bool hdr_tonemap = true;   // FP16 source only: soft roll-off above SDR white
        float sdr_white_nits = 80.f;
        SizeI dst_size;            // == src_size after rotation, even
        bool force_video_processor = false;   // tests: exercise the fallback on hardware that has the shader path
        // One compute dispatch with UAVs on both NV12 planes instead of two pixel-shader passes, when the device has
        // typed UAV stores on NV12. dst textures then need D3D11_BIND_UNORDERED_ACCESS (see DstBindFlags()).
        bool allow_compute = true;
    };

    ColorConverter() = default;
    ~ColorConverter();
    ColorConverter(const ColorConverter&) = delete;
    ColorConverter& operator=(const ColorConverter&) = delete;

    HRESULT Init(platform::D3DDevice&, const Config&);
    // src: Config::src_format, bound as shader resource (shader path). dst_nv12: NV12, RENDER_TARGET (+SHADER_RESOURCE).
    HRESULT Convert(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D* dst_nv12);   // caller holds the device lock
    // Converts only `box` of a larger source (e.g. the duplicated desktop image, so no crop copy is needed). The box
    // size must equal Config::src_size. Bilinear chroma taps stay inside the box (2x2 block centres).
    HRESULT ConvertRegion(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, const D3D11_BOX& box, ID3D11Texture2D* dst_nv12);
    bool UsingVideoProcessorFallback() const { return m_use_vp; }
    bool UsingCompute() const;
    // Bind flags the NV12 destination textures must have for the chosen path (RT|SR, plus UAV for compute).
    UINT DstBindFlags() const;
    void Shutdown();

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_use_vp = false;
};

// Row-major 4x4 for HLSL `mul(float4(u, v, 0, 1), M)`: output uv -> source uv.
// R90: (u,v) -> (v, 1-u); R180: (1-u, 1-v); R270: (1-v, u).
void RotationUvMatrix(Rotation rotation, float out[16]);
void RotateUvRef(Rotation rotation, float u, float v, float& su, float& sv);

// CPU reference of the shader path (Identity rotation, SDR): bgra is width*height pixels (B,G,R,A bytes), tightly
// packed; y gets width*height bytes, uv gets width*height/2 bytes (interleaved U,V). Chroma is computed from the
// average RGB of each 2x2 block (what the linear sampler does). width and height must be even.
void RgbToNv12Ref(const uint8_t* bgra, int width, int height, uint8_t* y, uint8_t* uv);

} // namespace ct::capture
