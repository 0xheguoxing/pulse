#pragma once
#include "app_model.h"
#include "../fs/fs_enum.h"
#include <unordered_set>
#include <unordered_map>
#include <utility>

namespace pulse::app {
inline void CapturePendingListingSelection(Tab& tab) {
    tab.pending_selection_revision = tab.selection_revision;
    tab.pending_selected_names.clear();
    tab.pending_selected_name.clear();
    tab.pending_ensure_selection_visible = false;
    if (!tab.snapshot || tab.SelectedCount() <= 0) return;
    for (int index : tab.SelectedIndices()) {
        if (index >= 0 && index < static_cast<int>(tab.EntryCount()))
            tab.pending_selected_names.push_back(tab.EntryAt(static_cast<size_t>(index)).name);
    }
    if (tab.selected_index >= 0 && tab.selected_index < static_cast<int>(tab.EntryCount()))
        tab.pending_selected_name = tab.EntryAt(static_cast<size_t>(tab.selected_index)).name;
}

struct ListingSelectionRestore {
    std::vector<std::wstring> names;
    std::wstring focus;
    bool ensure_visible = false;
    bool user_changed = false;
    fs::SnapshotPtr previous_snapshot;
    int previous_focus = -1;
};

// Called after request-generation validation and before replacing the old snapshot.
inline ListingSelectionRestore TakeListingSelection(Tab& tab, const std::wstring& result_path) {
    const bool changed = tab.pending_selection_revision != UINT64_MAX &&
        tab.pending_selection_revision != tab.selection_revision && tab.snapshot &&
        tab.snapshot_path == result_path && !fs::IsVirtualPath(result_path);
    if (changed) CapturePendingListingSelection(tab);
    ListingSelectionRestore restore{
        std::exchange(tab.pending_selected_names, {}),
        std::exchange(tab.pending_selected_name, {}),
        std::exchange(tab.pending_ensure_selection_visible, false), changed};
    if (!changed && !tab.content_results && tab.snapshot && tab.snapshot_path == result_path &&
        !fs::IsVirtualPath(result_path) && tab.selected_index >= 0 &&
        static_cast<size_t>(tab.selected_index) < tab.snapshot->size() &&
        (*tab.snapshot)[static_cast<size_t>(tab.selected_index)].name == restore.focus) {
        restore.previous_snapshot = tab.snapshot;
        restore.previous_focus = tab.selected_index;
    }
    tab.pending_selection_revision = UINT64_MAX;
    return restore;
}

inline void RestoreListingSelection(Tab& tab, const ListingSelectionRestore& restore,
                                    const Pane* pane = nullptr, const PlacesCatalog* places = nullptr) {
    const auto& names = restore.names;
    const auto& focus = restore.focus;
    if (restore.user_changed) {
        const std::unordered_set<std::wstring> wanted(names.begin(), names.end());
        bool survives = false;
        for (size_t i = 0; i < tab.EntryCount(); ++i) {
            if (!tab.EntryVisible(static_cast<int>(i))) continue;
            const auto name = tab.EntryAt(i).name;
            if (wanted.contains(name)) {
                survives = true;
                break;
            }
        }
        if (!survives) { tab.ClearSelection(); return; }
    }
    if (!names.empty()) {
        tab.RemapSelection(names, focus, false);
        if (tab.SelectedCount() > 0) return;
        if (restore.previous_snapshot && restore.previous_focus >= 0 && tab.snapshot) {
            // Keep the nearest surviving identity in the old displayed order,
            // rather than an index shifted by a multi-file deletion.
            std::vector<int> visible;
            if (pane) {
                ui::PaneViewModel view;
                FillPaneViewModel(view, *pane, places);
                const auto* groups = view.Groups();
                size_t group = 0;
                for (int row = 0; row < static_cast<int>(view.EntryCount()); ++row) {
                    if (groups) {
                        while (group < groups->size() && row >= (*groups)[group].first + (*groups)[group].count) ++group;
                        if (group < groups->size() && (*groups)[group].collapsed) continue;
                    }
                    visible.push_back(view.SourceIndex(row));
                }
            } else {
                CollectFilterMatches(tab, places, visible);
            }
            std::unordered_map<std::wstring_view, int> surviving;
            surviving.reserve(visible.size());
            for (int index : visible) {
                if (tab.EntryVisible(index) && static_cast<size_t>(index) < tab.snapshot->size())
                    surviving.emplace((*tab.snapshot)[static_cast<size_t>(index)].name, index);
            }
            const auto select = [&](int old_index) {
                const auto found = surviving.find((*restore.previous_snapshot)[static_cast<size_t>(old_index)].name);
                if (found == surviving.end()) return false;
                tab.SelectOnly(found->second);
                return true;
            };
            for (int i = restore.previous_focus + 1; i < static_cast<int>(restore.previous_snapshot->size()); ++i)
                if (select(i)) return;
            for (int i = restore.previous_focus - 1; i >= 0; --i)
                if (select(i)) return;
            if (!visible.empty()) tab.SelectOnly(visible.front());
            return;
        }
        if (tab.EntryCount() != 0) tab.FocusFirstEntry();
    } else if (tab.snapshot && tab.EntryCount() != 0) tab.FocusFirstEntry();
    else tab.ClearSelection();
}
} // namespace pulse::app
