#pragma once
// ct/media/mf_runtime.h
// Process-wide Media Foundation lifetime: a refcounted MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET) / MFShutdown.
// Thread model: Ensure/Shutdown are thread-safe (one lock). Every Ensure() that succeeded is paired with one
// Shutdown(); MFShutdown runs when the count returns to zero. The app calls Ensure() lazily on first use (startup
// budget); Mp4Writer, EncoderCaps and Probe call it themselves.

typedef long HRESULT;   // same type as <winnt.h>

namespace ct::media {

struct MfRuntime {
    static HRESULT Ensure();
    static void Shutdown();
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

} // namespace ct::media
