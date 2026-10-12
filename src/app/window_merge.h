#pragma once
// window_merge.h - Multi-window M6c: a window dragged by its title bar onto
// another window's tab strip merges into it, all its tabs in order with their
// groups; the toast that follows undoes it exactly.
#include <windows.h>

#include <cstddef>
#include <vector>

namespace pulse {
struct AppState;
struct WindowMergeUndo;

// Moves every tab of `src`, in order, into `dst` at `insert_at` (pinned tabs
// go to the end of the pinned region; a group run there is not split). Groups
// come along under new ids and src's active tab becomes active. `src` is left
// without tabs. The result tells RestoreMergedTabs how to put them back; its
// `place` is left for the caller.
WindowMergeUndo MergeTabsInto(AppState& src, AppState& dst, size_t insert_at);

// Moves the remembered tabs still in `from` to the end of `back`, in their old
// order, with their old groups and active tab. `from` keeps at least one tab.
// Returns how many of them are in `back` afterwards.
size_t RestoreMergedTabs(AppState& from, AppState& back, const WindowMergeUndo& undo);

// Merges window `src` into `dst` (MergeTabsInto), destroys `src` and offers
// the undo in a toast in `dst`. `before` is src's placement before the drag.
// Touch nothing of `src` afterwards.
bool MergeWindowInto(AppState& src, AppState& dst, size_t insert_at, const WINDOWPLACEMENT& before);

// "Merge all windows" (M7e): merges each of `sources` that is another window
// of dst's core into `dst`, in order, at the end of its tabs, and offers one
// undo for all of them. Returns how many were merged.
size_t MergeWindowsInto(const std::vector<HWND>& sources, AppState& dst);

// WM_UNDO_WINDOW_MERGE: the windows of the last merge into `dst` open again
// where they were, with their layouts and their tabs, groups and active tabs.
void UndoWindowMerge(AppState& dst);
}  // namespace pulse
