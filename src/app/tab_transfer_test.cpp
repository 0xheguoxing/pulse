#ifdef PULSE_WITH_SELFTEST
// tab_transfer_test.cpp - Multi-window M5b: WindowTabs::TakeTab/InsertTab and
// MoveLayoutTab move a live tab between two windows sharing one core.
#include "app_internal.h"
#include "tab_detach.h"
#include "tab_transfer.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace pulse::app {
namespace {
bool GroupRunsContiguous(const WindowTabs& tabs) {
    std::set<int> ended;
    int previous = 0;
    for (const auto& tab : tabs.items) {
        if (tab->tab_group != previous) {
            if (previous) ended.insert(previous);
            if (tab->tab_group && ended.contains(tab->tab_group)) return false;
        }
        previous = tab->tab_group;
    }
    return true;
}

bool HasGroup(const WindowTabs& tabs, int id) {
    for (const auto& group : tabs.tab_groups)
        if (group.id == id) return true;
    return false;
}

// Tabs on This PC: no folder loads and no watches in a hidden test window.
void Seed(WindowTabs& tabs, size_t count) {
    tabs = WindowTabs{};
    for (size_t i = 0; i < count; ++i) tabs.NewTabAtLocation(tabs.items.size(), L"");
}
}  // namespace

bool RunTabTransferTest() {
    const auto output = std::filesystem::absolute(L"bench_data/tab-transfer");
    std::filesystem::create_directories(output);
    std::ofstream log(output / L"results.log");
    bool ok = log.good();
    const auto check = [&](bool value, const char* label) {
        log << (value ? "[PASS] " : "[FAIL] ") << label << std::endl;
        ok &= value;
        return value;
    };

    // --- WindowTabs model ------------------------------------------------
    {
        WindowTabs one;
        Seed(one, 1);
        check(!one.TakeTab(0) && one.items.size() == 1, "the last tab of a window cannot be taken");
        LayoutTab* last = one.items[0].get();
        std::unique_ptr<LayoutTab> taken_last = one.TakeTab(0, true);
        check(taken_last.get() == last && one.items.empty() && one.active == 0 && !one.Active(),
              "a closing window may give up its last tab (M6a)");

        WindowTabs src;
        Seed(src, 4);
        src.items[0]->pinned = true;
        src.tab_groups.push_back(TabGroup{});
        src.tab_groups.back().id = 5;
        src.tab_groups.push_back(TabGroup{});
        src.tab_groups.back().id = 6;
        src.items[2]->tab_group = 5;
        src.items[3]->tab_group = 6;
        src.active = 3;
        LayoutTab* grouped = src.items[2].get();
        std::unique_ptr<LayoutTab> taken = src.TakeTab(2);
        check(taken.get() == grouped && taken->tab_group == 0 && !HasGroup(src, 5) && HasGroup(src, 6),
              "a taken tab leaves its group; the group it left empty is dropped, others stay");
        check(src.items.size() == 3 && src.active == 2 && src.items[2]->tab_group == 6,
              "the active tab after the taken one keeps its identity");
        LayoutTab* pinned = src.items[0].get();
        std::unique_ptr<LayoutTab> taken_pin = src.TakeTab(0);
        check(taken_pin.get() == pinned && taken_pin->pinned, "a pinned tab may leave and stays pinned");

        WindowTabs dst;
        Seed(dst, 4);
        dst.items[0]->pinned = true;
        dst.items[2]->tab_group = 9;
        dst.items[3]->tab_group = 9;
        size_t at = dst.InsertTab(std::move(taken_pin), 99);
        check(at == 1 && dst.items[1].get() == pinned && dst.active == 1,
              "a pinned tab lands at the end of the pinned region and becomes active");
        at = dst.InsertTab(std::move(taken), 0);
        check(at == 2 && dst.items[2].get() == grouped, "an unpinned tab cannot land among pinned tabs");
        std::unique_ptr<LayoutTab> loose = src.TakeTab(0);
        LayoutTab* loose_raw = loose.get();
        at = dst.InsertTab(std::move(loose), 5);  // between the two members of group 9
        check(at == 6 && dst.items[6].get() == loose_raw && GroupRunsContiguous(dst),
              "a tab that would split a group run lands after the run");
    }

    // --- Drag-out geometry (M6a) ------------------------------------------
    {
        const RECT r = DetachedWindowRect(SIZE{1200, 750}, POINT{900, 300}, POINT{150, 20}, POINT{8, 0});
        check(r.left == 742 && r.top == 280 && r.right == 1942 && r.bottom == 1030,
              "the detached window keeps the source size and the cursor keeps its grab point on the tab");
        check(!TabDetachDistanceReached(POINT{100, 20}, POINT{400, 55}, 1.5f) &&
                  TabDetachDistanceReached(POINT{100, 20}, POINT{100, 56}, 1.5f) &&
                  TabDetachDistanceReached(POINT{100, 40}, POINT{100, 16}, 1.0f),
              "a tab leaves the strip after 24 DIP up or down; sideways travel alone reorders");
    }

    // --- MoveLayoutTab between two windows --------------------------------
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseTabTransferTest";
    RegisterClassW(&wc);
    const auto make_window = [&] {
        return CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, -30000, -30000, 200, 200,
                               nullptr, nullptr, wc.hInstance, nullptr);
    };
    auto core = std::make_unique<AppCore>();
    auto src = std::make_unique<AppState>(*core);
    auto dst = std::make_unique<AppState>(*core);
    src->hwnd = make_window();
    dst->hwnd = make_window();
    if (check(src->hwnd && dst->hwnd, "two hidden windows share one core")) {
        core->windows.Add(src->hwnd, src.get());
        core->windows.Add(dst->hwnd, dst.get());
        core->windows.Touch(dst->hwnd);
        Seed(src->window_tabs, 3);
        Seed(dst->window_tabs, 2);
        BindCurrentLayout(*dst);

        // The moved tab: split in two, focus on the right pane, scrolled and
        // with a selection in it.
        LayoutTab* moved = src->window_tabs.items[1].get();
        auto extra = MakeSingleLayoutTab(L"");
        extra->panes.front()->ActiveTab()->current_path.clear();
        moved->panes.push_back(std::move(extra->panes.front()));
        moved->root.reset();
        src->window_tabs.SwitchTab(1);
        BindCurrentLayout(*src);
        Pane* right = moved->panes[1].get();
        Tab* view = right->ActiveTab();
        view->scroll_y = 240.0f;
        view->selected_index = 3;
        src->pane = right;
        src->renameClickCandidate = true;
        src->renameClickTab = view;
        src->renameClickPane = right;
        src->listTypeAheadTab = view;
        src->tabFlashKey = moved;
        view->current_path = L"pulse:search:report";
        view->pending_generation = 12;

        check(!MoveLayoutTab(*src, 1, *src, 0) && src->window_tabs.items.size() == 3,
              "a tab is not moved into its own window");
        const bool moved_ok = MoveLayoutTab(*src, 1, *dst, 99);
        check(moved_ok && src->window_tabs.items.size() == 2 && dst->window_tabs.items.size() == 3 &&
                  dst->window_tabs.items[2].get() == moved && dst->window_tabs.Active() == moved,
              "the same live tab object moves to the end of the other window and is active there");
        check(dst->pane == right && right->focused && !moved->panes[0]->focused && moved->root,
              "the split moves with it and the focused pane stays focused");
        check(right->ActiveTab()->selected_index == 3 && dst->scrollTargetY == 240.0f,
              "selection and scroll position travel with the tab");
        bool src_bound = false;
        for (const auto& tab : src->window_tabs.items)
            for (const auto& pane : tab->panes) src_bound |= pane.get() == src->pane;
        check(src_bound && src->window_tabs.Active() != moved, "the source window binds one of its own tabs");
        check(!src->renameClickCandidate && !src->renameClickTab && !src->listTypeAheadTab && !src->tabFlashKey,
              "the source window forgets clicks, type-ahead and flash aimed at the moved tab");
        check(&SearchResultWindow(*src, 12) == dst.get(),
              "a search still running in the moved tab now reaches the window it moved to");
        view->pending_generation = 0;
        view->current_path.clear();

        Seed(src->window_tabs, 1);
        BindCurrentLayout(*src);
        check(!MoveLayoutTab(*src, 0, *dst, 0) && src->window_tabs.items.size() == 1 &&
                  dst->window_tabs.items.size() == 3,
              "a window's only tab stays (the move is refused, nothing changes)");
        LayoutTab* only = src->window_tabs.items[0].get();
        check(MoveLayoutTab(*src, 0, *dst, 1, TransferSource::Closing) && src->window_tabs.items.empty() &&
                  !src->pane && dst->window_tabs.items.size() == 4 && dst->window_tabs.items[1].get() == only &&
                  dst->window_tabs.Active() == only,
              "a closing window hands over its last tab, which lands at the requested place (Esc after a drag-out)");

        core->windows.Remove(dst->hwnd);
        core->windows.Remove(src->hwnd);
    }
    for (HWND* hwnd : {&src->hwnd, &dst->hwnd}) {
        if (*hwnd) DestroyWindow(*hwnd);
        *hwnd = nullptr;
    }
    dst.reset();
    src.reset();
    core.reset();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}
}  // namespace pulse::app
#endif
