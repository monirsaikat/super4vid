#pragma once
// mf_startup_internal.h (private to ct_media)
// Minimal process-wide MFStartup guard for the P06 decoders. P04 owns the real lifetime object
// (media::MfRuntime::Ensure, refcounted); this file exists only because P04 and P06 were built in parallel.
// After the merge, replace EnsureMfStarted() with media::MfRuntime::Ensure() and delete this file (DECISIONS.md P06 6.1).

#include <ct/platform/win.h>

#include <mfapi.h>

namespace ct::media::detail {

// MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET) once per process; never shut down (process exit cleans up).
// Thread-safe (function-local static). Returns the HRESULT of the one MFStartup call.
inline HRESULT EnsureMfStarted() {
    static const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    return hr;
}

} // namespace ct::media::detail
