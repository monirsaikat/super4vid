#pragma once
// ct/platform/mf_runtime.h
// Process-wide Media Foundation lifetime: a refcounted MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET) / MFShutdown.
// Lives in ct_platform (not ct_media) so ct_capture (webcam) can use it: ct_media depends on ct_capture.
// ct/media/mf_runtime.h forwards it as media::MfRuntime / media::MfScope.
// Thread model: Ensure/Shutdown are thread-safe (one lock). Every Ensure() that succeeded is paired with one
// Shutdown(); MFShutdown runs when the count returns to zero. The app calls Ensure() lazily on first use (startup
// budget); Mp4Writer, EncoderCaps and Probe call it themselves.
// EnsureForProcess() is for code without a natural Shutdown point (decoders, MediaInfo, the webcam): its first
// successful call takes one reference that is never released, so MF stays up until process exit.

typedef long HRESULT;   // same type as <winnt.h>

namespace ct::platform {

struct MfRuntime {
    static HRESULT Ensure();
    static void Shutdown();
    static HRESULT EnsureForProcess();
    static int RefCount();   // for tests
};

// RAII pairing of Ensure/Shutdown.
class MfScope {
public:
    MfScope();
    ~MfScope();
    MfScope(const MfScope&) = delete;
    MfScope& operator=(const MfScope&) = delete;
    HRESULT Result() const { return m_hr; }

private:
    HRESULT m_hr;
};

} // namespace ct::platform
