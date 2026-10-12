#pragma once
// reopen_closed.h - Multi-window M7f (plan §8.4): Ctrl+Shift+T over one
// timeline of tabs closed by hand and windows closed while others stayed
// open (closed_tabs.h, closed_windows.h). Tabs go back to their window and
// place when that window is still open, else into the current window; the
// tabs one action closed come back together.
#include <cstddef>
#include <vector>

namespace pulse {
struct AppState;

// Right before the tabs at `indices` close by hand: remembers those that will
// close (never pinned tabs nor the last tab) as one action. When that is two
// or more, a toast offers them back.
void RememberClosingTabs(AppState& s, const std::vector<size_t>& indices);
// Ctrl+Shift+T: reopens the tabs or the window closed last. False when there
// is nothing to reopen.
bool ReopenLastClosed(AppState& s);
// WM_REOPEN_CLOSED_TABS (the toast): the tabs closed last come back.
void ReopenClosedTabs(AppState& s);
// Hides every window's toast whose click posts `message` (WM_REOPEN_CLOSED_TABS
// or WM_REOPEN_CLOSED_WINDOW): what it offered came back, or a later close
// would be what it brings back.
void DismissReopenToasts(AppState& s, unsigned message);
}  // namespace pulse
