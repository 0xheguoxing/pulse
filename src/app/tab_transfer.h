#pragma once
// tab_transfer.h - Multi-window M5b: a live tab moves from one window to
// another sharing the same core, keeping its panes, splits, history, scroll
// position, selection and pin (it leaves its group).
#include <cstddef>

#include "pane_drop.h"

namespace pulse {
struct AppState;

// What happens to the window a tab leaves.
enum class TransferSource {
    KeepsTabs,  // it must keep at least one tab
    Closing,    // it may give up its last tab; the caller hides and destroys
                // it right away, before it paints without a tab (M6a)
};

// Moves tab `index` of `src` into `dst` at `insert_at` (clamped to the pinned
// or unpinned region) and makes it active there. Editing that belongs to the
// moved tab ends in `src` first: rename and address edits are cancelled, a
// filter edit keeps its text, and Quick Look closes. Refuses (false) when both
// are the same window or, unless `source` is Closing, when `src` would be left
// without a tab. The caller owns any tab-strip drag state.
bool MoveLayoutTab(AppState& src, size_t index, AppState& dst, size_t insert_at,
                   TransferSource source = TransferSource::KeepsTabs);
// Moves the only pane of tab `index` of `src` into the active tab of `dst` as
// one more pane at `edge` (M6d), focused. Refuses (false, nothing changes)
// where MoveLayoutTab would, and when app::CanAddPane says no.
bool MoveTabIntoPane(AppState& src, size_t index, AppState& dst, app::ContentDrop edge,
                     TransferSource source = TransferSource::KeepsTabs);
}  // namespace pulse
