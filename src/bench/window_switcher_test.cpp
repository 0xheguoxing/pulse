// window_switcher_test.cpp - Multi-window M7d: the Ctrl+` switcher's rules
// (window_switcher.h). Pure checks, [PASS]/[FAIL] lines; exit code = failures.
#include "../app/window_switcher.h"

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

void Opening() {
    WindowSwitcherState st;
    Check(!st.IsOpen(), "starts closed");
    Check(!StepWindowSwitcher(st, {H(1)}, 1) && !st.IsOpen(), "one window: Ctrl+` does nothing");
    Check(!StepWindowSwitcher(st, {}, 1) && !st.IsOpen(), "no windows: nothing");
    Check(StepWindowSwitcher(st, {H(1), H(2), H(3)}, 1) && st.selected == 1,
          "opens on the previous window (second most recent), like Alt+Tab");
    Check(st.order == std::vector<HWND>({H(1), H(2), H(3)}), "keeps the most-recent-first snapshot");
    WindowSwitcherState back;
    Check(StepWindowSwitcher(back, {H(1), H(2), H(3)}, -1) && back.selected == 2,
          "Ctrl+Shift+` opens on the least recent window");
}

void Stepping() {
    WindowSwitcherState st;
    StepWindowSwitcher(st, {H(1), H(2), H(3)}, 1);
    StepWindowSwitcher(st, {H(9)}, 1);  // the list passed later is ignored: the snapshot stays
    Check(st.selected == 2 && st.order.size() == 3, "next press moves on within the snapshot");
    StepWindowSwitcher(st, {}, 1);
    Check(st.selected == 0, "wraps around to the current window");
    StepWindowSwitcher(st, {}, -1);
    Check(st.selected == 2, "Shift steps back and wraps the other way");
    SelectWindowSwitcher(st, 1);
    Check(st.selected == 1, "a click selects a card");
    SelectWindowSwitcher(st, 7);
    SelectWindowSwitcher(st, -1);
    Check(st.selected == 1, "out-of-range selection is ignored");
}

void Committing() {
    WindowSwitcherState st;
    StepWindowSwitcher(st, {H(1), H(2), H(3)}, 1);
    Check(CommitWindowSwitcher(st) == H(2) && !st.IsOpen(), "releasing Ctrl switches to the selected window and closes");
    StepWindowSwitcher(st, {H(1), H(2)}, 1);
    StepWindowSwitcher(st, {}, 1);
    Check(st.selected == 0 && CommitWindowSwitcher(st) == nullptr && !st.IsOpen(),
          "back on the current window: releasing Ctrl changes nothing");
    StepWindowSwitcher(st, {H(1), H(2)}, 1);
    CancelWindowSwitcher(st);
    Check(!st.IsOpen() && st.order.empty() && CommitWindowSwitcher(st) == nullptr, "Esc closes without switching");
}

void Pruning() {
    WindowSwitcherState st;
    StepWindowSwitcher(st, {H(1), H(2), H(3), H(4)}, 1);
    StepWindowSwitcher(st, {}, 1);  // H(3)
    PruneWindowSwitcher(st, {H(1), H(2), H(3), H(4)});
    Check(st.selected == 2 && st.order.size() == 4, "nothing closed: unchanged");
    PruneWindowSwitcher(st, {H(1), H(3), H(4)});
    Check(st.order == std::vector<HWND>({H(1), H(3), H(4)}) && st.order[static_cast<size_t>(st.selected)] == H(3),
          "an earlier window closed: the selection stays on its window");
    PruneWindowSwitcher(st, {H(1), H(4)});
    Check(st.order == std::vector<HWND>({H(1), H(4)}) && st.order[static_cast<size_t>(st.selected)] == H(4),
          "the selected window closed: the next one is selected");
    StepWindowSwitcher(st, {}, 1);
    StepWindowSwitcher(st, {}, 1);  // back on H(4)
    WindowSwitcherState last;
    StepWindowSwitcher(last, {H(1), H(2), H(3)}, -1);  // H(3), the last
    PruneWindowSwitcher(last, {H(1), H(2)});
    Check(last.IsOpen() && last.order[static_cast<size_t>(last.selected)] == H(2),
          "the last card closed: the card before it is selected, not the current window");
    PruneWindowSwitcher(st, {H(1)});
    Check(!st.IsOpen(), "fewer than two windows left: closes");
    WindowSwitcherState host;
    StepWindowSwitcher(host, {H(1), H(2), H(3)}, 1);
    PruneWindowSwitcher(host, {H(2), H(3)});
    Check(!host.IsOpen(), "the window showing it closed: closes");
    WindowSwitcherState closed;
    PruneWindowSwitcher(closed, {H(1)});
    Check(!closed.IsOpen(), "pruning a closed switcher is a no-op");
}

void Key() {
    const auto lp = [](UINT scan, bool extended) {
        return static_cast<LPARAM>((scan << 16) | (extended ? (1u << 24) : 0u) | 1u);
    };
    Check(IsWindowSwitcherScanCode(lp(0x29, false)), "scan code 0x29 (key below Esc) is the switcher key");
    Check(!IsWindowSwitcherScanCode(lp(0x02, false)), "the 1 key is not");
    Check(!IsWindowSwitcherScanCode(lp(0x29, true)), "an extended key with the same scan code is not");
    Check(IsWindowSwitcherScanCode(lp(0x29, false) | (1 << 30)), "auto-repeat still counts (holding steps on)");
}

}  // namespace

int main() {
    Opening();
    Stepping();
    Committing();
    Pruning();
    Key();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
