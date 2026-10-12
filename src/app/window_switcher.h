#pragma once
// window_switcher.h - Multi-window M7d (plan section 8, P3): the Ctrl+`
// window switcher's rules, without UI. Holding Ctrl, each press of the key
// below Esc moves to the next window in most-recently-used order (Shift goes
// back); releasing Ctrl switches to it and Esc keeps the current window.
// window_switcher_ui.h shows it.
#include <windows.h>

#include <vector>

namespace pulse::app {

// A snapshot of the windows, most recent (the one showing the switcher)
// first, taken when the switcher opens, and the selected one.
struct WindowSwitcherState {
    std::vector<HWND> order;
    int selected = -1;
    bool IsOpen() const noexcept { return selected >= 0; }
};

// Ctrl+` (dir > 0) or Ctrl+Shift+` (dir < 0). Closed: opens on `mru` with the
// next window selected (the last one for dir < 0), like Alt+Tab, when there
// are at least two windows. Open: moves the selection, wrapping around.
// True while the switcher is open.
bool StepWindowSwitcher(WindowSwitcherState& state, const std::vector<HWND>& mru, int dir);
// Selects a card directly (mouse). Out-of-range indexes are ignored.
void SelectWindowSwitcher(WindowSwitcherState& state, int index);
// Forgets windows that have closed since it opened (`alive` lists the open
// ones). The selection stays on its window, or moves to the next one when
// that closed (the one before it when it was the last); the switcher closes when the window showing it closed or
// fewer than two remain.
void PruneWindowSwitcher(WindowSwitcherState& state, const std::vector<HWND>& alive);
// Releasing Ctrl: the selected window, or nullptr when it is the current one
// or the switcher was not open. Closes the switcher.
HWND CommitWindowSwitcher(WindowSwitcherState& state);
void CancelWindowSwitcher(WindowSwitcherState& state);
// The switcher key is the key below Esc whatever the keyboard layout (scan
// code 0x29: ` on US keyboards, ^ on German ones, 半角/全角 on Japanese).
bool IsWindowSwitcherScanCode(LPARAM key_lparam) noexcept;

}  // namespace pulse::app
