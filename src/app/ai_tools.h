// ai_tools.h - The first batch of AI tools, registered on the tool broker.
//
//   pulse.get_context     Read    the window's state, as get_state      (UI thread)
//   pulse.list_folder     Read    a folder's items, paged             (ai_tools_fs.h)
//   pulse.search          Read    Pulse's file name index
//   pulse.get_properties  Read    file system and shell properties    (ai_tools_fs.h)
//   pulse.reveal          View    open the items' folder and select them (UI thread)
//   pulse.create_zip      Create  a new archive; one undo step "New zip by {client}"
//   pulse.undo_last       Undo    the newest undo step, only when an AI client made it
//
// The broker runs each call on its own thread; tools that need a window post
// to Pulse's host window (ai_tools_ui.h) and never block the UI thread.
#pragma once
#include "ai_broker.h"
#include <cstdint>
#include <windows.h>

namespace pulse::ops { class OpsManager; }
namespace pulse::index { class IndexClient; }

namespace pulse::app::ai {

struct AiToolsHost {
    HWND ui_window = nullptr;               // Pulse's host window (WM_AI_UI_TASK)
    ops::OpsManager* ops = nullptr;         // create_zip's undo step, undo_last
    index::IndexClient* index = nullptr;    // search
};

// Before broker.Start. `broker` and everything in `host` outlive the broker's
// calls (StopAiService stops the broker before Pulse tears them down).
void RegisterAiTools(ToolBroker& broker, const AiToolsHost& host);

// WM_INDEX_SEARCH, before the UI takes the result: true when request_id is an
// AI search's (its tool takes the result), so the UI must ignore it.
bool ClaimAiIndexResult(uint32_t request_id);

} // namespace pulse::app::ai
