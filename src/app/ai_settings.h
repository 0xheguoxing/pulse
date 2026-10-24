// ai_settings.h - %LOCALAPPDATA%\Pulse\ai\ai.json: whether AI clients may
// connect over MCP, how each known client was approved, the folders where a
// client may create files without asking, and how long the activity log is
// kept. Read with the strict json_value parser; a file that does not parse
// is ignored (defaults: everything off) and never overwritten silently.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::ai {

// The broker's permission classes (ai_broker.h). Only Create may be covered
// by an always-allow rule; Change is never offered without confirmation.
enum class PermissionClass : uint8_t { Read, View, Create, Undo, Change };

enum class ClientGrant : uint8_t {
    Ask,       // not decided yet: the first connection asks
    Allowed,   // 允许
    Session,   // 只允许这次会话 (stored as Ask; kept in memory only)
    Denied,    // 拒绝
};

struct ClientRecord {
    std::wstring name;
    ClientGrant grant = ClientGrant::Ask;
    uint64_t last_call = 0;   // Unix seconds
};

struct AllowRule {
    std::wstring client;   // MCP client name from hello
    std::string tool;      // e.g. "pulse.create_zip"
    std::wstring folder;   // absolute; covers its subfolders
};

// Pulse 助手 (ai_assistant.h): where its panel sits and the model last used.
struct AssistantPrefs {
    bool floating = false;                    // "dock": "docked" | "floating"
    std::string model = "deepseek-flash";
    int effort = 2;                           // 0 off, 1 low, 2 medium, 3 high
    std::string base_url;                     // 管理连接 · 高级; empty: https://api.deepseek.com
};

struct Settings {
    bool mcp_enabled = false;
    int log_days = 30;     // 1..365
    std::vector<ClientRecord> clients;
    std::vector<AllowRule> allow;
    AssistantPrefs assistant;
};

std::wstring AiDirectory();          // %LOCALAPPDATA%\Pulse\ai, empty without LOCALAPPDATA
std::wstring SettingsPath();         // ...\ai.json
std::wstring ActivityDirectory();    // ...\activity

// Missing file: defaults. A file that exists but does not parse: defaults
// and *malformed = true.
Settings LoadSettings(bool* malformed = nullptr);
Settings ParseSettings(std::string_view utf8, bool* malformed = nullptr);
bool SaveSettings(const Settings& settings);
std::string SerializeSettings(const Settings& settings);

// True when a rule lets `client` run `tool` in `folder` without asking.
// Always false for any class but Create.
bool IsAlwaysAllowed(const Settings& settings, PermissionClass cls, std::wstring_view client,
                     std::string_view tool, std::wstring_view folder);

} // namespace pulse::app::ai
