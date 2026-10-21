#include "ai_status.h"
#include "app_messages.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>

namespace pulse::app::ai {
namespace {

struct State {
    std::mutex mu;
    HWND host = nullptr;
    bool listening = false;
    std::map<uint64_t, std::wstring> connections;   // id -> client, ids grow with time
    std::deque<AiCallEntry> recent;                  // newest first
    size_t session_calls = 0;
    size_t running = 0;
    uint64_t last_finished_tick = 0;
    size_t burst_calls = 0;                          // finished since the chip was last idle
    std::atomic<bool> posted{false};
};

State& Get() {
    static auto* state = new State;   // process lifetime: broker threads may report late
    return *state;
}

uint64_t UnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000;
}

std::string ShortTool(const std::string& tool) {
    return tool.rfind("pulse.", 0) == 0 ? tool.substr(6) : tool;
}

void Notify(State& s, HWND host) {
    if (host && !s.posted.exchange(true) && !PostMessageW(host, WM_AI_STATUS, 0, 0)) s.posted = false;
}

AiCallEntry* Find(State& s, uint64_t connection, int64_t call) {
    for (AiCallEntry& e : s.recent)
        if (e.connection == connection && e.call == call && e.running) return &e;
    return nullptr;
}

} // namespace

void SetAiStatusWindow(HWND host) {
    State& s = Get();
    std::lock_guard lock(s.mu);
    s.host = host;
}

void SetAiListening(bool listening) {
    State& s = Get();
    HWND host = nullptr;
    {
        std::lock_guard lock(s.mu);
        if (s.listening == listening) return;
        s.listening = listening;
        if (!listening) {
            s.connections.clear();
            for (AiCallEntry& e : s.recent)
                if (e.running) {
                    e.running = false;
                    e.waiting_user = false;
                    e.code = "cancelled";
                }
            s.running = 0;
            s.burst_calls = 0;
        }
        host = s.host;
    }
    Notify(s, host);
}

void OnAiBrokerEvent(const BrokerEvent& event) {
    State& s = Get();
    HWND host = nullptr;
    {
        std::lock_guard lock(s.mu);
        switch (event.kind) {
            case BrokerEventKind::Connected:
                s.connections[event.connection] = event.client;
                break;
            case BrokerEventKind::Disconnected:
                s.connections.erase(event.connection);
                break;
            case BrokerEventKind::CallStarted: {
                AiCallEntry e;
                e.connection = event.connection;
                e.call = event.call;
                e.started_ms = UnixMs();
                e.client = event.client;
                e.tool = ShortTool(event.tool);
                s.recent.push_front(std::move(e));
                // Never drop a running call to make room: its finish must find it.
                while (s.recent.size() > kMaxRecent) {
                    const auto oldest_done = std::find_if(s.recent.rbegin(), s.recent.rend(),
                                                          [](const AiCallEntry& x) { return !x.running; });
                    if (oldest_done == s.recent.rend()) break;
                    s.recent.erase(std::next(oldest_done).base());
                }
                ++s.session_calls;
                ++s.running;
                break;
            }
            case BrokerEventKind::WaitingUser:
                if (AiCallEntry* e = Find(s, event.connection, event.call)) e->waiting_user = true;
                break;
            case BrokerEventKind::CallFinished:
                if (AiCallEntry* e = Find(s, event.connection, event.call)) {
                    e->running = false;
                    e->waiting_user = false;
                    e->ok = event.ok;
                    e->code = event.code;
                    e->changed_files = event.changed_files;
                    if (s.running) --s.running;
                    ++s.burst_calls;
                    s.last_finished_tick = GetTickCount64();
                }
                break;
        }
        host = s.host;
    }
    Notify(s, host);
}

void AckAiStatusMessage() { Get().posted = false; }

AiStatusSnapshot GetAiStatus() {
    State& s = Get();
    std::lock_guard lock(s.mu);
    AiStatusSnapshot out;
    out.listening = s.listening;
    for (const auto& [id, client] : s.connections)
        if (std::none_of(out.clients.begin(), out.clients.end(),
                         [&](const std::wstring& c) { return _wcsicmp(c.c_str(), client.c_str()) == 0; }))
            out.clients.push_back(client);
    out.recent.assign(s.recent.begin(), s.recent.end());
    out.session_calls = s.session_calls;
    return out;
}

AiChip GetAiChip(uint64_t now_tick) {
    State& s = Get();
    std::lock_guard lock(s.mu);
    AiChip chip;
    if (!s.listening) return chip;
    chip.clients = s.connections.size();
    const AiCallEntry* waiting = nullptr;
    const AiCallEntry* running = nullptr;
    for (const AiCallEntry& e : s.recent) {
        if (!e.running) continue;
        if (e.waiting_user && !waiting) waiting = &e;
        if (!running) running = &e;   // newest first
    }
    if (waiting) {
        chip.state = AiChipState::Waiting;
        chip.client = waiting->client;
        chip.tool = waiting->tool;
    } else if (running) {
        chip.state = AiChipState::Busy;
        chip.client = running->client;
        chip.tool = running->tool;
    } else if (s.burst_calls && now_tick - s.last_finished_tick < kAiDoneHoldMs) {
        chip.state = AiChipState::Done;
        chip.burst_calls = s.burst_calls;
        chip.client = s.recent.empty() ? std::wstring() : s.recent.front().client;
    } else {
        s.burst_calls = 0;
        chip.state = AiChipState::Idle;
        if (!s.connections.empty()) chip.client = s.connections.begin()->second;
    }
    return chip;
}

uint64_t AiChipSettlesIn(uint64_t now_tick) {
    State& s = Get();
    std::lock_guard lock(s.mu);
    if (!s.listening || s.running || !s.burst_calls) return 0;
    const uint64_t age = now_tick - s.last_finished_tick;
    return age < kAiDoneHoldMs ? kAiDoneHoldMs - age : 0;
}

void ResetAiStatusForTest() {
    State& s = Get();
    std::lock_guard lock(s.mu);
    s.listening = false;
    s.connections.clear();
    s.recent.clear();
    s.session_calls = 0;
    s.running = 0;
    s.burst_calls = 0;
    s.last_finished_tick = 0;
    s.host = nullptr;
    s.posted = false;
}

} // namespace pulse::app::ai
