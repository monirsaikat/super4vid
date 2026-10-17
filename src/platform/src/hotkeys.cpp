#define CT_LOG_CATEGORY "platform"
#include <ct/platform/hotkeys.h>

#include <ct/core/config.h>
#include <ct/core/log.h>

#include <algorithm>

namespace ct::platform {

HotkeyManager::~HotkeyManager() { UnregisterAll(); }

std::string HotkeyManager::Describe(UINT modifiers, UINT vk) {
    return HotkeyToString(Hotkey{modifiers & (MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN), vk});
}

bool HotkeyManager::Register(HWND hwnd, int id, UINT modifiers, UINT vk, std::string* conflict_error) {
    if (IsRegistered(hwnd, id)) Unregister(hwnd, id);
    const UINT mods = (modifiers & (MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN)) | MOD_NOREPEAT;
    if (vk == 0) {
        if (conflict_error) *conflict_error = "No key set for this hotkey";
        return false;
    }
    if (!RegisterHotKey(hwnd, id, mods, vk)) {
        const DWORD e = GetLastError();
        const std::string name = Describe(modifiers, vk);
        CT_LOG_WARN("RegisterHotKey(%s) failed: %lu", name.c_str(), e);
        if (conflict_error) {
            if (e == ERROR_HOTKEY_ALREADY_REGISTERED)
                *conflict_error = name + " is already used by another app";
            else
                *conflict_error = name + " could not be registered (error " + std::to_string(e) + ")";
        }
        return false;
    }
    m_entries.push_back({hwnd, id});
    return true;
}

bool HotkeyManager::Unregister(HWND hwnd, int id) {
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.hwnd == hwnd && e.id == id; });
    if (it == m_entries.end()) return false;
    UnregisterHotKey(hwnd, id);
    m_entries.erase(it);
    return true;
}

void HotkeyManager::UnregisterAll() {
    for (const Entry& e : m_entries) UnregisterHotKey(e.hwnd, e.id);
    m_entries.clear();
}

bool HotkeyManager::IsRegistered(HWND hwnd, int id) const {
    return std::any_of(m_entries.begin(), m_entries.end(), [&](const Entry& e) { return e.hwnd == hwnd && e.id == id; });
}

} // namespace ct::platform
