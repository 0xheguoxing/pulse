// action_policy.h - What the assistant may do with an action, and how
// (plans/ai/P3_AI接入动作表.html §2). MCP (P5) reads the same answer.
//
//   Direct   Read / View / Organize / Create: runs at once; the card shows
//            old -> new and undoes the call as one group
//   Confirm  External (programs, scripts) and System: a confirm card first,
//            every time (never "always allow")
//   Later    Move / Delete / PermanentDelete: not through run_actions yet
//            (P4); the flow tools with their own plan cards do these
//   Locked   UserOnly and the assistant's own limits: Pulse shows the user
//            why and where to change it themselves
#pragma once

#include "action_inventory.h"

#include <cstdint>
#include <string_view>

namespace pulse::app::actions {

enum class Gate : uint8_t { Direct, Confirm, Later, Locked };

struct Policy {
    Gate gate = Gate::Locked;
    ActionRisk risk = ActionRisk::UserOnly;
};

// Programs and scripts: .exe .bat .cmd .ps1 .vbs .msi and the like, and
// shortcuts (a shortcut may start anything). Documents and media are not.
bool IsProgramPath(std::wstring_view path) noexcept;

// The policy for running `id` on `target` (nav.open: the file it opens).
// Unknown ids are Locked. settings.set is Direct: a setting that changes
// Windows still runs at once and its card says so (third batch); the locked
// settings are refused by the settings code itself.
Policy AiPolicy(std::string_view id, std::wstring_view target = {}) noexcept;

const char* RiskName(ActionRisk risk) noexcept;   // "view", "organize", ...

} // namespace pulse::app::actions
