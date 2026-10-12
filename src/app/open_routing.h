// open_routing.h - Opens folders that come from outside Pulse (a second
// launch, Win+E, another program's "show in folder", Explorer takeover) where
// plan §6 says: multi-window M7a. The rules are external_open.h; this applies
// them to the windows of the process.
#pragma once
#include <windows.h>

#include <string>

namespace pulse {

struct AppState;

// `raw` as a launch passes it: a folder, a file (its folder opens with it
// selected) or This PC. `mru` is the window that received the request (the
// most recently used one). Returns the window that shows it now.
AppState& OpenExternalFolder(AppState& mru, const std::wstring& raw);

// The message loop: records key presses, clicks and wheel turns in Pulse
// windows (child controls included) as AppState::last_input_tick.
void NoteUserInput(AppState& any, const MSG& msg);

}  // namespace pulse
