// app_prompts.h — Yes/no questions asked from app code, in the Pulse confirm dialog.
#pragma once

#include <cstddef>

namespace pulse {
struct AppState;

bool ConfirmClearRecent(AppState& s);
bool ConfirmClearDiagnostics(AppState& s);
// Privacy notice, then whether to include the index service. False = cancelled.
bool ConfirmDiagnosticsExport(AppState& s, bool& include_service, bool& include_dumps);
// True = retry the recoverable operations, false = discard them.
bool AskRetryRecovery(AppState& s, size_t count, bool uncertain_destructive, bool duplicate_cleanup = false);
// Where unscoped content searches look (content_scope_mode.h): 1 the user
// folders (recommended), 2 all drives, 0 = closed without choosing.
int AskContentScope(AppState& s);
} // namespace pulse
