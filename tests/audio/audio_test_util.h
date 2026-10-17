#pragma once
// Shared helpers for the audio tests: a scoped MTA apartment (the WASAPI classes need COM on the calling thread).

#include <ct/platform/win.h>

#include <objbase.h>

namespace ct::audio::test {

class ComScope {
public:
    ComScope() : m_ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope() {
        if (m_ok) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool m_ok;
};

} // namespace ct::audio::test
