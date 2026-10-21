// shell_window_sync.h — Keeps ShellWindowRegistry in step with Pulse's panes
// and carries out the shell's "select this item" requests (B站 #1 phase 2a).
#pragma once
#include "explorer_window_takeover.h"
#include "shell_window_plan.h"

namespace pulse {
struct AppState;
namespace app { struct Tab; }

// After each frame: publishes the panes' folders when they changed (while
// Pulse opens folders) and starts/stops the experimental Explorer window
// takeover with its setting. Never in shot or test runs.
void SyncShellWindows(AppState& s);
// WM_DESTROY: revokes every registration, stops the takeover.
void StopShellWindows(AppState& s);
// WM_SHELL_SELECT: brings the pane forward and selects the item.
void HandleShellSelect(AppState& s, const app::ShellSelectRequest& request);
// WM_EXPLORER_TAKEOVER: opens the source folder, then acknowledges the actual load.
void HandleExplorerTakeover(AppState& s, const app::ExplorerTakeoverRequest& request);
void CompleteExplorerNavigation(app::Tab& tab, uint64_t generation, bool success);
// Adds `name` to the tab's selection now when listed, else once its listing
// lands (a program selecting several files; AI reveal, ai_tools_ui.h).
void AddNameToSelection(AppState& s, app::Tab& tab, const std::wstring& name);

} // namespace pulse