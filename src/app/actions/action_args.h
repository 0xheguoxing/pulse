// action_args.h - Actions run with arguments (全面打通 P4,
// plans/ai/P4_全部领域接入.html).
//
// The palette runs an action as its menu row does; the assistant (and MCP in
// P5) names what to act on: which tab, which tag, what name, which keys. A
// runner here does that one action with those arguments and says what
// changed, in the words of the result card, and how to undo it. Runners only
// call what the menus and the settings page call, and never ask: the gate
// (action_policy.h) decides before a runner is reached.
#pragma once

#include "../../common/json_value.h"

#include <functional>
#include <vector>
#include <string>
#include <string_view>

namespace pulse {
struct AppState;
}

namespace pulse::app::actions {

struct ArgRun {
    bool ok = false;
    bool skip = false;           // nothing to do: it was so already
    std::string error;           // for the model (English)
    std::wstring why;            // !ok: the step card's reason
    std::wstring arg;            // the step's chip (报销, 3 个)
    std::wstring sub;            // the step's second line (蓝色 · 在「工作」分组)
    // The result card's line: before + bold + after (empty: the title).
    std::wstring line_before, line_bold, line_after;
    // Key caps for the line (keymap.set): from -> to.
    std::vector<std::wstring> caps_from, caps_to;
    bool read_only = false;      // read rows: no line, no undo
    // Restores what the action changed, if it is still as the action left it.
    std::function<void(AppState&)> undo;
    json::Value data;            // extra result fields for the model
};

// True when `id` has a runner here (the assistant then uses it, not the
// plain menu row).
bool HasArgRunner(std::string_view id) noexcept;
ArgRun RunWithArgs(AppState& s, std::string_view id, const json::Value& args);

// Color words the runners take: red orange yellow green teal blue purple
// pink none; index into TabController::Palette(), -1 none, -2 unknown.
int ColorIndex(std::string_view word) noexcept;
std::wstring ColorName(int index);

} // namespace pulse::app::actions
