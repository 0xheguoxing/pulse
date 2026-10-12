#pragma once
// window_overview.h - The window overview (multi-window M7e, Ctrl+Shift+O):
// one card per Pulse window with its tabs, drawn over the window it was opened
// in. Tabs are dragged between cards or onto the "new window" card; one
// button merges every window into this one. The rules here are pure; the
// view is ui/window_overview_view.h and the glue app/window_overview_ui.h.
#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace pulse::app {

// What a tab dragged in the overview does where it is released.
enum class OverviewDrop {
    None,          // on its own card, or on nothing: nothing changes
    MoveTab,       // on another window's card: it moves to the end of its tabs
    MoveLastTab,   // the same, and it was the last tab: its window goes away
    NewWindow,     // on the "new window" card: it opens in a window of its own
    AlreadyAlone,  // the same, but it is the only tab: it already is one
};

// `source_card`: the card the tab came from; `source_tabs`: how many tabs that
// window has; `target_card`: the card under the release point (-1: none);
// `on_new_window`: released on the "new window" card.
OverviewDrop DecideOverviewDrop(int source_card, size_t source_tabs, int target_card, bool on_new_window);

// Cards keep their places while the overview is open: windows that went away
// leave and new ones are added at the end, in `alive` order.
void SyncOverviewOrder(std::vector<HWND>& order, const std::vector<HWND>& alive);

// A press on a tab becomes a drag once it leaves the system drag rectangle
// (`slop`: half its size, px) around the press.
bool OverviewDragStarted(POINT press, POINT now, SIZE slop);

struct WindowOverviewState {
    bool open = false;
    std::vector<HWND> order;  // one card per window, in this order
    // A press on a tab, which becomes a drag once it moves.
    bool pressed = false;
    bool dragging = false;
    int press_card = -1;
    int press_chip = -1;   // its chip on the card
    size_t press_tab = 0;  // index in that window's tabs
    POINT press_pt{};
    // An action is opening or closing windows: the activation changes that
    // come with it do not close the overview.
    bool busy = false;
    // Said in place of the hint until the next click (a drop that did nothing).
    std::wstring notice;
};

}  // namespace pulse::app
