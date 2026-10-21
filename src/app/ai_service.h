// ai_service.h - The tool broker (ai_broker.h) as part of the running Pulse.
//
// Pulse listens for Pulse.AI.exe only while the AI pack is usable (Ready, or
// Bundled in development builds) and "Allow AI clients" is on in ai.json
// (mcp_enabled, off by default). The checks and the start run on a worker
// thread: the pack check reads and hashes Pulse.AI.exe, which must not happen
// on the UI thread.
//
// M3 registers no tools yet (M4 does), so every call answers "unsupported";
// Create would be refused because there is no confirmation UI yet (M5).
#pragma once

namespace pulse::app::ai {

// Starts the broker in the background if the pack and ai.json allow it. Does
// nothing when it already runs or is starting. Also prunes the activity log.
void StartAiServiceAsync();

// Stops the broker and waits for its threads: running calls are cancelled and
// the pipe closes. Safe to call when nothing runs. Called on the way out.
void StopAiService();

bool AiServiceRunning();

} // namespace pulse::app::ai
