#include "window_switcher.h"

#include <algorithm>

namespace pulse::app {

bool StepWindowSwitcher(WindowSwitcherState& state, const std::vector<HWND>& mru, int dir) {
    if (!state.IsOpen()) {
        if (mru.size() < 2) return false;
        state.order = mru;
        state.selected = dir < 0 ? static_cast<int>(mru.size()) - 1 : 1;
        return true;
    }
    const int count = static_cast<int>(state.order.size());
    if (count < 2) {
        CancelWindowSwitcher(state);
        return false;
    }
    const int step = dir < 0 ? -1 : 1;
    state.selected = ((state.selected + step) % count + count) % count;
    return true;
}

void SelectWindowSwitcher(WindowSwitcherState& state, int index) {
    if (state.IsOpen() && index >= 0 && index < static_cast<int>(state.order.size())) state.selected = index;
}

void PruneWindowSwitcher(WindowSwitcherState& state, const std::vector<HWND>& alive) {
    if (!state.IsOpen()) return;
    const auto is_alive = [&](HWND h) { return std::find(alive.begin(), alive.end(), h) != alive.end(); };
    if (state.order.empty() || !is_alive(state.order.front())) {
        CancelWindowSwitcher(state);
        return;
    }
    std::vector<HWND> kept;
    int selected = -1;
    for (int i = 0; i < static_cast<int>(state.order.size()); ++i) {
        const HWND h = state.order[static_cast<size_t>(i)];
        if (i == state.selected) selected = static_cast<int>(kept.size());  // its slot, or the next window's
        if (is_alive(h)) kept.push_back(h);
    }
    if (kept.size() < 2) {
        CancelWindowSwitcher(state);
        return;
    }
    // The last card closed: the window before it, not the current one.
    if (selected >= static_cast<int>(kept.size())) selected = static_cast<int>(kept.size()) - 1;
    state.order = std::move(kept);
    state.selected = selected;
}

HWND CommitWindowSwitcher(WindowSwitcherState& state) {
    HWND target = nullptr;
    if (state.IsOpen() && state.selected > 0 && state.selected < static_cast<int>(state.order.size()))
        target = state.order[static_cast<size_t>(state.selected)];
    CancelWindowSwitcher(state);
    return target;
}

void CancelWindowSwitcher(WindowSwitcherState& state) {
    state.order.clear();
    state.selected = -1;
}

bool IsWindowSwitcherScanCode(LPARAM key_lparam) noexcept {
    const UINT scan = static_cast<UINT>((key_lparam >> 16) & 0xFF);
    const bool extended = (key_lparam & (1 << 24)) != 0;
    return scan == 0x29 && !extended;
}

}  // namespace pulse::app
