// ai_tools_actions.h - The action table for AI apps (P5): pulse.find_actions
// and pulse.run_actions, registered by ai_tools.cpp. Which groups of actions
// may run is the user's choice in ai.json (mcp_allow); see ai_mcp_run.h.
#pragma once
#include "ai_broker.h"
#include "ai_mcp_run.h"
#include "ai_settings.h"
#include <functional>
#include <memory>
#include <windows.h>

namespace pulse::app::ai {

std::array<bool, actions::kMcpGroupCount> McpAllowedGroups(const Settings& settings);

// pulse.find_actions {query | domain: "settings"}: up to 8 rows (every
// setting for the domain), each with its group and whether AI apps may run it.
ToolOutcome FindActionsTool(HWND host, const ToolCall& call, const Settings& settings);

// pulse.run_actions {steps}: runs on the UI thread and waits for it, closing
// a question left open for 2 minutes. `add_rule` stores 始终允许这个文件夹.
// `finished` gets the run (its undo) once it is over.
ToolOutcome RunActionsTool(HWND host, const ToolCall& call, const Settings& settings,
                           std::function<void(AllowRule)> add_rule, std::shared_ptr<McpActionsRun>& finished);

} // namespace pulse::app::ai
