#pragma once
// window_overview_ui.h - Multi-window M7e: the window overview over the window
// it was opened in (Ctrl+Shift+O, or the window name menu). Every window is a
// card with its tabs:
//  - click a tab: go to that window and tab; "Switch to": go to the window;
//  - drag a tab onto another card: it moves there (its last tab: the window
//    goes away); onto "New window": it gets a window of its own;
//  - "Merge all windows": every other window joins this one, one toast
//    undoes it;
//  - Esc, "Done", a click outside the panel or leaving the window closes it.
// The rules are app/window_overview.h, the drawing ui/window_overview_view.h.
#include <windows.h>

namespace pulse {
struct AppState;

// Opens the overview, or closes it when it is open.
void ToggleWindowOverview(AppState& s);
// Every message while the overview is open (and Ctrl+Shift+O before). True
// when handled; `result` is then the window procedure's reply.
bool WindowOverviewMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result);
}  // namespace pulse
