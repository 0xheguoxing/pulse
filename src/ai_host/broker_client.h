// broker_client.h - Pulse.AI.exe's side of the tool broker pipe
// (ai_tool_protocol.h).
//
// Connects lazily on the first call and again after Pulse restarts, so the
// MCP server keeps answering discovery and tools/list while Pulse is closed.
// Before saying hello it checks the other end: the pipe's server process
// must run the very pulse.exe that launched us (same file, same session).
// One reader thread routes results and progress to the waiting calls.
#pragma once
#include "../common/json_value.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pulse::ai_host {

struct BrokerReply {
    bool ok = false;
    json::Value data;          // ok
    std::string code;          // !ok: the plan's error codes (pulse_not_running, user_denied, ...)
    std::string message;
    bool retryable = false;
    static BrokerReply Fail(std::string code, std::string message, bool retryable = false) {
        BrokerReply r;
        r.code = std::move(code);
        r.message = std::move(message);
        r.retryable = retryable;
        return r;
    }
};

class BrokerClient {
public:
    // `server_image`: the pulse.exe that must own the pipe.
    BrokerClient(std::wstring pipe_name, std::wstring server_image, std::string pack_version);
    ~BrokerClient();
    BrokerClient(const BrokerClient&) = delete;
    BrokerClient& operator=(const BrokerClient&) = delete;

    // The MCP client's name and version, sent in hello. Applies to the next connection.
    void SetClient(std::string name, std::string version);

    // Runs one tool. Blocks until the answer, a lost connection or Shutdown.
    // When `cancelled` turns true a cancel is sent and the call returns at the
    // latest a few seconds later. `on_waiting_user` runs (on this thread) when
    // Pulse starts asking the user.
    BrokerReply Call(const std::string& tool, const json::Value& args, const std::atomic<bool>& cancelled,
                     const std::function<void()>& on_waiting_user);

    // Ends every call and the connection; Call returns pulse_not_running after this.
    void Shutdown();

private:
    struct Pending;
    struct Connection;
    std::shared_ptr<Connection> Connect(BrokerReply& failure);
    void Read(std::shared_ptr<Connection> connection);
    bool Send(Connection& connection, const json::Value& message);
    void Drop(Connection& connection, const BrokerReply& reason);

    const std::wstring pipe_name_;
    const std::wstring server_image_;
    const std::string pack_version_;
    std::mutex connect_mu_;               // one connection attempt at a time
    std::mutex mu_;                       // the members below and every Pending
    std::condition_variable cv_;
    std::shared_ptr<Connection> connection_;
    std::string client_name_ = "MCP client";
    std::string client_version_;
    int64_t next_id_ = 0;
    bool shutdown_ = false;
    std::atomic<bool> stopping_{false};
    std::vector<std::thread> readers_;
};

} // namespace pulse::ai_host
