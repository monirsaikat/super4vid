#pragma once
// ct/capture/frame_sink.h
// Where ScreenCapturer delivers NV12 frames: ct_media::Mp4Writer in P04, capture_bench's sink in P02.
// Thread model: OnFrame/OnError are called on ct-capture with D3DDevice::Lock held; they must not block (push to a
// queue and return). Ownership: when OnFrame returns true the sink owns pool_index and must call
// ScreenCapturer::Nv12Pool().Release(pool_index) once the texture is no longer needed (from one consumer thread).

#include <ct/core/types.h>
#include <ct/platform/win.h>

#include <d3d11.h>

#include <cstdint>

namespace ct::capture {

struct FrameSink {
    // false = sink rejected (full); the capturer releases the texture and counts a drop.
    virtual bool OnFrame(ID3D11Texture2D* nv12, uint32_t pool_index, TimeUs t_us) = 0;
    // A fatal capture error ended the session's capture thread (device removed, duplication failure that is not
    // access-lost). Called once, then the thread exits; the owner should finalize and call Stop().
    virtual void OnError(HRESULT hr) { (void)hr; }
    virtual ~FrameSink() = default;
};

} // namespace ct::capture
