// toolbar_tools.h — the tools pinned to the toolbar (AppPrefs::toolbar_tools),
// in toolbar order, saved by stable id. Labels, glyphs and commands live in
// the catalog in app_commands.cpp (kToolbarTools, same order as the ids here).
//
// Prefs before this list had a 5-bit mask, toolbar_pins: bit i pinned
// kToolbarToolIds[i]. It is still written, from the list, so an older Pulse
// keeps the pins it knows.
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// Saved in prefs: never rename or reorder. Append new tools at the end.
inline constexpr std::wstring_view kToolbarToolIds[] = {
    // The former toolbar_pins bits, in bit order.
    L"new_text", L"copy_path", L"terminal", L"properties", L"undo",
    // Toggles: each flips an existing setting (the toolbar shows it on).
    L"hidden", L"protected", L"compact", L"zebra", L"size_bar", L"smart_date",
    L"outline", L"sidebar", L"theme",
    // Toolbar picker T3d (the v3 prototype's whole catalog): the commands that
    // were fixed buttons (cut .. delete, split / details / columns), then the
    // rest of the prototype's tools Pulse has commands for.
    L"cut", L"copy", L"paste", L"rename", L"delete", L"split", L"details", L"columns",
    L"select_all", L"invert", L"select_wild", L"new_md", L"recent", L"apply_all",
    L"search_all", L"adv_search", L"recycle", L"map_drive", L"shortcuts", L"settings",
};
inline constexpr size_t kToolbarToolCount = std::size(kToolbarToolIds);
inline constexpr size_t kLegacyToolbarPins = 5;
// Every tool may be pinned; the toolbar folds what has no room into More.
// Matches ui::kToolbarCommandPins.
inline constexpr size_t kMaxToolbarTools = kToolbarToolCount;
// toolbar_tools_v in app.json. Lists saved before 2 lack the former fixed
// buttons; UpgradeToolbarTools adds them back where they used to be.
inline constexpr int kToolbarToolsVersion = 2;

inline int ToolbarToolIndex(std::wstring_view id) noexcept {
    for (size_t i = 0; i < kToolbarToolCount; ++i)
        if (kToolbarToolIds[i] == id) return static_cast<int>(i);
    return -1;
}

// Known ids only, each once, at most kMaxToolbarTools, order kept.
inline std::vector<std::wstring> NormalizeToolbarTools(const std::vector<std::wstring>& tools) {
    std::vector<std::wstring> out;
    for (const auto& id : tools) {
        if (out.size() >= kMaxToolbarTools) break;
        if (ToolbarToolIndex(id) < 0) continue;
        bool seen = false;
        for (const auto& kept : out) seen = seen || kept == id;
        if (!seen) out.push_back(id);
    }
    return out;
}

// The prototype's default toolbar (a fresh install).
inline std::vector<std::wstring> DefaultToolbarTools() {
    return {L"cut", L"copy", L"paste", L"rename", L"delete", L"hidden", L"split", L"details", L"columns"};
}

// A list saved before kToolbarToolsVersion: cut .. delete were fixed buttons
// ahead of the pins and split / details / columns sat after them, so they
// come back there and the toolbar looks as before.
inline std::vector<std::wstring> UpgradeToolbarTools(const std::vector<std::wstring>& tools) {
    const auto has = [&](std::wstring_view id) {
        for (const auto& t : tools) if (t == id) return true;
        return false;
    };
    std::vector<std::wstring> out;
    for (std::wstring_view id : {L"cut", L"copy", L"paste", L"rename", L"delete"})
        if (!has(id)) out.emplace_back(id);
    out.insert(out.end(), tools.begin(), tools.end());
    for (std::wstring_view id : {L"split", L"details", L"columns"})
        if (!has(id)) out.emplace_back(id);
    return NormalizeToolbarTools(out);
}

inline std::vector<std::wstring> ToolbarToolsFromLegacyPins(uint32_t pins) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < kLegacyToolbarPins; ++i)
        if (pins & (1u << i)) out.emplace_back(kToolbarToolIds[i]);
    return out;
}

inline uint32_t LegacyPinsFromToolbarTools(const std::vector<std::wstring>& tools) noexcept {
    uint32_t pins = 0;
    for (const auto& id : tools) {
        const int i = ToolbarToolIndex(id);
        if (i >= 0 && static_cast<size_t>(i) < kLegacyToolbarPins) pins |= 1u << i;
    }
    return pins;
}

} // namespace pulse::app
