#include "reopen_closed.h"
#include "app_internal.h"
#include "app_messages.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "closed_tabs.h"
#include "session.h"
#include "tray_reveal.h"
#include "window_registry.h"
#include "../common/localization.h"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>

namespace pulse {
namespace {

// The group a reopened tab joins: the same one when it is still there (it is
// shown again), else the group made again.
int GroupFor(app::WindowTabs& tabs, const app::GroupSessionSnapshot& saved) {
    for (app::TabGroup& group : tabs.tab_groups) {
        if (group.id == saved.id && group.name == saved.name && group.color_rgb == saved.color_rgb) {
            group.collapsed = false;
            return group.id;
        }
    }
    int id = saved.id;
    const bool taken = std::any_of(tabs.tab_groups.begin(), tabs.tab_groups.end(),
                                   [id](const app::TabGroup& group) { return group.id == id; });
    if (taken) id = tabs.next_tab_group_id++;
    else tabs.next_tab_group_id = std::max(tabs.next_tab_group_id, id + 1);
    tabs.tab_groups.push_back({id, saved.name, saved.color_rgb, false});
    return id;
}

// Puts the tabs back into `w`, each at its old place (in ascending order, so
// the places line up again), and shows the last of them.
void RestoreInto(AppState& w, const std::vector<app::ClosedTab>& batch) {
    RememberLayoutFocus(w);
    app::WindowTabs& tabs = w.window_tabs;
    AppState* const state = &w;
    const app::SessionTabLoader load = [state](app::Tab& tab, const std::wstring& path) {
        WarmupUnc(*state, path);
        StartLoadingPath(*state, tab, path, PathLoadReason::RestoreSession);
    };
    const app::LayoutTab* shown = nullptr;
    for (const app::ClosedTab& closed : batch) {
        auto tab = std::make_unique<app::LayoutTab>();
        app::RestoreLayoutTab(*tab, closed.tab, load);
        app::RebuildLayoutRoot(*tab);
        if (tab->root && !closed.tab.split_ratios.empty()) app::ApplySplitRatios(*tab->root, closed.tab.split_ratios);
        app::LayoutTab* raw = tab.get();
        tabs.InsertTab(std::move(tab), closed.index);  // clears its group
        if (closed.group.id > 0) raw->tab_group = GroupFor(tabs, closed.group);
        shown = raw;
    }
    app::NormalizeGroupRuns(tabs);
    for (size_t i = 0; i < tabs.items.size(); ++i)
        if (tabs.items[i].get() == shown) tabs.active = i;
    BindCurrentLayout(w);
    InvalidateRect(w.hwnd, nullptr, FALSE);
}

}  // namespace

void RememberClosingTabs(AppState& s, const std::vector<size_t>& indices) {
    const app::WindowTabs& tabs = s.window_tabs;
    std::vector<size_t> order = indices;
    std::sort(order.begin(), order.end(), std::greater<size_t>());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    // As WindowTabs::CloseTab does from the highest index down: pinned tabs
    // stay, and so does the last tab.
    const uint64_t seq = s.core.close_seq + 1;
    std::vector<app::ClosedTab> batch;
    size_t remaining = tabs.items.size();
    for (size_t index : order) {
        if (remaining <= 1) break;
        if (index >= tabs.items.size() || !tabs.items[index] || tabs.items[index]->pinned) continue;
        --remaining;
        const app::LayoutTab& layout = *tabs.items[index];
        if (IsSettingsTab(layout.ActiveFolder())) continue;  // Settings opens from its button
        app::ClosedTab closed;
        closed.tab = app::CaptureLayoutTab(layout);
        closed.index = index;
        closed.window = s.hwnd;
        closed.seq = seq;
        for (const app::TabGroup& group : tabs.tab_groups)
            if (group.id == layout.tab_group) closed.group = {group.id, group.name, group.color_rgb, group.collapsed};
        batch.push_back(std::move(closed));
    }
    if (batch.empty()) return;
    // An earlier "N tabs closed" toast would now bring these back instead.
    DismissReopenToasts(s, WM_REOPEN_CLOSED_TABS);
    std::reverse(batch.begin(), batch.end());  // old order: they go back lowest place first
    s.core.close_seq = seq;
    const size_t count = batch.size();
    s.core.closed_tabs.Push(std::move(batch));
    if (count < 2 || !s.hwnd) return;
    wchar_t title[96]{};
    swprintf_s(title, l10n::Get(l10n::StringId::TabsClosedTitle).c_str(), static_cast<int>(count));
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, title, {},
                                   {{l10n::Get(l10n::StringId::SnackRestore), WM_REOPEN_CLOSED_TABS}}, L"Ctrl+Shift+T");
}

void ReopenClosedTabs(AppState& s) {
    std::vector<app::ClosedTab> batch = s.core.closed_tabs.TakeLatestBatch();
    if (batch.empty()) return;
    DismissReopenToasts(s, WM_REOPEN_CLOSED_TABS);
    AppState* target = batch.front().window ? s.core.windows.Lookup(batch.front().window) : nullptr;
    if (!target) target = &s;
    RestoreInto(*target, batch);
    if (target != &s) RevealWindow(*target);
}

void DismissReopenToasts(AppState& s, unsigned message) {
    app::ForEachWindowSharingCore(s, [message](AppState& window) {
        if (window.hwnd) window.notification_toast.DismissFor(window.hwnd, message);
    });
}

bool ReopenLastClosed(AppState& s) {
    switch (app::NextToReopen(s.core.closed_tabs, s.core.closed_windows)) {
    case app::ReopenKind::Tabs:
        ReopenClosedTabs(s);
        return true;
    case app::ReopenKind::Window:
        // Posted: the window must not be created under the key handler.
        PostMessageW(s.hwnd, WM_REOPEN_CLOSED_WINDOW, 0, 0);
        return true;
    case app::ReopenKind::Nothing:
        break;
    }
    return false;
}

}  // namespace pulse
