#include <ct/recorder/keys.h>

#include <ct/core/strings.h>
#include <ct/events/ctev.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ct::recorder::keys {

static_assert(kCtrl == events::kModCtrl && kShift == events::kModShift && kAlt == events::kModAlt &&
              kWin == events::kModWin);

bool IsModifierVk(uint16_t vk) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool IsShortcut(uint16_t vk, uint8_t modifiers) {
    if (vk == 0 || IsModifierVk(vk)) return false;
    if (vk >= VK_F1 && vk <= VK_F24) return true;
    if (vk == VK_ESCAPE) return true;
    return (modifiers & (kCtrl | kAlt | kWin)) != 0;
}

namespace {

const char* NamedKey(uint16_t vk, bool extended) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "Shift";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "Alt";
    case VK_LWIN: case VK_RWIN: return "Win";
    case VK_RETURN: return extended ? "Num Enter" : "Enter";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_ESCAPE: return "Esc";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PgUp";
    case VK_NEXT: return "PgDn";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_CAPITAL: return "Caps Lock";
    case VK_NUMLOCK: return "Num Lock";
    case VK_SCROLL: return "Scroll Lock";
    case VK_SNAPSHOT: return "PrtSc";
    case VK_PAUSE: return "Pause";
    case VK_APPS: return "Menu";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DECIMAL: return "Num .";
    case VK_DIVIDE: return "Num /";
    case VK_VOLUME_MUTE: return "Mute";
    case VK_VOLUME_DOWN: return "Volume Down";
    case VK_VOLUME_UP: return "Volume Up";
    case VK_MEDIA_PLAY_PAUSE: return "Play/Pause";
    case VK_MEDIA_NEXT_TRACK: return "Next Track";
    case VK_MEDIA_PREV_TRACK: return "Previous Track";
    case VK_MEDIA_STOP: return "Stop";
    default: return nullptr;
    }
}

void Copy(char out[32], const char* s) {
    std::snprintf(out, 32, "%s", s);
}

} // namespace

void LabelFor(uint16_t vk, uint16_t scancode, bool extended, char out[32]) {
    out[0] = 0;
    if (const char* named = NamedKey(vk, extended)) return Copy(out, named);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        out[0] = static_cast<char>(vk);
        out[1] = 0;
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        std::snprintf(out, 32, "F%d", vk - VK_F1 + 1);
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        std::snprintf(out, 32, "Num %d", vk - VK_NUMPAD0);
        return;
    }
    // Layout-specific keys (OEM punctuation, letters of other scripts): the name the layout gives the scan code.
    wchar_t name[64] = {};
    const LONG lparam = (static_cast<LONG>(scancode & 0xFF) << 16) | (extended ? (1L << 24) : 0);
    int n = scancode ? GetKeyNameTextW(lparam, name, 64) : 0;
    if (n <= 0) {
        // No scan code (injected input) or no name: ask the layout for the character.
        const UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFF;
        if (ch >= 0x21) {
            name[0] = static_cast<wchar_t>(ch);
            name[1] = 0;
            n = 1;
        }
    }
    if (n > 0) {
        const std::string utf8 = WideToUtf8(std::wstring_view(name, static_cast<size_t>(n)));
        events::CopyLabel(out, 32, utf8);
        return;
    }
    std::snprintf(out, 32, "VK %u", static_cast<unsigned>(vk));
}

uint32_t CharFor(uint16_t vk, uint16_t scancode, const BYTE keystate[256], HKL layout) {
    if (!layout) {
        const HWND fg = GetForegroundWindow();
        layout = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);
    }
    wchar_t buf[8] = {};
    const UINT kDoNotChangeState = 1u << 2;   // Windows 10 1607+: leaves the dead-key buffer of the thread alone
    const int n = ToUnicodeEx(vk, scancode, keystate, buf, 8, kDoNotChangeState, layout);
    if (n <= 0) return 0;   // dead key (< 0) or nothing
    uint32_t cp = buf[0];
    if (n >= 2 && cp >= 0xD800 && cp <= 0xDBFF && buf[1] >= 0xDC00 && buf[1] <= 0xDFFF)
        cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<uint32_t>(buf[1]) - 0xDC00);
    if (cp < 0x20 || cp == 0x7F) return 0;   // Enter, Tab, Backspace, Esc produce control characters
    return cp;
}

uint8_t AsyncModifiers() {
    uint8_t m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= kCtrl;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) m |= kShift;
    if (GetAsyncKeyState(VK_MENU) & 0x8000) m |= kAlt;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) m |= kWin;
    return m;
}

} // namespace ct::recorder::keys
