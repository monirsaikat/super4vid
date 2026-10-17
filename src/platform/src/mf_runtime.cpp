#define CT_LOG_CATEGORY "media"
#include <ct/platform/mf_runtime.h>

#include <ct/core/error.h>
#include <ct/core/log.h>
#include <ct/platform/win.h>

#include <mfapi.h>

namespace ct::platform {

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
int g_refs = 0;
bool g_process_ref = false;   // EnsureForProcess() holds its one reference

HRESULT EnsureLocked() {
    HRESULT hr = S_OK;
    if (g_refs == 0) {
        hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(hr)) CT_LOG_ERROR("MFStartup failed: %s", HrToString(hr).c_str());
    }
    if (SUCCEEDED(hr)) ++g_refs;
    return hr;
}
} // namespace

HRESULT MfRuntime::Ensure() {
    AcquireSRWLockExclusive(&g_lock);
    const HRESULT hr = EnsureLocked();
    ReleaseSRWLockExclusive(&g_lock);
    return hr;
}

HRESULT MfRuntime::EnsureForProcess() {
    AcquireSRWLockExclusive(&g_lock);
    HRESULT hr = S_OK;
    if (!g_process_ref) {
        hr = EnsureLocked();
        g_process_ref = SUCCEEDED(hr);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return hr;
}

void MfRuntime::Shutdown() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_refs > 0 && --g_refs == 0) CT_LOG_IF_FAILED(MFShutdown());
    ReleaseSRWLockExclusive(&g_lock);
}

int MfRuntime::RefCount() {
    AcquireSRWLockShared(&g_lock);
    const int n = g_refs;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

MfScope::MfScope() : m_hr(MfRuntime::Ensure()) {}

MfScope::~MfScope() {
    if (SUCCEEDED(m_hr)) MfRuntime::Shutdown();
}

} // namespace ct::platform
