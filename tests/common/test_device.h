#pragma once
// tests/common/test_device.h
// One D3D11 device per test process (created on first use, debug layer in Debug builds), shared by device tests.

#include <ct/platform/d3d_device.h>

namespace ct::test {

// nullptr when no device can be created (CI without GPU): the caller CT_SKIPs.
inline platform::D3DDevice* SharedDevice() {
    static platform::D3DDevice* dev = [] {
        auto* d = new platform::D3DDevice();   // intentionally leaked: lives until process exit
#ifdef CT_DEBUG
        const bool debug_layer = true;
#else
        const bool debug_layer = false;
#endif
        if (FAILED(d->Init(debug_layer)) && (!debug_layer || FAILED(d->Init(false)))) {
            delete d;
            return static_cast<platform::D3DDevice*>(nullptr);
        }
        return d;
    }();
    return dev;
}

} // namespace ct::test

#define CT_REQUIRE_DEVICE(var)                                   \
    ::ct::platform::D3DDevice* var = ::ct::test::SharedDevice(); \
    if (!var) {                                                  \
        CT_SKIP("no D3D11 device");                              \
    }
