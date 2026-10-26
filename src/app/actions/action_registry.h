// action_registry.h - What each action is and does right now (全面打通 P1).
//
// The command palette lists these; the assistant (P3) and MCP (P5) will read
// the same list. An action runs what its menu row or key runs, so a palette
// pick behaves exactly like the menu: the user's own settings (删除前确认)
// apply, the assistant's confirm rules do not. Every id is an inventory id
// (action_inventory.h); rows sharing one (the settings) differ by `key`.
#pragma once

#include "../../ui/fluent_menu.h"

#include <string>
#include <string_view>
#include <vector>

namespace pulse {
struct AppState;
}

namespace pulse::app::actions {

// The palette's command ids, clear of every MenuCmd range and kCmdGit*.
inline constexpr int kPaletteAskAssistant = 39991;  // 交给 Pulse 助手
inline constexpr int kPaletteCmdBase = 40000;       // + action * stride + choice + 1
inline constexpr int kPaletteStride = 32;
inline constexpr int kPaletteMaxActions = 512;
inline constexpr int kPaletteCmdEnd = kPaletteCmdBase + kPaletteMaxActions * kPaletteStride;

constexpr int PaletteCommand(int action, int choice = -1) noexcept {
    return kPaletteCmdBase + action * kPaletteStride + choice + 1;
}

struct ActionView {
    int index = -1;               // RunAction's handle (stable within a build)
    std::string_view id;          // inventory id
    std::string_view key;         // a setting's id; empty for the rest
    std::wstring title;
    std::wstring domain;          // the palette's group heading
    std::wstring glyph;
    ui::fluent::MenuPictogram pictogram = ui::fluent::MenuPictogram::None;
    std::wstring shortcut;        // the key that runs it now (the user's keymap)
    std::wstring value;           // its current value (名称 ↑, 开), if it has one
    bool enabled = true;
    std::wstring why;             // !enabled: what is missing (先选中一个文件)
    // Non-empty: pick one. A row with a command runs that menu command; a
    // row with command 0 runs RunAction(index, its position).
    std::vector<ui::FluentMenuItem> choices;
};

// Every action the palette offers, as of now.
std::vector<ActionView> ListActions(AppState& s);
// Runs action `index`; choice -1 runs the action itself. False when the
// action is unknown or cannot run now.
bool RunAction(AppState& s, int index, int choice = -1);
// The action whose choice rows hold menu command `command`, or -1.
int ActionOfMenuCommand(AppState& s, int command);

// ---- The palette's rows (pure; the selftests feed made-up views) ----------
enum class PaletteMode {
    Commands,  // ">": every match, grouped under headings
    Mixed,     // commands before files: the best three, no headings
};
// The rows for `needle` (already without ">"). `used` holds the actions run
// this session, newest first. Commands mode with an empty needle lists the
// used ones and the fixed 常用 rows.
std::vector<ui::FluentMenuItem> PaletteRows(const std::vector<ActionView>& views,
                                            std::wstring_view needle, PaletteMode mode,
                                            const std::vector<int>& used);
// The view's palette row (its choices renumbered to palette commands).
ui::FluentMenuItem PaletteRow(const ActionView& view);

} // namespace pulse::app::actions
