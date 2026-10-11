#pragma once
// pane_drop.h - Multi-window M6d: a tab dropped on a window's content area,
// either as a new tab (center) or as one more pane of the active tab (edge).
#include "app_model.h"

#include <windows.h>

#include <memory>

namespace pulse::app {
enum class ContentDrop { None, Center, Left, Top, Right, Bottom };

// Where in `content` (client px) a drop at `pt` goes: within `band` (a share
// of the width for left/right, of the height for top/bottom) of an edge it
// splits there, nearest edge first; anywhere else it is a new tab. None
// outside `content`.
ContentDrop ContentDropZone(const D2D1_RECT_F& content, POINT pt, float band);

// The layout a tab with `visible` panes takes with one more pane dropped on
// `edge`: 1 -> two side by side (left/right) or stacked (top/bottom), 2 ->
// three, 3 -> four. Single when the tab cannot take another pane.
LayoutPreset GrownPreset(size_t visible, ContentDrop edge);

// True when `tab` can take the only pane of `incoming` at `edge`.
bool CanAddPane(const LayoutTab& tab, const LayoutTab& incoming, ContentDrop edge);

// Where the dropped pane would be in `content`: laid out like the grown tab,
// with the new pane first (left, top) or last (right, bottom).
D2D1_RECT_F PaneDropRect(const LayoutTab& tab, ContentDrop edge, const D2D1_RECT_F& content, float scale);

// Moves the only pane of `incoming` into `tab` at `edge` (CanAddPane) and
// focuses it. The panes are reordered so the visible ones come first in
// their on-screen order: layout presets and session restore follow it.
bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, ContentDrop edge);
}  // namespace pulse::app
