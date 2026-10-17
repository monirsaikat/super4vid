#pragma once
// ct/platform/hotkeys.h
// System-wide hotkeys (RegisterHotKey with MOD_NOREPEAT) bound to one window; WM_HOTKEY arrives in that window's
// procedure with wParam = id. Registered on the recorder bar's hidden message window so they work while it is hidden.
// Thread model: RegisterHotKey is per thread: call everything on the thread that owns the window (the UI thread).
// Lifetime: the destructor unregisters everything still registered.

#include <ct/platform/win.h>

#include <string>
#include <vector>

namespace ct::platform {

class HotkeyManager {
public:
    HotkeyManager() = default;
    ~HotkeyManager();
    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    // modifiers: MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN (MOD_NOREPEAT is added). Re-registering an id on the
    // same window replaces the old binding. On failure *conflict_error gets a user-presentable message such as
    // "Ctrl+Shift+F9 is already used by another app" (ERROR_HOTKEY_ALREADY_REGISTERED) and the app keeps running.
    bool Register(HWND hwnd, int id, UINT modifiers, UINT vk, std::string* conflict_error);
    bool Unregister(HWND hwnd, int id);
    void UnregisterAll();
    bool IsRegistered(HWND hwnd, int id) const;
    size_t Count() const { return m_entries.size(); }

    // "Ctrl+Alt+Shift+Win+Key" for messages and menus (same canonical text as the settings file).
    static std::string Describe(UINT modifiers, UINT vk);

private:
    struct Entry { HWND hwnd; int id; };
    std::vector<Entry> m_entries;
};

} // namespace ct::platform
