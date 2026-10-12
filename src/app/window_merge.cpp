// window_merge.cpp - Multi-window M6c: merges a whole window into another one
// and undoes that.
#include "window_merge.h"

#include "app_internal.h"
#include "../common/localization.h"
#include "app_messages.h"
#include "tab_transfer.h"
#include "window_factory.h"
#include "window_placement.h"
#include "window_registry.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace pulse {
namespace {
int IndexOf(const app::WindowTabs& tabs, const app::LayoutTab* tab) {
    for (size_t i = 0; i < tabs.items.size(); ++i)
        if (tabs.items[i].get() == tab) return static_cast<int>(i);
    return -1;
}

void Activate(AppState& s, const app::LayoutTab* tab) {
    const int index = IndexOf(s.window_tabs, tab);
    if (index < 0) return;
    RememberLayoutFocus(s);
    s.window_tabs.SwitchTab(static_cast<size_t>(index));
    BindCurrentLayout(s);
    RevalidateVisibleFolders(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
}  // namespace

WindowMergeUndo MergeTabsInto(AppState& src, AppState& dst, size_t insert_at) {
    WindowMergeUndo undo;
    app::WindowTabs& from = src.window_tabs;
    app::WindowTabs& to = dst.window_tabs;
    undo.tab_groups = from.tab_groups;
    undo.active = from.active;
    for (const auto& tab : from.items) {
        undo.tabs.push_back(tab.get());
        undo.groups.push_back(tab->tab_group);
    }
    std::vector<std::pair<int, int>> ids;  // src group id -> its id in dst
    for (const app::TabGroup& group : from.tab_groups) {
        app::TabGroup copy = group;
        copy.id = to.next_tab_group_id++;
        to.tab_groups.push_back(copy);
        ids.emplace_back(group.id, copy.id);
    }

    // Pinned and unpinned tabs each keep their order from one slot.
    size_t pinned_at = 0;
    while (pinned_at < to.items.size() && to.items[pinned_at]->pinned) ++pinned_at;
    size_t loose_at = std::clamp(insert_at, pinned_at, to.items.size());
    pinned_at = (std::min)(insert_at, pinned_at);
    while (loose_at > 0 && loose_at < to.items.size() && to.items[loose_at - 1]->tab_group != 0 &&
           to.items[loose_at - 1]->tab_group == to.items[loose_at]->tab_group)
        ++loose_at;
    for (size_t k = 0; k < undo.tabs.size() && !from.items.empty(); ++k) {
        const bool pinned = from.items[0]->pinned;
        if (!MoveLayoutTab(src, 0, dst, pinned ? pinned_at : loose_at, TransferSource::Closing)) break;
        const int at = IndexOf(to, undo.tabs[k]);
        if (at < 0) break;
        for (const auto& [old_id, new_id] : ids)
            if (old_id == undo.groups[k]) to.items[static_cast<size_t>(at)]->tab_group = new_id;
        if (pinned) ++pinned_at;
        ++loose_at;
    }
    std::erase_if(to.tab_groups, [&](const app::TabGroup& group) {
        const bool merged = std::any_of(ids.begin(), ids.end(), [&](const auto& id) { return id.second == group.id; });
        const bool used = std::any_of(to.items.begin(), to.items.end(),
                                      [&](const auto& tab) { return tab->tab_group == group.id; });
        return merged && !used;
    });
    app::NormalizeGroupRuns(to);
    if (undo.active < undo.tabs.size()) Activate(dst, undo.tabs[undo.active]);
    return undo;
}

size_t RestoreMergedTabs(AppState& from, AppState& back, const WindowMergeUndo& undo) {
    app::WindowTabs& to = back.window_tabs;
    for (const app::LayoutTab* tab : undo.tabs) {
        const int index = IndexOf(from.window_tabs, tab);
        if (index >= 0 && IndexOf(to, tab) < 0)
            MoveLayoutTab(from, static_cast<size_t>(index), back, to.items.size());
    }
    std::vector<int> restored_groups;
    for (const app::TabGroup& group : undo.tab_groups) {
        const bool taken = std::any_of(to.tab_groups.begin(), to.tab_groups.end(),
                                       [&](const app::TabGroup& g) { return g.id == group.id; });
        bool used = false;
        for (size_t k = 0; k < undo.tabs.size(); ++k)
            used |= undo.groups[k] == group.id && IndexOf(to, undo.tabs[k]) >= 0;
        if (taken || !used) continue;
        to.tab_groups.push_back(group);
        to.next_tab_group_id = (std::max)(to.next_tab_group_id, group.id + 1);
        restored_groups.push_back(group.id);
    }
    size_t restored = 0;
    for (size_t k = 0; k < undo.tabs.size(); ++k) {
        const int at = IndexOf(to, undo.tabs[k]);
        if (at < 0) continue;
        ++restored;
        if (std::find(restored_groups.begin(), restored_groups.end(), undo.groups[k]) != restored_groups.end())
            to.items[static_cast<size_t>(at)]->tab_group = undo.groups[k];
    }
    app::NormalizeGroupRuns(to);
    if (undo.active < undo.tabs.size()) Activate(back, undo.tabs[undo.active]);
    return restored;
}

namespace {

// Merges `src` into `dst` and destroys it; the undo remembers where it was.
// Nothing when it could not give up all its tabs.
std::optional<WindowMergeUndo> MergeOne(AppState& src, AppState& dst, size_t insert_at, const WINDOWPLACEMENT& before) {
    if (&src == &dst || src.window_tabs.items.empty()) return std::nullopt;
    app::ClosedWindow place;
    place.rect = app::RestoredWindowRect(before.rcNormalPosition);
    place.maximized = before.showCmd == SW_SHOWMAXIMIZED;
    place.layout = CaptureWindowLayout(src);
    place.identity = src.identity;  // M7c: undo brings back its name and color
    if (const app::LayoutTab* active = src.window_tabs.Active()) place.name = app::LayoutTabTitle(*active);

    const HWND hwnd = src.hwnd;
    ShowWindow(hwnd, SW_HIDE);  // it must not paint while its tabs leave
    WindowMergeUndo undo = MergeTabsInto(src, dst, insert_at);
    if (!src.window_tabs.items.empty()) {
        RestoreMergedTabs(dst, src, undo);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        return std::nullopt;
    }
    undo.place = std::move(place);
    DestroyWindow(hwnd);
    return undo;
}

void OfferMergeUndo(AppState& dst, std::vector<WindowMergeUndo> batch) {
    std::wstring title = l10n::Get(l10n::StringId::WindowMergedTitle), detail;
    if (batch.size() == 1) {
        detail = batch.front().place.name;
    } else {
        wchar_t buf[160]{};
        swprintf_s(buf, l10n::Get(l10n::StringId::WindowsMergedTitle).c_str(), static_cast<int>(batch.size()));
        title = buf;
    }
    dst.window_merge_undo = std::move(batch);
    dst.notification_toast.ShowSnack(dst.hwnd, ui::SnackIcon::Merge, std::move(title), std::move(detail),
                                     {{l10n::Get(l10n::StringId::SnackUndo), WM_UNDO_WINDOW_MERGE}});
}

// Opens one merged window again at its old place, with the tabs of it that
// are still in `dst`. Returns it, or nullptr.
HWND UndoOne(AppState& dst, const WindowMergeUndo& undo) {
    int first = -1;
    for (const app::LayoutTab* tab : undo.tabs)
        if ((first = IndexOf(dst.window_tabs, tab)) >= 0) break;
    if (first < 0 || dst.window_tabs.items.size() < 2) return nullptr;

    // The window opens at its old place with its layout, adopting the first
    // merged tab still here (M5c); the others follow alive.
    WindowInit init;
    init.restore = undo.place;
    if (const app::Tab* view = dst.window_tabs.items[static_cast<size_t>(first)]->ActiveFolder())
        init.path = view->current_path;
    init.adopt_from = &dst;
    init.adopt_index = static_cast<size_t>(first);
    const HWND hwnd = OpenPulseWindow(dst, init);
    if (!hwnd) return nullptr;
    AppState& back = app::FindWindowSharingCore(dst, [hwnd](const AppState& w) { return w.hwnd == hwnd; });
    if (back.hwnd != hwnd) return nullptr;
    RestoreMergedTabs(dst, back, undo);
    return hwnd;
}

}  // namespace

bool MergeWindowInto(AppState& src, AppState& dst, size_t insert_at, const WINDOWPLACEMENT& before) {
    std::optional<WindowMergeUndo> undo = MergeOne(src, dst, insert_at, before);
    if (!undo) return false;
    std::vector<WindowMergeUndo> batch;
    batch.push_back(std::move(*undo));
    OfferMergeUndo(dst, std::move(batch));
    SetForegroundWindow(dst.hwnd);
    return true;
}

size_t MergeWindowsInto(const std::vector<HWND>& sources, AppState& dst) {
    std::vector<WindowMergeUndo> batch;
    for (HWND hwnd : sources) {
        AppState* src = dst.core.windows.Lookup(hwnd);
        if (!src || src == &dst) continue;
        WINDOWPLACEMENT before{sizeof(before)};
        GetWindowPlacement(hwnd, &before);
        if (std::optional<WindowMergeUndo> undo = MergeOne(*src, dst, dst.window_tabs.items.size(), before))
            batch.push_back(std::move(*undo));
    }
    const size_t merged = batch.size();
    if (merged) OfferMergeUndo(dst, std::move(batch));
    return merged;
}

void UndoWindowMerge(AppState& dst) {
    if (dst.window_merge_undo.empty()) return;
    const std::vector<WindowMergeUndo> batch = std::move(dst.window_merge_undo);
    dst.window_merge_undo.clear();
    // Last merged first, so each window finds its tabs and the stacking
    // order comes back; the first merged ends up in front.
    HWND front = nullptr;
    for (auto it = batch.rbegin(); it != batch.rend(); ++it)
        if (const HWND hwnd = UndoOne(dst, *it)) front = hwnd;
    if (front) SetForegroundWindow(front);
}
}  // namespace pulse
