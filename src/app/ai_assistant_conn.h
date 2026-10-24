// ai_assistant_conn.h - 管理连接: the model menu's connection card
// (plans/ai/Pulse_AI助手_管理连接.html, form B). Owns the key and address
// EDITs, the 测试连接 probe and saving the key (DPAPI, ai_model_key.h) and
// the address (ai.json assistant.base_url). The menu stays open under it:
// ‹ goes back to the menu, ⌄ / Esc / a click outside fold both away.
#pragma once

#include "../ui/ai_panel_view.h"
#include <windows.h>

namespace pulse {
struct AppState;

void AiConnOpen(AppState& s);
bool AiConnIsOpen(const AppState& s);
// Back to the menu (also when the menu folds away). Forgets a typed key.
void AiConnClose(AppState& s);
// BuildVm: p.conn while the card is open.
void AiConnFill(AppState& s, ui::AiPanelViewModel& p);
// A click on the card. False: not the card's (the caller handles it).
// `close_menu`: 完成 - fold the menu away too.
bool AiConnActivate(AppState& s, const ui::AiPanelHit& hit, bool& close_menu);
// After every frame: the EDITs follow the drawn fields.
void AiConnLayout(AppState& s);
// The card's header and eye tips; nullptr for other parts.
bool AiConnTip(const AppState& s, ui::AiPanelPart part, std::wstring& text);
bool AiConnIsField(ui::AiPanelPart part);
HBRUSH AiConnCtlColor(AppState& s, HDC hdc, HWND edit);
void AiConnDpiChanged(AppState& s);
void AiConnDestroy(AppState& s);

} // namespace pulse
