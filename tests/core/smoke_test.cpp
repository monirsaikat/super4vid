// Smoke tests for the P00 ct_core stub: geometry, colors, HRESULT names.
#include <ct/core/error.h>
#include <ct/core/types.h>
#include <ct/test/ct_test.h>

#include <string>

CT_TEST(rect_intersect) {
    const ct::RectI a{0, 0, 100, 50};
    const ct::RectI b{60, 20, 100, 100};
    const ct::RectI i = a.Intersect(b);
    CT_CHECK_EQ(i.x, 60);
    CT_CHECK_EQ(i.y, 20);
    CT_CHECK_EQ(i.w, 40);
    CT_CHECK_EQ(i.h, 30);
    CT_CHECK_EQ(i.Right(), 100);
    CT_CHECK_EQ(i.Bottom(), 50);

    // Touching edges do not overlap: Right() is exclusive.
    CT_CHECK(a.Intersect(ct::RectI{100, 0, 10, 10}).IsEmpty());
    CT_CHECK(a.Intersect(ct::RectI{-50, -50, 10, 10}).IsEmpty());

    CT_CHECK(a.Contains(ct::PointI{0, 0}));
    CT_CHECK(a.Contains(ct::PointI{99, 49}));
    CT_CHECK(!a.Contains(ct::PointI{100, 49}));
    CT_CHECK(!a.Contains(ct::PointI{-1, 0}));
}

CT_TEST(color_hex_roundtrip) {
    const ct::ColorF accent = ct::ColorF::FromHex("#4F8CFF");
    CT_CHECK_NEAR(accent.r, 0x4F / 255.0, 1e-6);
    CT_CHECK_NEAR(accent.g, 0x8C / 255.0, 1e-6);
    CT_CHECK_NEAR(accent.b, 1.0, 1e-6);
    CT_CHECK_NEAR(accent.a, 1.0, 1e-6);
    CT_CHECK_EQ(accent.ToHex(), std::string("#4F8CFF"));

    const ct::ColorF soft = ct::ColorF::FromHex("4f8cff2e");
    CT_CHECK_NEAR(soft.a, 0x2E / 255.0, 1e-6);
    CT_CHECK_EQ(soft.ToHex(), std::string("#4F8CFF2E"));

    for (const char* hex : {"#000000", "#FFFFFF", "#0F1115", "#FF4D4F80", "#12345678"})
        CT_CHECK_EQ(ct::ColorF::FromHex(hex).ToHex(), std::string(hex));

    // Malformed input yields opaque black.
    CT_CHECK_EQ(ct::ColorF::FromHex("#12345").ToHex(), std::string("#000000"));
    CT_CHECK_EQ(ct::ColorF::FromHex("#GG0000").ToHex(), std::string("#000000"));
}

CT_TEST(hr_to_string_names_dxgi) {
    const std::string s = ct::HrToString(static_cast<long>(0x887A0026));
    CT_CHECK(s.find("ACCESS_LOST") != std::string::npos);
    CT_CHECK(s.find("0x887A0026") == 0);

    // Unknown names fall back to the system message text.
    const std::string denied = ct::HrToString(static_cast<long>(0x80070002));   // ERROR_FILE_NOT_FOUND
    CT_CHECK(denied.find("0x80070002 (") == 0);
    CT_CHECK(denied.find("unknown") == std::string::npos);

    ct::Err err;
    CT_CHECK(err.Ok());
    err.Set("disk full", static_cast<long>(0x80070070));
    CT_CHECK(!err.Ok());
    CT_CHECK_EQ(err.message, std::string("disk full"));
}
