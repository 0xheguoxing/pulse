// window_overview_test.cpp - Multi-window M7e: the window overview's rules
// (window_overview.h). Pure checks, [PASS]/[FAIL] lines; exit code = failures.
#include "../app/window_overview.h"

#include <cstdio>
#include <vector>

namespace {

int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

HWND H(int n) { return reinterpret_cast<HWND>(static_cast<INT_PTR>(0x1000 + n)); }

using namespace pulse::app;

void Drops() {
    Check(DecideOverviewDrop(0, 3, 1, false) == OverviewDrop::MoveTab, "onto another card: the tab moves there");
    Check(DecideOverviewDrop(2, 3, 0, false) == OverviewDrop::MoveTab, "works towards an earlier card too");
    Check(DecideOverviewDrop(0, 1, 1, false) == OverviewDrop::MoveLastTab,
          "its window's last tab: it moves and the window goes away");
    Check(DecideOverviewDrop(1, 3, 1, false) == OverviewDrop::None, "back onto its own card: nothing");
    Check(DecideOverviewDrop(1, 1, 1, false) == OverviewDrop::None, "last tab back onto its own card: nothing");
    Check(DecideOverviewDrop(0, 3, -1, false) == OverviewDrop::None, "released on nothing: nothing");
    Check(DecideOverviewDrop(0, 3, -1, true) == OverviewDrop::NewWindow, "onto \"new window\": a window of its own");
    Check(DecideOverviewDrop(0, 1, -1, true) == OverviewDrop::AlreadyAlone,
          "an only tab onto \"new window\": it already is one");
    Check(DecideOverviewDrop(-1, 3, 1, false) == OverviewDrop::None, "no source card: nothing");
    Check(DecideOverviewDrop(0, 0, 1, false) == OverviewDrop::None, "a window without tabs: nothing");
}

void Order() {
    std::vector<HWND> order;
    SyncOverviewOrder(order, {H(3), H(1), H(2)});
    Check(order == std::vector<HWND>({H(3), H(1), H(2)}), "opens in the order given (most recent first)");
    SyncOverviewOrder(order, {H(2), H(3), H(1)});
    Check(order == std::vector<HWND>({H(3), H(1), H(2)}), "cards stay put when the recent order changes");
    SyncOverviewOrder(order, {H(4), H(2), H(3)});
    Check(order == std::vector<HWND>({H(3), H(2), H(4)}), "a closed window leaves, a new one goes to the end");
    SyncOverviewOrder(order, {H(5), H(6), H(3), H(2), H(4)});
    Check(order == std::vector<HWND>({H(3), H(2), H(4), H(5), H(6)}), "several new windows keep their given order");
    SyncOverviewOrder(order, {});
    Check(order.empty(), "no windows: no cards");
    std::vector<HWND> with_null{H(1), nullptr};
    SyncOverviewOrder(with_null, {H(1), nullptr});
    Check(with_null == std::vector<HWND>({H(1)}), "a null window never gets a card");
}

void Drag() {
    const POINT p{100, 100};
    const SIZE slop{4, 4};
    Check(!OverviewDragStarted(p, p, slop), "no movement: still a click");
    Check(!OverviewDragStarted(p, POINT{104, 96}, slop), "inside the drag rectangle: still a click");
    Check(OverviewDragStarted(p, POINT{105, 100}, slop), "past it to the right: a drag");
    Check(OverviewDragStarted(p, POINT{100, 95}, slop), "past it upwards: a drag");
    Check(OverviewDragStarted(p, POINT{90, 100}, slop), "past it to the left: a drag");
}

}  // namespace

int main() {
    Drops();
    Order();
    Drag();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
