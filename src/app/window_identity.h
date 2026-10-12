#pragma once
// window_identity.h - Multi-window M7c (plan section 8.3): each window's name
// and color. Pure rules here; drawing, menus and the taskbar are in
// window_identity_ui.h. Nothing of it shows while only one window is open.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

// The six window colors (0xRRGGBB), as in the prototype.
inline constexpr uint32_t kWindowColors[] = {0x4cc2ff, 0xf5a65b, 0x7fd88f, 0xc9a3ff, 0xff7fa3, 0xe6cc5c};
inline constexpr int kWindowColorCount = static_cast<int>(sizeof(kWindowColors) / sizeof(kWindowColors[0]));
inline constexpr size_t kWindowNameMaxLength = 48;

struct WindowIdentity {
    std::wstring name;  // empty: named after its folders (AutoNameFolder)
    int color = -1;     // index into kWindowColors; -1: not given one yet
    bool operator==(const WindowIdentity&) const = default;
};

inline bool ShowsWindowIdentity(size_t window_count) { return window_count >= 2; }
inline bool ValidWindowColor(int color) { return color >= 0 && color < kWindowColorCount; }

// The folder a window is named after, from the paths its visible panes show:
// their common folder unless that is a drive root shared by different paths;
// otherwise the deepest non-root folder holding the most of them (at least
// two), so one stray tab elsewhere does not rename the window. Pulse places
// (pulse:..., This PC "") take no part. "" when nothing qualifies: the caller
// names the window after its active tab.
std::wstring AutoNameFolder(const std::vector<std::wstring>& pane_paths);

// The color used by the fewest of `taken` (the other windows' colors; -1
// entries ignored), the lowest index among equals.
int PickWindowColor(const std::vector<int>& taken);

// A typed name: trimmed, control characters dropped, at most
// kWindowNameMaxLength characters. "" means back to the automatic name.
std::wstring NormalizeWindowName(std::wstring_view typed);

}  // namespace pulse::app
