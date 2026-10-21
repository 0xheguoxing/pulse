// ai_settings_view.h - what Settings > 增强包 > AI 与 MCP (page 9) draws.
// Built by app/ai_settings_ui.cpp on the UI thread from ai.json and
// ai_status.h; immutable once published, shared by the view models.
#pragma once

#include <d2d1.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {

// HitTestResult::SettingsAi indices.
enum AiHit : int {
    kAiToggle = 0,            // 允许 AI 应用连接
    kAiCopyConfig = 1,        // 复制配置
    kAiOpenLog = 2,           // 打开活动记录
    kAiUninstall = 3,         // 卸载 AI 增强包
    kAiClientAllowBase = 100,   // + client row: 允许 (always)
    kAiClientDenyBase = 200,    // + client row: 拒绝 (and disconnect)
    kAiClientForgetBase = 300,  // + client row: 移除
    kAiRuleRemoveBase = 400,    // + rule row: 移除
    kAiRowLimit = 99,           // rows per list that get buttons
};

// app::ai::ClientGrant values, plus "allowed until Pulse quits".
enum class AiGrantView : uint8_t { Ask, Allowed, Session, Denied };

struct AiClientRowView {
    std::wstring name;
    AiGrantView grant = AiGrantView::Ask;
    bool connected = false;
};

struct AiRuleRowView {
    std::wstring client;
    std::string tool;          // "pulse.create_zip" (what RemoveAiAllowRule needs)
    std::wstring tool_label;   // 新建压缩包
    std::wstring folder;
};

// Tone of a call's outcome text.
enum class AiCallTone : uint8_t { Done, Running, Declined, Failed };

struct AiCallRowView {
    std::wstring time;         // local HH:MM:SS
    std::wstring client;
    std::wstring tool_label;
    std::wstring outcome;
    AiCallTone tone = AiCallTone::Done;
};

struct AiPageView {
    bool enabled = false;      // ai.json mcp_enabled
    bool listening = false;    // the broker runs
    bool broken = false;       // ai.json does not parse: nothing is written
    bool can_uninstall = false;  // an installed (not bundled) pack
    std::wstring status;       // under the switch
    std::vector<AiClientRowView> clients;
    std::vector<AiRuleRowView> rules;
    std::vector<AiCallRowView> calls;   // newest first, at most kAiPageCalls
    size_t more_calls = 0;              // older calls not listed
};

inline constexpr size_t kAiPageCalls = 12;

// Geometry of page 9 (LayoutSettingsAi in settings_ai_layout.h).
struct AiClientRowLayout {
    D2D1_RECT_F row{}, allow{}, deny{}, forget{};
};
struct AiPageLayout {
    D2D1_RECT_F main_card{}, enable_row{}, toggle{}, setup_row{}, copy{}, broken{};
    D2D1_RECT_F apps_section{}, apps_card{}, apps_empty{};
    std::vector<AiClientRowLayout> clients;
    D2D1_RECT_F rules_section{}, rules_card{}, rules_empty{};
    std::vector<D2D1_RECT_F> rules, rule_remove;
    D2D1_RECT_F recent_section{}, open_log{}, recent_card{}, recent_empty{}, more_calls{};
    std::vector<D2D1_RECT_F> calls;
    D2D1_RECT_F pack_section{}, uninstall_row{}, uninstall{};
};

} // namespace pulse::ui
