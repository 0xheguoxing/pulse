// ai_tools_ui.h - AI tools that read or show something in Pulse's windows.
//
// The broker's call thread posts a task to Pulse's host window
// (WM_AI_UI_TASK) and waits a few seconds for the UI thread to run it; a task
// that is cancelled or late is dropped unrun. The UI thread only reads state
// and starts navigation; anything touching the disk happens on the call
// thread first.
#pragma once
#include "ai_broker.h"
#include <windows.h>

namespace pulse { struct AppState; }

namespace pulse::app::ai {

// The host window's WM_AI_UI_TASK; `target` is the most recently active
// window (null when none is open). Frees the task.
void HandleAiUiTask(AppState* target, LPARAM lparam);

// pulse.get_context: the active tab's folder and up to 200 selected paths.
ToolOutcome GetContextTool(HWND host, const ToolCall& call);
// pulse.reveal {paths[]}: opens the first item's folder (the client's reveal
// tab when it still exists, else a tab already showing it, else a new one)
// and selects the items in it; items in other folders come back as not_shown.
ToolOutcome RevealTool(HWND host, const ToolCall& call);

} // namespace pulse::app::ai
