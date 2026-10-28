// mcp_tools.h - The tools Pulse.AI.exe offers to MCP clients (first batch,
// see the plan §5.2): names, descriptions for the model, input schemas and
// argument checks. The work itself happens in pulse.exe behind the broker,
// which checks everything again; this side only keeps malformed calls from
// reaching it and gives the model a precise reason it can correct.
//
// MCP names have no dots (some clients only accept [A-Za-z0-9_-]); the
// server is called "pulse", so clients show them as pulse / list_folder etc.
// The broker uses "pulse.<name>".
#pragma once
#include "../common/json_value.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ai_host {

// Steps: run_actions' list of {action, args?, wait?} objects.
enum class ParamKind : uint8_t { String, Integer, StringArray, Date, Steps };

struct Param {
    const char* name;
    ParamKind kind;
    bool required;
    int64_t minimum;          // String/Date: min length; Integer: min value; StringArray: min items
    int64_t maximum;          // ... max length / value / items
    const char* description;
    std::vector<const char*> choices = {};   // String: allowed values (enum)
};

struct ToolDef {
    const char* name;          // MCP name
    const char* title;
    const char* description;
    bool read_only;
    bool destructive;
    bool idempotent;
    std::vector<Param> params;
    std::string broker_name() const { return std::string("pulse.") + name; }
};

const std::vector<ToolDef>& Tools();
const ToolDef* FindTool(std::string_view name);

// {"tools":[...]} in a fixed order.
json::Value ToolsList();
json::Value InputSchema(const ToolDef& tool);

// Empty when `args` (an object) fits the tool; otherwise a short English
// reason naming the argument. Unknown members are refused.
std::string ValidateArguments(const ToolDef& tool, const json::Value& args);

} // namespace pulse::ai_host
