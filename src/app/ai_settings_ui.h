// ai_settings_ui.h - Settings > 增强包 > AI 与 MCP (page 9), the AI chip in
// the status bar and the notifications for what AI clients changed: glue
// between AppState, ai_service.h / ai_status.h and the renderer's views.
#pragma once

#include "ai_broker.h"
#include <windows.h>

namespace pulse {
struct AppState;
namespace ui { struct WindowViewModel; }

// BuildVm, every window: the status bar's AI chip (hidden unless a client is
// connected or something just ran).
void FillAiStatusVm(ui::WindowViewModel& vm);
// BuildVm on page 9: the page view (ai.json is re-read at most every second).
void FillAiSettingsVm(AppState& s, ui::WindowViewModel& vm);
// A click on page 9 (HitTestResult::SettingsAi index, ui::AiHit).
void AiSettingsAction(AppState& s, int index);
// A click on the chip: the open question comes forward, else page 9 opens.
void AiChipClicked(AppState& s);
// UI thread, after a call that changed files: 「Claude 新建了 x.zip」 with 撤销
// and 打开位置; after undo_last, what it undid.
void NoteAiChange(AppState& s, const app::ai::BrokerEvent& event);
// WM_AI_SNACK_UNDO / WM_AI_SNACK_OPEN from that notification.
void HandleAiSnackMessage(AppState& s, UINT message);
// {"mcpServers":{"pulse":{...}}} for this pulse.exe (Claude Desktop / Code).
std::wstring AiMcpConfigText();
// --shot-ai: made-up clients, rules and calls instead of ai.json and the
// broker, so shots never show the user's own. 0 busy, 1 waiting, 2 done.
void StageAiShot(int variant);

} // namespace pulse
