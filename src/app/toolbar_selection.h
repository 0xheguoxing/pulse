// toolbar_selection.h — the actions the toolbar's selection chip may show
// (AppPrefs::toolbar_selection), in chip order, saved by stable id. Commands,
// glyphs and labels live in the catalog in app_commands.cpp
// (kSelectionActions, same order as the ids here).
//
// Prefs without the key show DefaultToolbarSelection.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// Saved in prefs: never rename or reorder. Append new actions at the end.
inline constexpr std::wstring_view kSelectionActionIds[] = {
    L"batch_rename", L"group_folder", L"dissolve_folder", L"compare", L"tags", L"copy_path",
    L"new_tab", L"open_path", L"quick_access", L"properties", L"terminal", L"recent_changes",
    L"pin_workspace", L"pin_network",
};
inline constexpr size_t kSelectionActionCount = std::size(kSelectionActionIds);

inline int SelectionActionIndex(std::wstring_view id) noexcept {
    for (size_t i = 0; i < kSelectionActionCount; ++i)
        if (kSelectionActionIds[i] == id) return static_cast<int>(i);
    return -1;
}

// Known ids only, each once, order kept.
inline std::vector<std::wstring> NormalizeToolbarSelection(const std::vector<std::wstring>& actions) {
    std::vector<std::wstring> out;
    for (const auto& id : actions) {
        if (SelectionActionIndex(id) < 0) continue;
        bool seen = false;
        for (const auto& kept : out) seen = seen || kept == id;
        if (!seen) out.push_back(id);
    }
    return out;
}

// The situational rules (batch rename, compare, open path) are on; the
// user's own actions are 标签, 复制路径, 在新标签页打开, 属性 (toolbar
// redesign §5). Pinning moved to the chip's 固定到… menu.
inline std::vector<std::wstring> DefaultToolbarSelection() {
    return {L"batch_rename", L"compare", L"open_path", L"tags", L"copy_path", L"new_tab", L"properties"};
}

} // namespace pulse::app
