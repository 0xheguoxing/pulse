// ai_broker.h - The tool broker in pulse.exe: Pulse.AI.exe connects to it
// over a named pipe (ai_tool_protocol.h) and calls Pulse's tools on behalf of
// an MCP client.
//
//  - The pipe accepts only the current user, local clients, and the broker
//    owns the first instance, so nobody can squat the name.
//  - Every connection is checked before a byte is read (verify_peer: in the
//    app, the client process must run the verified Pulse.AI.exe of this
//    session) and must start with hello.
//  - Calls run concurrently (each on its own thread), can be cancelled, and
//    time out. Their permission class decides what happens first: Read, View
//    and Undo run; Create asks the user (confirm) unless an always-allow rule
//    covers it; Change is refused.
//  - Every call is written to the activity log as a summary.
//  - Undo groups produced by successful AI calls are remembered, so undo_last
//    can refuse to undo anything the user did themselves.
#pragma once
#include "ai_activity_log.h"
#include "ai_settings.h"
#include "../common/json_value.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pulse::app::ai {

struct ToolError {
    std::string code;          // see the plan's error table (user_denied, invalid_args, ...)
    std::string message;       // short, English, for the AI client
    bool retryable = false;
};

struct ToolOutcome {
    bool ok = true;
    json::Value data;
    ToolError error;
    bool changed_files = false;
    uint64_t undo_group = 0;   // non-zero: remembered as an AI group (also after a late timeout)
    static ToolOutcome Fail(std::string code, std::string message, bool retryable = false) {
        ToolOutcome o;
        o.ok = false;
        o.error = {std::move(code), std::move(message), retryable};
        return o;
    }
};

struct ToolCall {
    std::wstring client;
    const json::Value& args;
    const std::atomic<bool>& cancelled;
};

struct ToolSpec {
    std::string name;
    PermissionClass cls = PermissionClass::Read;
    std::function<ToolOutcome(const ToolCall&)> run;
    DWORD timeout_ms = 30000;    // after confirmation; the call then fails with "timeout"
    // Create: the folder an always-allow rule and the confirmation refer to.
    std::function<std::wstring(const json::Value& args)> target_folder;
    // Create: checks the arguments before anyone is asked; false answers
    // `error` at once (nobody is asked about a call that cannot run).
    std::function<bool(const json::Value& args, ToolOutcome& error)> check;
};

enum class Decision { Allow, Deny, Timeout };

struct ConfirmRequest {
    std::wstring client;
    std::string tool;
    std::wstring folder;
    const json::Value& args;
};

// What the broker is doing, for Pulse's status bar and activity list. Sent
// from broker threads; the handler must be quick and must not call back into
// the broker.
enum class BrokerEventKind { Connected, Disconnected, CallStarted, WaitingUser, CallFinished };

struct BrokerEvent {
    BrokerEventKind kind = BrokerEventKind::Connected;
    uint64_t connection = 0;   // unique per connection while Pulse runs
    std::wstring client;
    std::string tool;          // calls only
    int64_t call = 0;          // calls only: the client's call id
    bool ok = false;           // CallFinished
    std::string code;          // CallFinished: the error code when !ok
    bool changed_files = false;
};

struct BrokerOptions {
    std::wstring pipe_name;
    // Required. Called with each connected pipe before anything is read.
    std::function<bool(HANDLE pipe)> verify_peer;
    // Asked for Create calls no rule covers; must honour `cancelled` and return
    // within `timeout_ms`. Without it such calls are denied.
    std::function<Decision(const ConfirmRequest&, const std::atomic<bool>& cancelled, DWORD timeout_ms)> confirm;
    // Always-allow lookup for Create calls (normally IsAlwaysAllowed on ai.json).
    std::function<bool(const std::wstring& client, const std::string& tool, const std::wstring& folder)> always_allowed;
    // Each connection's hello, before calls are accepted; false refuses the client
    // (client_not_approved). Without it every verified client is accepted.
    std::function<bool(const std::wstring& client)> approve_client;
    std::function<void(const BrokerEvent&)> on_event;
    std::wstring activity_directory;    // empty: no activity log
    DWORD confirm_timeout_ms = 120000;  // 2 minutes, then user_timeout
    size_t max_connections = 4;
    size_t max_calls_per_connection = 4;
};

class ToolBroker {
public:
    explicit ToolBroker(BrokerOptions options);
    ~ToolBroker();
    ToolBroker(const ToolBroker&) = delete;
    ToolBroker& operator=(const ToolBroker&) = delete;

    // Before Start only.
    void Register(ToolSpec tool);

    // Creates the first pipe instance (fails with ERROR_ACCESS_DENIED or
    // ERROR_PIPE_BUSY when the name is taken) and starts listening.
    bool Start(DWORD& error);
    // Cancels every call, closes every connection and waits for all threads.
    void Stop();
    bool running() const noexcept { return running_.load(); }

    size_t connection_count() const;
    // Closes every connection of `client`: its calls are cancelled. The client
    // may connect again (and is asked again unless allowed).
    void DisconnectClient(const std::wstring& client);
    bool IsAiUndoGroup(uint64_t group) const;
    void ForgetAiUndoGroup(uint64_t group);

private:
    struct Connection;
    struct Call;
    void Listen();
    void Serve(std::shared_ptr<Connection> connection);
    void RunCall(std::shared_ptr<Connection> connection, std::shared_ptr<Call> call);
    bool Send(Connection& connection, const json::Value& message);
    void Log(const Connection& connection, const std::string& tool, const json::Value& args,
             const ToolOutcome& outcome, uint64_t started);
    void Emit(const Connection& connection, BrokerEventKind kind, const std::string& tool = {}, int64_t call = 0,
              const ToolOutcome* outcome = nullptr);

    BrokerOptions options_;
    std::unordered_map<std::string, ToolSpec> tools_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    HANDLE stop_event_ = nullptr;
    HANDLE listening_ = INVALID_HANDLE_VALUE;
    std::thread listener_;
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::set<uint64_t> ai_undo_groups_;
    std::atomic<uint64_t> next_connection_{1};
};

} // namespace pulse::app::ai
