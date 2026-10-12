#pragma once
// pane_drop.h - Multi-window M6d: a tab dropped on a window's content area,
// either as a new tab (center) or as one more pane of the active tab (edge).
#include "app_model.h"

#include <windows.h>

#include <memory>
#include <vector>

namespace pulse::app {
enum class ContentDrop { None, Center, Left, Top, Right, Bottom };

// Where in `content` (client px) a drop at `pt` goes: within `band` (a share
// of the width for left/right, of the height for top/bottom) of an edge it
// splits there, nearest edge first; anywhere else it is a new tab. None
// outside `content`.
ContentDrop ContentDropZone(const D2D1_RECT_F& content, POINT pt, float band);

// The most panes one tab shows.
constexpr size_t kMaxTabPanes = 4;
// The layout a tab with `visible` panes takes with `added` more dropped on
// `edge`: two in all -> side by side (left/right) or stacked (top/bottom),
// three -> three, four -> four. Single when that is more than four.
LayoutPreset GrownPreset(size_t visible, size_t added, ContentDrop edge);
inline LayoutPreset GrownPreset(size_t visible, ContentDrop edge) { return GrownPreset(visible, 1, edge); }
// How many panes `incoming` brings to a split: its visible panes, or 0 when
// it also keeps hidden ones (they have no place in the grown layout).
size_t DropPaneCount(const LayoutTab& incoming);
// True when `tab` has room for all the panes `incoming` brings (M7h: a tab
// split in two can join a single pane, up to four in all).
bool HasRoomForPanes(const LayoutTab& tab, const LayoutTab& incoming);
// True when `tab` can take the panes of `incoming` at `edge`.
bool CanAddPane(const LayoutTab& tab, const LayoutTab& incoming, ContentDrop edge);
// Where `added` dropped panes would be in `content`, in their order: laid
// out like the grown tab, with the new panes first (left, top) or last
// (right, bottom). Two joining two at the left or right take one column.
std::vector<D2D1_RECT_F> PaneDropRects(const LayoutTab& tab, size_t added, ContentDrop edge,
                                       const D2D1_RECT_F& content, float scale);
// The one-pane case of PaneDropRects.
D2D1_RECT_F PaneDropRect(const LayoutTab& tab, ContentDrop edge, const D2D1_RECT_F& content, float scale);
// Moves the panes of `incoming` into `tab` at `edge` (CanAddPane), keeping
// their order, and focuses the one that was focused there. The panes are
// reordered so the visible ones come first in their on-screen order: layout
// presets and session restore follow it.
bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, ContentDrop edge);

// M7i: where dropped panes go, as one of the layouts a tab can keep (session
// restore stores a preset and its split ratios, not a free tree): the grown
// preset, the places of the added panes in its order (ascending), and the
// order the panes already there take in the other places (indexes into the
// visible panes; empty keeps their order).
struct PanePlacement {
    LayoutPreset layout = LayoutPreset::Single;  // Single: no split
    std::vector<size_t> slots;
    std::vector<size_t> kept;
    bool Splits() const noexcept { return layout != LayoutPreset::Single && !slots.empty(); }
    bool operator==(const PanePlacement&) const = default;
};
// What a drop at the content edge `edge` did before M7i: the added panes in
// front (left, top) or at the back (right, bottom); two joining two at the
// left or right take one column.
PanePlacement EdgePlacement(const LayoutTab& tab, size_t added, ContentDrop edge);
// Where letting go at `pt` (client px, `content` as PaneDropRects) puts
// `added` panes: the visible pane under the cursor splits at the edge of it
// the cursor is near (within `band` of that pane, as ContentDropZone), and
// of the layouts with room for all of them the one whose new panes best cover
// that half of the pane wins, moving the other panes the least.
// `side` gets the pane edge (Center in the middle of a pane, None outside
// `content`). No split (Single) in the middle of a pane or without room.
PanePlacement AimPaneDrop(const LayoutTab& tab, size_t added, const D2D1_RECT_F& content, float scale, POINT pt,
                          float band, ContentDrop* side = nullptr);
// Where the added panes of `placement` would be in `content`, in their order;
// just `content` when it does not split.
std::vector<D2D1_RECT_F> PlacementRects(const LayoutTab& tab, const PanePlacement& placement,
                                        const D2D1_RECT_F& content, float scale);
// True when `tab` can take the panes of `incoming` as `placement` says.
bool CanPlacePanes(const LayoutTab& tab, const LayoutTab& incoming, const PanePlacement& placement);
// AddPaneFromTab with the panes going where `placement` says.
bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, const PanePlacement& placement);
// Where a pane dragged out of a split was: its place among the visible
// panes, and the layout and split ratios it was part of.
struct PaneOrigin {
    size_t position = 0;
    LayoutPreset layout = LayoutPreset::Single;
    std::vector<float> ratios;
    bool target = false;  // it was the copy/move target pane
};

// Takes visible `pane` out of `tab` (which must show at least two) into a
// tab of its own, focused. `tab` keeps the rest in their order with one pane
// fewer (3 -> two side by side, 4 -> three); `origin` records how to put it
// back. Null when `pane` is not one of at least two visible panes.
std::unique_ptr<LayoutTab> TakePaneAsTab(LayoutTab& tab, Pane* pane, PaneOrigin& origin);

// True when the only pane of `incoming` can go back into `tab` at `origin`
// (`tab` still shows one pane fewer than that layout).
bool CanPutPaneBack(const LayoutTab& tab, const LayoutTab& incoming, const PaneOrigin& origin);

// Puts the only pane of `incoming` back where `origin` says, with the old
// layout and split ratios, focused.
bool PutPaneBack(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, const PaneOrigin& origin);
}  // namespace pulse::app
