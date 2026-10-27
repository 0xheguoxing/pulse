// ai_status.h - What AI clients are doing right now, for the status bar's AI
// chip and the "AI 与 MCP" settings page. Fed by the broker's events (broker
// threads); read by the UI thread. Changes post WM_AI_STATUS to Pulse's host
// window, at most one pending at a time.
#pragma once
#include "ai_broker.h"
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace pulse::app::ai {

struct AiCallEntry {
    uint64_t connection = 0;
    int64_t call = 0;
    uint64_t started_ms = 0;   // Unix milliseconds
    std::wstring client;
    std::string tool;          // without the "pulse." prefix
    bool running = true;
    bool waiting_user = false;
    bool ok = false;
    std::string code;          // the error code when it failed
    bool changed_files = false;
    std::wstring summary;      // ToolOutcome::summary
};

enum class AiChipState : uint8_t {
    Hidden,    // not listening: no chip at all
    Idle,      // listening; maybe clients connected, nothing running
    Busy,      // a call runs
    Waiting,   // a call waits for the user's answer in Pulse
    Done,      // calls finished within the last few seconds
};

struct AiChip {
    AiChipState state = AiChipState::Hidden;
    std::wstring client;       // Busy / Waiting: whose call; Idle: the first connected client (may be empty)
    std::string tool;          // Busy / Waiting
    size_t clients = 0;        // connected now
    size_t burst_calls = 0;    // Done: calls in the burst that just ended
};

struct AiStatusSnapshot {
    bool listening = false;
    std::vector<std::wstring> clients;    // connected now, first connection first, no duplicates
    std::vector<AiCallEntry> recent;      // this session, newest first, at most kMaxRecent
    size_t session_calls = 0;
};

inline constexpr size_t kMaxRecent = 200;
inline constexpr uint64_t kAiDoneHoldMs = 4000;   // how long the chip says "done"

// Where WM_AI_STATUS goes (null: nowhere).
void SetAiStatusWindow(HWND host);
// Listening started or stopped. Stopping forgets the connections and running
// calls (the broker cancelled them), not the session's list.
void SetAiListening(bool listening);
void OnAiBrokerEvent(const BrokerEvent& event);
// The UI thread handled WM_AI_STATUS: the next change posts again.
void AckAiStatusMessage();

AiStatusSnapshot GetAiStatus();
// Cheap enough for every frame. `now_tick` is GetTickCount64().
AiChip GetAiChip(uint64_t now_tick);
// Milliseconds until the chip changes by itself (Done ends), or 0 when it will not.
uint64_t AiChipSettlesIn(uint64_t now_tick);

// Tests.
void ResetAiStatusForTest();

} // namespace pulse::app::ai
