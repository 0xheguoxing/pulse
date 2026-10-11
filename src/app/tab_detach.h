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

// A visible tab of a strip, in strip order: its window_tabs index and rect.
struct TabSpan {
    size_t index = 0;
    RECT rect{};
};
// Where a tab dropped at `pt` (client px) lands among `spans`: before the
// first tab whose middle is right of `pt`, else at `count` (the end). The
// whole tab row counts, as in browsers: -1 only when `pt` is more than
// `slack` px above or below the tabs or left of the first one, or right of
// `strip_right` (where the caption buttons start).
int TabDropIndex(const TabSpan* spans, size_t span_count, size_t count, POINT pt, LONG slack,
                 LONG strip_right);

// WM_BEGIN_TAB_DETACH: runs the system move loop for `s` while the button is
// still down. `from` is the window the tab came from and `index` its old
// position (nullptr: the whole window was dragged by its only tab). After Esc
// the tab goes back and `s` is destroyed. Released over another Pulse
// window's tab strip, a window with one tab merges into it there (M6b).
void RunDetachedTabMove(AppState& s, HWND from, size_t index);

// WM_MOVING: while `s` is in that move loop, marks where its tab would land
// on the tab strip of the Pulse window under the cursor (M6b).
void TrackTabDrop(AppState& s);
}  // namespace pulse
