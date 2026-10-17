#include <ct/recorder/keys.h>
#include <ct/test/ct_test.h>

#include <cstring>
#include <string>

using namespace ct::recorder;

CT_TEST(keys_is_shortcut_table) {
    struct Row { uint16_t vk; uint8_t mods; bool expect; };
    const Row rows[] = {
        {'S', keys::kCtrl, true},                     // Ctrl+S
        {'S', keys::kCtrl | keys::kShift, true},      // Ctrl+Shift+S
        {VK_TAB, keys::kAlt, true},                   // Alt+Tab
        {'D', keys::kWin, true},                      // Win+D
        {VK_F4, keys::kAlt, true},
        {VK_F5, 0, true},                             // F-keys alone
        {VK_F1, 0, true},
        {VK_F24, 0, true},
        {VK_ESCAPE, 0, true},
        {'A', 0, false},                              // typing
        {'A', keys::kShift, false},                   // capital letter is typing
        {VK_SPACE, 0, false},
        {VK_RETURN, 0, false},
        {VK_BACK, keys::kShift, false},
        {VK_LEFT, 0, false},
        {VK_LCONTROL, keys::kCtrl, false},            // modifiers alone are never shortcuts
        {VK_LSHIFT, keys::kCtrl, false},
        {VK_RMENU, keys::kCtrl | keys::kAlt, false},
        {VK_LWIN, 0, false},
        {0, keys::kCtrl, false},
    };
    for (const Row& r : rows) {
        if (keys::IsShortcut(r.vk, r.mods) != r.expect) {
            std::printf("  vk 0x%02X mods %u expected %d\n", r.vk, r.mods, r.expect ? 1 : 0);
            CT_FAIL("IsShortcut mismatch");
        }
    }
}

CT_TEST(keys_labels_us_layout) {
    char out[32];
    keys::LabelFor(VK_RETURN, 0x1C, false, out);
    CT_CHECK_EQ(std::string(out), std::string("Enter"));
    keys::LabelFor(VK_RETURN, 0x1C, true, out);
    CT_CHECK_EQ(std::string(out), std::string("Num Enter"));
    keys::LabelFor(VK_SPACE, 0x39, false, out);
    CT_CHECK_EQ(std::string(out), std::string("Space"));
    keys::LabelFor('A', 0x1E, false, out);
    CT_CHECK_EQ(std::string(out), std::string("A"));
    keys::LabelFor('7', 0x08, false, out);
    CT_CHECK_EQ(std::string(out), std::string("7"));
    keys::LabelFor(VK_LCONTROL, 0x1D, false, out);
    CT_CHECK_EQ(std::string(out), std::string("Ctrl"));
    keys::LabelFor(VK_RSHIFT, 0x36, false, out);
    CT_CHECK_EQ(std::string(out), std::string("Shift"));
    keys::LabelFor(VK_F11, 0x57, false, out);
    CT_CHECK_EQ(std::string(out), std::string("F11"));
    keys::LabelFor(VK_ESCAPE, 0x01, false, out);
    CT_CHECK_EQ(std::string(out), std::string("Esc"));
    keys::LabelFor(VK_NUMPAD4, 0x4B, false, out);
    CT_CHECK_EQ(std::string(out), std::string("Num 4"));
    // OEM keys come from the layout; whatever it is, it is a short non-empty UTF-8 label.
    keys::LabelFor(VK_OEM_1, 0x27, false, out);
    CT_CHECK(out[0] != 0 && std::strlen(out) < 32);
    keys::LabelFor(VK_OEM_1, 0, false, out);   // injected input without a scan code
    CT_CHECK(out[0] != 0);
}

CT_TEST(keys_char_for_us_layout) {
    HKL us = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
    if (!us) CT_SKIP("US layout not available");
    BYTE state[256] = {};
    CT_CHECK_EQ(keys::CharFor('A', 0x1E, state, us), static_cast<uint32_t>('a'));
    state[VK_SHIFT] = 0x80;
    CT_CHECK_EQ(keys::CharFor('A', 0x1E, state, us), static_cast<uint32_t>('A'));
    CT_CHECK_EQ(keys::CharFor('1', 0x02, state, us), static_cast<uint32_t>('!'));
    state[VK_SHIFT] = 0;
    state[VK_CAPITAL] = 0x01;
    CT_CHECK_EQ(keys::CharFor('A', 0x1E, state, us), static_cast<uint32_t>('A'));
    state[VK_CAPITAL] = 0;
    CT_CHECK_EQ(keys::CharFor(VK_RETURN, 0x1C, state, us), 0u);   // control characters are not text
    CT_CHECK_EQ(keys::CharFor(VK_BACK, 0x0E, state, us), 0u);
    CT_CHECK_EQ(keys::CharFor(VK_F5, 0x3F, state, us), 0u);
    CT_CHECK_EQ(keys::CharFor(VK_SPACE, 0x39, state, us), static_cast<uint32_t>(' '));
}
