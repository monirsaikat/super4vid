#include <ct/platform/gpu_timer.h>

#include <ct/core/error.h>

namespace ct::platform {

HRESULT GpuTimer::Init(D3DDevice& dev) {
    ID3D11Device* device = dev.Device();
    if (!device) return E_UNEXPECTED;
    const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    const D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
    for (Set& s : m_sets) {
        CT_RETURN_IF_FAILED(device->CreateQuery(&disjoint, s.disjoint.ReleaseAndGetAddressOf()));
        CT_RETURN_IF_FAILED(device->CreateQuery(&stamp, s.begin.ReleaseAndGetAddressOf()));
        CT_RETURN_IF_FAILED(device->CreateQuery(&stamp, s.end.ReleaseAndGetAddressOf()));
        s.pending = false;
    }
    m_write = m_read = 0;
    m_open = false;
    return S_OK;
}

void GpuTimer::Begin(ID3D11DeviceContext* ctx) {
    if (m_open || !m_sets[0].disjoint) return;
    Set& s = m_sets[m_write];
    if (s.pending) return;   // every set still in flight: skip this span rather than stall
    ctx->Begin(s.disjoint.Get());
    ctx->End(s.begin.Get());
    m_open = true;
}

void GpuTimer::End(ID3D11DeviceContext* ctx) {
    if (!m_open) return;
    Set& s = m_sets[m_write];
    ctx->End(s.end.Get());
    ctx->End(s.disjoint.Get());
    s.pending = true;
    m_open = false;
    m_write = (m_write + 1) % kSets;
}

bool GpuTimer::Resolve(ID3D11DeviceContext* ctx, double& ms) {
    bool got = false;
    while (m_sets[m_read].pending) {
        Set& s = m_sets[m_read];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(s.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(s.begin.Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (ctx->GetData(s.end.Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        s.pending = false;
        m_read = (m_read + 1) % kSets;
        if (!dj.Disjoint && dj.Frequency != 0 && t1 >= t0) {
            ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
            got = true;
        }
    }
    return got;
}

void GpuTimer::Shutdown() {
    for (Set& s : m_sets) {
        s.disjoint.Reset();
        s.begin.Reset();
        s.end.Reset();
        s.pending = false;
    }
    m_open = false;
}

} // namespace ct::platform
