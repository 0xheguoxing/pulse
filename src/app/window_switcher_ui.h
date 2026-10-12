#pragma once
// window_switcher_ui.h - Multi-window M7d (plan section 8, P3): the Ctrl+`
// window switcher on screen. Holding Ctrl, the key below Esc opens a card
// per window over the current one (most recent first, the previous window
// selected) and moves on with each press, Shift going back; releasing Ctrl
// switches to the selected window. Esc, a click outside the cards or losing
// focus keeps the current window. Rules: window_switcher.h; drawing:
// ui/window_switcher_view.h.
#include <windows.h>

namespace pulse {
struct AppState;

// WM_KEYDOWN: Ctrl+` (Ctrl+Shift+` backwards) opens the switcher or steps it.
// False when it is not that key or there is only one window.
bool HandleWindowSwitcherKey(AppState& s, LPARAM key_lparam);
// While open the switcher takes the window's keyboard and mouse input.
// True when the message was consumed.
bool WindowSwitcherMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

}  // namespace pulse
