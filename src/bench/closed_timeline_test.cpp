// closed_timeline_test.cpp - Multi-window M7f (plan §8.4): tabs closed by hand
// and closed windows form one Ctrl+Shift+T timeline. The tabs one action
// closed come back together, the stack keeps the newest kCapacity tabs, and
// a closed window survives the session round trip unchanged.
#include "../app/closed_tabs.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace pulse::app;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

ClosedTab MakeTab(const wchar_t* title, size_t index, uint64_t seq, int group = 0) {
    ClosedTab tab;
    tab.tab.title = title;
    tab.tab.panes.resize(1);
    tab.tab.panes[0].path = std::wstring(L"C:\\") + title;
    tab.index = index;
    tab.seq = seq;
    tab.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x100 + seq));
    if (group) tab.group = {group, L"工作", 0x4CC2FF, false};
    return tab;
}

ClosedWindow MakeWindow(const wchar_t* name, uint64_t seq) {
    ClosedWindow window;
    window.name = name;
    window.rect = {-1800, 120, -600, 900};
    window.layout_tabs.resize(2);
    window.layout_tabs[0].title = L"a";
    window.layout_tabs[1].title = L"b";
    window.closed_seq = seq;
    return window;
}

}  // namespace

int main() {
    ClosedTabStack tabs;
    ClosedWindowStack windows;
    Check(NextToReopen(tabs, windows) == ReopenKind::Nothing, "nothing closed: Ctrl+Shift+T does nothing");
    Check(tabs.TakeLatestBatch().empty(), "an empty stack gives no tabs");

    tabs.Push({MakeTab(L"one", 3, 1)});
    tabs.Push({MakeTab(L"left", 0, 2, 7), MakeTab(L"mid", 2, 2, 7), MakeTab(L"right", 5, 2)});
    Check(tabs.Size() == 4 && tabs.LatestSeq() == 2, "two actions, four tabs");
    Check(NextToReopen(tabs, windows) == ReopenKind::Tabs, "only tabs closed: tabs come back");

    std::vector<ClosedTab> batch = tabs.TakeLatestBatch();
    Check(batch.size() == 3 && batch[0].tab.title == L"left" && batch[1].tab.title == L"mid" &&
              batch[2].tab.title == L"right",
          "close others: all its tabs come back together, in their old order");
    Check(batch.size() == 3 && batch[0].index == 0 && batch[1].index == 2 && batch[2].index == 5 &&
              batch[0].group.id == 7 && batch[0].group.name == L"工作" && batch[2].group.id == 0,
          "each keeps its old place and group");
    Check(tabs.Size() == 1 && tabs.LatestSeq() == 1, "the earlier action stays");
    batch = tabs.TakeLatestBatch();
    Check(batch.size() == 1 && batch[0].tab.title == L"one" && batch[0].tab.panes[0].path == L"C:\\one",
          "then the single tab closed before");
    Check(tabs.Empty() && tabs.LatestSeq() == 0, "and the stack is empty");

    // One timeline: whichever was closed last.
    tabs.Push({MakeTab(L"t1", 1, 1)});
    windows.Push(MakeWindow(L"w2", 2));
    Check(NextToReopen(tabs, windows) == ReopenKind::Window, "a window closed after a tab comes back first");
    tabs.Push({MakeTab(L"t3", 1, 3), MakeTab(L"t3b", 2, 3)});
    Check(NextToReopen(tabs, windows) == ReopenKind::Tabs, "tabs closed after a window come back first");
    tabs.TakeLatestBatch();
    Check(NextToReopen(tabs, windows) == ReopenKind::Window, "then the window");
    windows.TakeLatest();
    Check(NextToReopen(tabs, windows) == ReopenKind::Tabs, "then the oldest tab");
    tabs.TakeLatestBatch();
    windows.Push(MakeWindow(L"only", 9));
    Check(NextToReopen(tabs, windows) == ReopenKind::Window, "only a window closed: the window comes back");
    windows.TakeLatest();

    // Capacity: the oldest tabs go, whole actions stay ordered.
    ClosedTabStack full;
    for (uint64_t seq = 1; seq <= 30; ++seq) full.Push({MakeTab(L"x", 0, seq)});
    Check(full.Size() == ClosedTabStack::kCapacity && full.LatestSeq() == 30,
          "at most 25 closed tabs are kept, the newest");
    size_t taken = 0;
    uint64_t last_seq = 31;
    bool descending = true;
    while (!full.Empty()) {
        std::vector<ClosedTab> one = full.TakeLatestBatch();
        descending = descending && one.size() == 1 && one[0].seq < last_seq;
        last_seq = one.empty() ? 0 : one[0].seq;
        ++taken;
    }
    Check(descending && taken == ClosedTabStack::kCapacity && last_seq == 6,
          "they come back newest first, down to the 25th newest");
    ClosedTabStack big;
    std::vector<ClosedTab> many;
    for (size_t i = 0; i < 30; ++i) many.push_back(MakeTab(L"m", i, 4));
    big.Push(std::move(many));
    batch = big.TakeLatestBatch();
    Check(batch.size() == ClosedTabStack::kCapacity && batch.front().index == 5 && batch.back().index == 29,
          "an action closing more than 25 tabs keeps its last 25");

    // Session form of a closed window (plan §7).
    ClosedWindow original = MakeWindow(L"文档", 5);
    original.maximized = true;
    original.active_layout_tab = 1;
    original.tab_groups.push_back({3, L"g", 0x112233, true});
    original.identity = {L"Work", 4};
    WindowLayoutSnapshot layout;
    layout.details_panel = true;
    layout.details_panel_width = 420;
    original.layout = layout;
    const ClosedWindowSessionSnapshot saved = ClosedWindowToSession(original);
    Check(saved.name == L"文档" && saved.window.window_rect.left == -1800 && saved.window.maximized &&
              saved.window.layout_tabs.size() == 2 && saved.window.active_layout_tab == 1 &&
              saved.window.tab_groups.size() == 1 && saved.window.layout.details_panel_width == 420 &&
              saved.window.identity == original.identity,
          "a closed window goes into the session with its screen rect, tabs, groups, layout and identity");
    const ClosedWindow back = ClosedWindowFromSession(saved, 42);
    Check(back.name == original.name && back.rect.left == original.rect.left &&
              back.rect.bottom == original.rect.bottom && back.maximized &&
              back.layout_tabs.size() == 2 && back.layout_tabs[1].title == L"b" &&
              back.tab_groups.size() == 1 && back.tab_groups[0].collapsed &&
              back.active_layout_tab == 1 && back.layout && back.layout->details_panel_width == 420 &&
              back.identity == original.identity && back.closed_seq == 42,
          "and comes back the same, numbered for this run's timeline");
    ClosedWindow bare = MakeWindow(L"bare", 1);
    Check(ClosedWindowFromSession(ClosedWindowToSession(bare), 1).layout.has_value(),
          "back from the session it always has a layout (saved with its window)");

    std::printf("closed_timeline_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
