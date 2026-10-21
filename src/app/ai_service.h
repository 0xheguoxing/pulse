// ai_service.h - The tool broker (ai_broker.h) as part of the running Pulse.
//
// Pulse listens for Pulse.AI.exe only while the AI pack is usable (Ready, or
// Bundled in development builds) and "Allow AI clients" is on in ai.json
// (mcp_enabled, off by default). The checks and the start run on a worker
// thread: the pack check reads and hashes Pulse.AI.exe, which must not happen
// on the UI thread.
//
// The tools are ai_tools.h's first batch. A client not yet allowed is asked
// about in Pulse on its first connection, and Create calls no rule covers are
// confirmed there too (ai_confirm.h); ai_status.h follows what runs.
#pragma once
#include "ai_settings.h"
#include "ai_tools.h"
#include <functional>

namespace pulse::app::ai {

// Starts the broker in the background if the pack and ai.json allow it. Does
// nothing when it already runs or is starting. Also prunes the activity log.
// `host` (Pulse's host window, operations, index) outlives StopAiService.
void StartAiServiceAsync(const AiToolsHost& host);

// Stops the broker and waits for its threads: running calls are cancelled and
// the pipe closes. Safe to call when nothing runs. Called on the way out.
void StopAiService();

bool AiServiceRunning();

// The same stop from the settings page: on the service's worker thread, so
// the UI thread never waits for running calls to finish.
void StopAiServiceAsync();

// ai.json, read-modify-written under one lock; a file that does not parse is
// left alone (false). Small file, local disk: fine on the UI thread.
bool UpdateAiSettings(const std::function<void(Settings&)>& change);
Settings ReadAiSettings(bool* malformed = nullptr);

// Settings > AI 与 MCP.
bool SetAiMcpEnabled(bool enabled, const AiToolsHost& host);   // saves, then starts or stops
void SetAiClientGrant(const std::wstring& client, ClientGrant grant);   // Denied also disconnects it
void ForgetAiClient(const std::wstring& client);   // its record and rules; disconnects it
void RemoveAiAllowRule(const AllowRule& rule);
void DisconnectAiClient(const std::wstring& client);

} // namespace pulse::app::ai
