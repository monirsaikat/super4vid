#pragma once
// ct/recorder/keys.h
// Keyboard helpers for the event recorder: which key presses count as shortcuts (the default privacy mode records
// only those), stable English labels for KEY records ("Ctrl", "A", "Enter"), and the character a key produces.
// Thread model: pure functions; LabelFor/CharFor call layout APIs that are safe on any thread. Never call CharFor
// inside a low-level hook (ToUnicodeEx is slow and touches keyboard state); the ct-events-io thread does.

#include <ct/platform/win.h>

#include <cstdint>

namespace ct::recorder::keys {

// Modifier bits as stored in KEY records (same values as ct::events::kMod*).
constexpr uint8_t kCtrl = 1u << 0;
constexpr uint8_t kShift = 1u << 1;
constexpr uint8_t kAlt = 1u << 2;
constexpr uint8_t kWin = 1u << 3;

bool IsModifierVk(uint16_t vk);   // Ctrl/Shift/Alt/Win in their generic and left/right forms

// A key-down worth recording in "shortcuts only" mode: Ctrl, Alt or Win held with a non-modifier key, or F1..F24,
// or Esc. Modifier keys themselves are never shortcuts (the stroke carries them as bits); Shift alone is typing.
bool IsShortcut(uint16_t vk, uint8_t modifiers);

// UTF-8, zero-terminated, at most 31 bytes. Fixed English names for named keys ("Ctrl", "Shift", "Alt", "Win",
// "Enter", "Space", "Tab", "Esc", "Backspace", "Delete", "Left", "F5", "Num 7", ...), the VK letter/digit for A-Z
// and 0-9, otherwise GetKeyNameTextW for the scan code (layout-specific OEM keys such as ";" or "Ö").
void LabelFor(uint16_t vk, uint16_t scancode, bool extended, char out[32]);

// The Unicode character the key produces with the given key state, via ToUnicodeEx with the "do not change keyboard
// state" flag (1 << 2) so dead keys keep working for the user. 0 for dead keys, control characters and keys that
// produce nothing. layout = nullptr uses the layout of the foreground window's thread.
uint32_t CharFor(uint16_t vk, uint16_t scancode, const BYTE keystate[256], HKL layout = nullptr);

// Modifier bits from the async key state (GetAsyncKeyState; valid inside a low-level hook).
uint8_t AsyncModifiers();

} // namespace ct::recorder::keys
