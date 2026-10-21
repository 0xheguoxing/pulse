// ai_confirm.h - The questions Pulse asks for AI clients: may this client
// connect, and may it create this file. Asked with Pulse's confirm dialog on
// the UI thread (WM_AI_UI_TASK), one question at a time; the asking broker
// thread waits and gives up on a timeout, a cancelled call or the service
// stopping - never waiting on the UI thread itself, which may be the one
// stopping the service. A question given up on closes its dialog (WM_CLOSE).
#pragma once
#include "ai_broker.h"
#include "../ui/confirm_dialog.h"
#include <atomic>
#include <string>
#include <vector>
#include <windows.h>

namespace pulse::app::ai {

enum class ConnectAnswer { Allow, Session, Deny };
enum class CreateAnswer { Allow, AlwaysHere, Deny, Timeout };

// No window to ask in, `stopping`, or the timeout: Deny.
ConnectAnswer AskToConnect(HWND host, const std::wstring& client, const std::atomic<bool>& stopping, DWORD timeout_ms);
// `cancelled` (the call) or `stopping`: Deny; the broker reports the cancel itself.
CreateAnswer AskToCreate(HWND host, const ConfirmRequest& request, const std::atomic<bool>& cancelled,
                         const std::atomic<bool>& stopping, DWORD timeout_ms);

// UI thread (the status-bar chip): brings the open question's dialog forward.
// False when none is open.
bool BringAiQuestionToFront();

// The dialogs' contents; also drawn by the dialog gallery.
ui::ConfirmDialogSpec ConnectQuestionSpec(const std::wstring& client);
// `names`: the items (at most five are listed), `total`: how many there are.
ui::ConfirmDialogSpec CreateZipQuestionSpec(const std::wstring& client, const std::vector<std::wstring>& names,
                                            size_t total, const std::wstring& archive_name,
                                            const std::wstring& folder);

} // namespace pulse::app::ai
