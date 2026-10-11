#pragma once
// tab_detach.h - Multi-window M6a (plan §8.2, Chrome model): a tab dragged off
// the tab strip becomes a real window that follows the cursor in the system
// move loop, so Snap and Snap Layouts come from Windows itself. Esc during
// the move puts the tab back where it was.
#include <windows.h>
#include <cstddef>

namespace pulse {
struct AppState;

// Vertical distance from the press, in DIP, that pulls a tab off the strip.
constexpr float kTabDetachDistanceDip = 24.0f;

// True once a strip drag has left the strip far enough to detach the tab.
bool TabDetachDistanceReached(POINT press, POINT now, float scale);

// The new window's rect: `size`, placed so that the cursor lands on
// `grab_client` (a point in the new window's client area); `client_offset` is
// the client origin relative to the window's top-left corner.
RECT DetachedWindowRect(SIZE size, POINT cursor, POINT grab_client, POINT client_offset);

// WM_MOUSEMOVE during a press on a tab (mx, my: client px). Pulls the tab off
// into a new window once the drag leaves the strip; in a window with one tab,
// any drag moves the window instead. Returns true when the strip drag ended
// here (the caller stops handling the move).
bool TryDetachDraggedTab(AppState& s, int mx, int my);

// WM_BEGIN_TAB_DETACH: runs the system move loop for `s` while the button is
// still down. `from` is the window the tab came from and `index` its old
// position (nullptr: the whole window was dragged by its only tab). After Esc
// the tab goes back and `s` is destroyed.
void RunDetachedTabMove(AppState& s, HWND from, size_t index);
}  // namespace pulse
