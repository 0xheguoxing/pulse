#include "window_overview.h"

#include <algorithm>

namespace pulse::app {

OverviewDrop DecideOverviewDrop(int source_card, size_t source_tabs, int target_card, bool on_new_window) {
    if (source_card < 0 || source_tabs == 0) return OverviewDrop::None;
    if (on_new_window) return source_tabs == 1 ? OverviewDrop::AlreadyAlone : OverviewDrop::NewWindow;
    if (target_card < 0 || target_card == source_card) return OverviewDrop::None;
    return source_tabs == 1 ? OverviewDrop::MoveLastTab : OverviewDrop::MoveTab;
}

void SyncOverviewOrder(std::vector<HWND>& order, const std::vector<HWND>& alive) {
    const auto is_alive = [&](HWND h) { return std::find(alive.begin(), alive.end(), h) != alive.end(); };
    order.erase(std::remove_if(order.begin(), order.end(), [&](HWND h) { return !h || !is_alive(h); }), order.end());
    for (HWND h : alive)
        if (h && std::find(order.begin(), order.end(), h) == order.end()) order.push_back(h);
}

bool OverviewDragStarted(POINT press, POINT now, SIZE slop) {
    return now.x < press.x - slop.cx || now.x > press.x + slop.cx || now.y < press.y - slop.cy ||
           now.y > press.y + slop.cy;
}

}  // namespace pulse::app
