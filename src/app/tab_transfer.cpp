// tab_transfer.cpp - Multi-window M5b: moves a live tab between windows.
#include "tab_transfer.h"

#include "app_internal.h"
#include "content_results_ui.h"

namespace pulse {
namespace {
bool OwnsPane(const app::LayoutTab& tab, const app::Pane* pane) {
    for (const auto& owned : tab.panes)
        if (owned.get() == pane) return true;
    return false;
}

bool OwnsView(const app::LayoutTab& tab, const app::Tab* view) {
    for (const auto& pane : tab.panes)
        if (pane->ActiveTab() == view) return true;
    return false;
}

// Ends what `s` was doing with `tab`, which is about to leave the window.
void ReleaseLeavingTab(AppState& s, const app::LayoutTab& tab, bool active) {
    if (active) {
        if (s.renameIndex >= 0) HideRenameOverlay(s, false);
        if (s.addressEditing) HideAddressEditor(s, false);
        if (s.filterEditing) HideFilterEditor(s, true);
        if (s.quickPreview.visible()) s.quickPreview.Close();
    }
    if (OwnsView(tab, s.renameClickTab) || OwnsPane(tab, s.renameClickPane)) CancelRenameClick(s);
    if (OwnsView(tab, s.blankClickTab) || OwnsPane(tab, s.blankClickPane)) {
        s.blankClickPane = nullptr;
        s.blankClickTab = nullptr;
    }
    if (OwnsView(tab, s.blankDoubleTab) || OwnsPane(tab, s.blankDoublePane)) {
        s.blankDoublePane = nullptr;
        s.blankDoubleTab = nullptr;
        s.blankDoublePending = false;
    }
    if (OwnsView(tab, s.listTypeAheadTab)) s.listTypeAheadTab = nullptr;
    if (s.tabFlashKey == &tab) s.tabFlashKey = nullptr;
    for (const auto& pane : tab.panes) CancelContentSelection(s, *pane->ActiveTab());
}
}  // namespace

bool MoveLayoutTab(AppState& src, size_t index, AppState& dst, size_t insert_at) {
    auto& from = src.window_tabs;
    if (&src == &dst || index >= from.items.size() || from.items.size() < 2 || !from.items[index])
        return false;
    ReleaseLeavingTab(src, *from.items[index], index == from.active);
    RememberLayoutFocus(src);  // the tab carries its focused and target pane
    std::unique_ptr<app::LayoutTab> tab = from.TakeTab(index);
    if (!tab) return false;
    BindCurrentLayout(src);
    RevalidateVisibleFolders(src);
    InvalidateRect(src.hwnd, nullptr, FALSE);

    if (dst.addressSearching) HideAddressEditor(dst, false);
    RememberLayoutFocus(dst);
    dst.window_tabs.InsertTab(std::move(tab), insert_at);
    BindCurrentLayout(dst);  // pane, target pane and scroll position follow the tab
    RevalidateVisibleFolders(dst);
    InvalidateRect(dst.hwnd, nullptr, FALSE);
    return true;
}
}  // namespace pulse
