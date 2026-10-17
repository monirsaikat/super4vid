#include <ct/audio/devices.h>

#include "audio_test_util.h"

#include <ct/test/ct_test.h>

#include <cstdio>

using ct::audio::AudioDevices;

CT_TEST_DEVICE(devices_enumerate) {
    ct::audio::test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    const auto render = devices.List(false);
    if (render.empty()) CT_SKIP("no active render endpoint");
    const auto capture = devices.List(true);
    int defaults = 0;
    for (const auto& d : render) {
        CT_CHECK(!d.id.empty());
        CT_CHECK(!d.name.empty());
        defaults += d.is_default ? 1 : 0;
        if (ct::test::Verbose()) std::printf("  render  %s%s\n", d.name.c_str(), d.is_default ? " (default)" : "");
    }
    for (const auto& d : capture) {
        CT_CHECK(!d.id.empty());
        if (ct::test::Verbose()) std::printf("  capture %s%s\n", d.name.c_str(), d.is_default ? " (default)" : "");
    }
    CT_CHECK_EQ(defaults, 1);
    const std::wstring def = devices.DefaultId(false);
    CT_CHECK(!def.empty());
    CT_CHECK(!devices.NameOf(def).empty());
    CT_CHECK(devices.NameOf(L"{not-a-device}").empty());
    devices.Shutdown();
    CT_CHECK(devices.List(false).empty());   // after Shutdown nothing is enumerated
}

namespace {
struct CountingListener : AudioDevices::Listener {
    void OnDefaultDeviceChanged(bool, const std::wstring&) override {}
    void OnDeviceRemoved(const std::wstring&) override {}
};
} // namespace

CT_TEST_DEVICE(devices_listeners_register_and_remove) {
    ct::audio::test::ComScope com;
    AudioDevices devices;
    CT_REQUIRE(SUCCEEDED(devices.Init()));
    CountingListener a, b;
    devices.SetListener(&a);
    devices.AddListener(&b);
    devices.AddListener(&b);   // duplicate add is ignored
    devices.RemoveListener(&b);
    devices.SetListener(nullptr);
    devices.Shutdown();
    devices.Shutdown();        // idempotent
    CT_CHECK(SUCCEEDED(devices.Init()));   // can be re-initialized
}
