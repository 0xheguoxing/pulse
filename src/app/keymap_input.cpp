#include "keymap_input.h"

#include <cstring>

namespace pulse::app {
namespace {

constexpr int kModifierKeys[] = {
    VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU, VK_SHIFT, VK_LSHIFT, VK_RSHIFT,
};

bool IsExtendedKey(UINT vk) noexcept {
    switch (vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_APPS: case VK_DIVIDE:
    case VK_NUMLOCK: case VK_SNAPSHOT:
        return true;
    default:
        return false;
    }
}

} // namespace

UINT KeyMessageFor(KeyChord chord) noexcept {
    // Alt without Ctrl arrives as a system key (Alt+Enter, Alt+D, Alt+arrows).
    return (chord.mods & kKeyAlt) && !(chord.mods & kKeyCtrl) ? WM_SYSKEYDOWN : WM_KEYDOWN;
}

LPARAM KeyMessageLParam(KeyChord chord, LPARAM original) noexcept {
    const UINT scan = chord.vk == SwitcherVirtualKey()
        ? 0x29u : MapVirtualKeyW(chord.vk, MAPVK_VK_TO_VSC);
    LPARAM lp = original & 0xFFFF;                     // repeat count
    if (lp == 0) lp = 1;
    lp |= static_cast<LPARAM>(scan & 0xFF) << 16;
    if (IsExtendedKey(chord.vk)) lp |= LPARAM{1} << 24;
    if (chord.mods & kKeyAlt) lp |= LPARAM{1} << 29;   // context code: Alt is down
    lp |= original & (LPARAM{1} << 30);                // previous key state (auto-repeat)
    return lp;
}

KeyRoute RouteKey(const Keymap& keymap, KeyChord pressed, LPARAM lparam) {
    KeyRoute route;
    const Keymap::Translation t = keymap.Translate(pressed);
    route.action = t.action;
    if (t.kind == Keymap::Translation::Swallow) {
        route.kind = KeyRoute::Swallow;
    } else if (t.kind == Keymap::Translation::Rewrite) {
        route.kind = KeyRoute::Rewrite;
        route.msg = KeyMessageFor(t.target);
        route.vk = t.target.vk;
        route.lparam = KeyMessageLParam(t.target, lparam);
        route.mods = t.target.mods;
    }
    return route;
}

ModifierOverride::ModifierOverride(uint8_t mods) noexcept {
    if (!GetKeyboardState(saved_)) return;
    memcpy(faked_, saved_, sizeof(faked_));
    auto set = [&](int vk, bool down) {
        faked_[vk] = static_cast<BYTE>((faked_[vk] & 0x01) | (down ? 0x80 : 0x00));  // keep the toggle bit
    };
    const bool ctrl = (mods & kKeyCtrl) != 0, alt = (mods & kKeyAlt) != 0, shift = (mods & kKeyShift) != 0;
    set(VK_CONTROL, ctrl);
    set(VK_LCONTROL, ctrl);
    set(VK_RCONTROL, false);
    set(VK_MENU, alt);
    set(VK_LMENU, alt);
    set(VK_RMENU, false);
    set(VK_SHIFT, shift);
    set(VK_LSHIFT, shift);
    set(VK_RSHIFT, false);
    active_ = SetKeyboardState(faked_) != FALSE;
}

ModifierOverride::~ModifierOverride() {
    if (!active_) return;
    BYTE now[256]{};
    if (!GetKeyboardState(now)) return;
    for (const int vk : kModifierKeys)
        if (now[vk] == faked_[vk]) now[vk] = saved_[vk];
    SetKeyboardState(now);
}

} // namespace pulse::app
