// keymap_input.h — applies the user's keymap to key messages (app_main.cpp
// WndProcImpl). A key the user assigned is handed on as the action's default
// key with the default's modifiers reported by GetKeyState, so every existing
// handler (HandleKeyDown, ExplorerShortcut, the window switcher, the overview)
// keeps working unchanged.
#pragma once

#include "keymap.h"

namespace pulse::app {

struct KeyRoute {
    enum Kind : uint8_t { Pass, Swallow, Rewrite } kind = Pass;
    UINT msg = 0;       // Rewrite: WM_KEYDOWN or WM_SYSKEYDOWN, as the default key arrives
    WPARAM vk = 0;
    LPARAM lparam = 0;  // scan code of the default key (the switcher checks 0x29)
    uint8_t mods = 0;   // modifiers to report while it is handled
    KeyAction action = KeyAction::Count;
};

KeyRoute RouteKey(const Keymap& keymap, KeyChord pressed, LPARAM lparam);
UINT KeyMessageFor(KeyChord chord) noexcept;
LPARAM KeyMessageLParam(KeyChord chord, LPARAM original) noexcept;

// While alive, GetKeyState reports exactly `mods` for Ctrl, Alt and Shift on
// this thread. Restores each modifier afterwards unless a nested message
// loop already updated it (then the newer state stays).
class ModifierOverride {
public:
    explicit ModifierOverride(uint8_t mods) noexcept;
    ~ModifierOverride();
    ModifierOverride(const ModifierOverride&) = delete;
    ModifierOverride& operator=(const ModifierOverride&) = delete;

private:
    BYTE saved_[256]{};
    BYTE faked_[256]{};
    bool active_ = false;
};

} // namespace pulse::app
