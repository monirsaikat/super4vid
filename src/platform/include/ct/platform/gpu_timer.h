#pragma once
// ct/platform/gpu_timer.h
// GPU duration of a span of immediate-context work: a TIMESTAMP_DISJOINT query around two TIMESTAMP queries.
// Several query sets rotate so Resolve() never stalls: it reports the newest finished span (a few frames old).
// Thread model: one thread, and the caller holds D3DDevice::Lock around Begin/End/Resolve.

#include <ct/platform/d3d_device.h>

namespace ct::platform {

class GpuTimer {
public:
    HRESULT Init(D3DDevice&);
    void Begin(ID3D11DeviceContext*);
    void End(ID3D11DeviceContext*);
    // Non-blocking (D3D11_ASYNC_GETDATA_DONOTFLUSH). True with ms set when an older span finished and was not
    // disjoint; false when nothing new is ready.
    bool Resolve(ID3D11DeviceContext*, double& ms);
    void Shutdown();

private:
    static constexpr int kSets = 4;
    struct Set {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    Set m_sets[kSets];
    int m_write = 0;      // set used by the next Begin
    int m_read = 0;       // oldest pending set
    bool m_open = false;  // between Begin and End
};

} // namespace ct::platform
