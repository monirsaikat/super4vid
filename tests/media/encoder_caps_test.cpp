// ct/media/mf_runtime.h, ct/media/encoder_caps.h
#include <ct/media/encoder_caps.h>
#include <ct/media/mf_runtime.h>
#include <ct/core/strings.h>
#include <ct/test/ct_test.h>

#include <cstdio>

using namespace ct;
using namespace ct::media;

CT_TEST_DEVICE(mf_runtime_refcount) {
    const int base = MfRuntime::RefCount();
    CT_CHECK_HR(MfRuntime::Ensure());
    CT_CHECK_HR(MfRuntime::Ensure());
    CT_CHECK_EQ(MfRuntime::RefCount(), base + 2);
    MfRuntime::Shutdown();
    {
        MfScope scope;
        CT_CHECK_HR(scope.Result());
        CT_CHECK_EQ(MfRuntime::RefCount(), base + 2);
    }
    MfRuntime::Shutdown();
    CT_CHECK_EQ(MfRuntime::RefCount(), base);
}

CT_TEST_DEVICE(mf_runtime_process_reference_taken_once) {
    // The decoders and the webcam take one never-released reference (EnsureForProcess), however often they call it.
    const int base = MfRuntime::RefCount();
    CT_CHECK_HR(MfRuntime::EnsureForProcess());
    const int after_first = MfRuntime::RefCount();
    CT_CHECK(after_first == base || after_first == base + 1);   // + 1 unless an earlier test already took it
    CT_CHECK_HR(MfRuntime::EnsureForProcess());
    CT_CHECK_HR(platform::MfRuntime::EnsureForProcess());
    CT_CHECK_EQ(MfRuntime::RefCount(), after_first);
    CT_CHECK(after_first >= 1);
}

CT_TEST_DEVICE(encoder_caps_enumerate_h264) {
    const std::vector<EncoderInfo> list = EncoderCaps::Enumerate(VideoCodec::H264);
    bool software = false, hw_first = true, seen_sw = false;
    for (const EncoderInfo& e : list) {
        CT_CHECK(!e.name.empty());
        CT_CHECK(e.codec == VideoCodec::H264);
        if (!e.hardware) seen_sw = true;
        if (e.hardware && seen_sw) hw_first = false;
        if (!e.hardware && ToLowerAscii(e.name).find("microsoft") != std::string::npos) software = true;
        if (!e.hardware && e.name == "H264 Encoder MFT") software = true;
    }
    CT_CHECK(software);   // the Microsoft software encoder is always there
    CT_CHECK(hw_first);
    const std::string report = EncoderCaps::Report();
    std::printf("%s", report.c_str());
    CT_CHECK(report.find("h264:") != std::string::npos);
    // Cached: a second call returns the same list.
    CT_CHECK_EQ(EncoderCaps::Enumerate(VideoCodec::H264).size(), list.size());
    for (const EncoderInfo& e : list)
        if (!e.clsid.empty()) CT_CHECK_EQ(EncoderCaps::NameForClsid(e.clsid), e.name);
    CT_CHECK(EncoderCaps::NameForClsid("{00000000-0000-0000-0000-000000000000}").empty());
    std::printf("  hardware H.264: %s, hardware HEVC: %s\n", EncoderCaps::HasHardware(VideoCodec::H264) ? "yes" : "no",
                EncoderCaps::HasHardware(VideoCodec::Hevc) ? "yes" : "no");
}
