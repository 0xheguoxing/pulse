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
};
inline constexpr size_t kToolbarToolCount = std::size(kToolbarToolIds);
inline constexpr size_t kLegacyToolbarPins = 5;
// Room on the toolbar; matches ui::kToolbarCommandPins.
inline constexpr size_t kMaxToolbarTools = 12;

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
