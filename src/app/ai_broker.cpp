#include "ai_broker.h"
#include "../common/current_user_security.h"
#include "../common/runtime_log.h"
#include "../ipc/ai_tool_protocol.h"
#include <chrono>
#include <future>

namespace pulse::app::ai {

namespace proto = pulse::ipc::ai;

struct ToolBroker::Call {
    int64_t id = 0;
    std::string tool;
    json::Value args;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> answered{false};   // result sent: no longer counts against the limit
    std::atomic<bool> done{false};       // thread finished: can be joined
    std::thread thread;
};

struct ToolBroker::Connection {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    uint64_t id = 0;
    std::wstring client;
    std::atomic<bool> closing{false};
    std::mutex write_mu;
    std::mutex calls_mu;
    std::vector<std::shared_ptr<Call>> calls;
    std::thread thread;
    std::atomic<bool> finished{false};
};

namespace {

bool ValidClientName(const std::string& utf8) {
    const std::wstring name = json::Wide(utf8);
    if (name.empty() || name.size() > 128) return false;
    for (wchar_t c : name)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}


} // namespace

ToolBroker::ToolBroker(BrokerOptions options) : options_(std::move(options)) {}

ToolBroker::~ToolBroker() { Stop(); }

void ToolBroker::Register(ToolSpec tool) {
    if (running_) return;
    std::string name = tool.name;
    tools_[std::move(name)] = std::move(tool);
}

bool ToolBroker::Start(DWORD& error) {
    error = ERROR_SUCCESS;
    if (running_) return true;
    if (options_.pipe_name.empty() || !options_.verify_peer) { error = ERROR_INVALID_PARAMETER; return false; }
    CurrentUserSecurityAttributes security;
    if (!security) { error = GetLastError() ? GetLastError() : ERROR_ACCESS_DENIED; return false; }
    listening_ = CreateNamedPipeW(options_.pipe_name.c_str(),
                                  PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                  PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, security.get());
    if (listening_ == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        error = GetLastError();
        CloseHandle(listening_);
        listening_ = INVALID_HANDLE_VALUE;
        return false;
    }
    stopping_ = false;
    running_ = true;
    listener_ = std::thread([this] { Listen(); });
    diagnostics::runtime::Event("ai_broker_started");
    return true;
}

void ToolBroker::Stop() {
    if (!running_.exchange(false)) return;
    stopping_ = true;
    SetEvent(stop_event_);
    if (listener_.joinable()) listener_.join();
    std::vector<std::shared_ptr<Connection>> connections;
    {
        std::lock_guard lock(mu_);
        connections.swap(connections_);
    }
    for (auto& c : connections) {
        c->closing = true;
        {
            std::lock_guard lock(c->calls_mu);
            for (auto& call : c->calls) call->cancelled = true;
        }
        CancelIoEx(c->pipe, nullptr);
    }
    for (auto& c : connections)
        if (c->thread.joinable()) c->thread.join();
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
    diagnostics::runtime::Event("ai_broker_stopped");
}

size_t ToolBroker::connection_count() const {
    std::lock_guard lock(mu_);
    size_t n = 0;
    for (const auto& c : connections_) n += !c->finished;
    return n;
}

bool ToolBroker::IsAiUndoGroup(uint64_t group) const {
    std::lock_guard lock(mu_);
    return group && ai_undo_groups_.count(group);
}

void ToolBroker::ForgetAiUndoGroup(uint64_t group) {
    std::lock_guard lock(mu_);
    ai_undo_groups_.erase(group);
}

void ToolBroker::Listen() {
    CurrentUserSecurityAttributes security;
    while (!stopping_) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) break;
        bool connected = ConnectNamedPipe(listening_, &overlapped) != FALSE;
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && error == ERROR_PIPE_CONNECTED) connected = true;
        if (!connected && error == ERROR_IO_PENDING) {
            const HANDLE waits[] = {overlapped.hEvent, stop_event_};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(listening_, &overlapped, &ignored, FALSE) != FALSE;
            } else {
                CancelIoEx(listening_, &overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(listening_, &overlapped, &ignored, TRUE);
            }
        }
        CloseHandle(overlapped.hEvent);
        if (stopping_) break;
        if (!connected) {
            DisconnectNamedPipe(listening_);
            continue;
        }
        // Reserve the name with the next instance before handing this one over.
        const HANDLE pipe = listening_;
        listening_ = CreateNamedPipeW(options_.pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                      PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, security.get());
        bool accept = options_.verify_peer(pipe);
        if (!accept) diagnostics::runtime::Event("ai_broker_peer_rejected");
        if (accept) {
            std::lock_guard lock(mu_);
            std::erase_if(connections_, [](const std::shared_ptr<Connection>& c) {
                if (!c->finished) return false;
                if (c->thread.joinable()) c->thread.join();
                return true;
            });
            accept = connections_.size() < options_.max_connections;
            if (accept) {
                auto connection = std::make_shared<Connection>();
                connection->pipe = pipe;
                connection->id = next_connection_.fetch_add(1);
                connections_.push_back(connection);
                connection->thread = std::thread([this, connection] { Serve(connection); });
            } else {
                diagnostics::runtime::Event("ai_broker_busy");
            }
        }
        if (!accept) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
        if (listening_ == INVALID_HANDLE_VALUE) break;   // cannot keep the name: stop listening
    }
    if (listening_ != INVALID_HANDLE_VALUE) {
        CloseHandle(listening_);
        listening_ = INVALID_HANDLE_VALUE;
    }
}

bool ToolBroker::Send(Connection& c, const json::Value& message) {
    std::lock_guard lock(c.write_mu);
    if (c.closing) return false;
    const bool ok = proto::WriteFrame(c.pipe, message, GetTickCount64() + proto::kFrameBodyTimeoutMs,
                                      [&] { return stopping_.load(); });
    if (!ok) c.closing = true;
    return ok;
}

void ToolBroker::Serve(std::shared_ptr<Connection> c) {
    auto stopped = [&] { return stopping_.load() || c->closing.load(); };
    json::Value hello;
    bool ok = proto::ReadFrame(c->pipe, hello, GetTickCount64() + proto::kHelloTimeoutMs, stopped) ==
                  proto::FrameStatus::Ok &&
              hello.get_string("type") == proto::kHello;
    if (ok) {
        const json::Value* client = hello.find("client");
        const std::string name = client ? client->get_string("name") : std::string{};
        ok = ValidClientName(name);
        if (ok) c->client = json::Wide(name);
    }
    if (ok && options_.approve_client && !options_.approve_client(c->client)) {
        Send(*c, proto::MakeError(0, "client_not_approved", "This client is not allowed to use Pulse.", false));
        ok = false;
    }
    if (!ok) diagnostics::runtime::Event("ai_broker_hello_rejected");
    const bool connected = ok && !stopped();
    if (connected) Emit(*c, BrokerEventKind::Connected);

    while (ok && !stopped()) {
        json::Value message;
        const auto status = proto::ReadFrame(c->pipe, message, 0, stopped);
        if (status != proto::FrameStatus::Ok) {
            if (status == proto::FrameStatus::TooLarge || status == proto::FrameStatus::BadJson)
                diagnostics::runtime::Event("ai_broker_bad_frame", {{"status", static_cast<uint64_t>(status)}});
            break;
        }
        const std::string type = message.get_string("type");
        int64_t id = 0;
        const json::Value* id_value = message.find("id");
        if (!id_value || !id_value->get_int(id, 1, proto::kMaxCallId)) break;   // protocol violation
        if (type == proto::kCancel) {
            std::lock_guard lock(c->calls_mu);
            for (auto& call : c->calls)
                if (call->id == id && !call->answered) call->cancelled = true;
            continue;
        }
        if (type != proto::kCall) break;
        const json::Value* tool = message.find("tool");
        const json::Value* args = message.find("args");
        if (!tool || !tool->is_string() || (args && !args->is_object())) {
            Send(*c, proto::MakeError(id, "invalid_args", "A call needs a tool name and an object of arguments.", false));
            continue;
        }
        std::lock_guard lock(c->calls_mu);
        std::erase_if(c->calls, [](const std::shared_ptr<Call>& call) {
            if (!call->done) return false;
            if (call->thread.joinable()) call->thread.join();
            return true;
        });
        bool duplicate = false;
        size_t running = 0;
        for (const auto& call : c->calls) {
            if (call->answered) continue;
            duplicate |= call->id == id;
            ++running;
        }
        if (duplicate) {
            Send(*c, proto::MakeError(id, "invalid_args", "This call id is already in use.", false));
            continue;
        }
        if (running >= options_.max_calls_per_connection) {
            Send(*c, proto::MakeError(id, "busy", "Too many calls at once; try again.", true));
            continue;
        }
        auto call = std::make_shared<Call>();
        call->id = id;
        call->tool = tool->as_string();
        call->args = args ? *args : json::Value::Object();
        c->calls.push_back(call);
        call->thread = std::thread([this, c, call] { RunCall(c, call); });
    }

    // Closing: cancel what still runs and wait for it.
    c->closing = true;
    std::vector<std::shared_ptr<Call>> calls;
    {
        std::lock_guard lock(c->calls_mu);
        for (auto& call : c->calls) call->cancelled = true;
        calls.swap(c->calls);
    }
    for (auto& call : calls)
        if (call->thread.joinable()) call->thread.join();
    // CloseHandle, not DisconnectNamedPipe: a disconnect discards what the
    // client has not read yet (e.g. client_not_approved); a close lets it drain.
    CloseHandle(c->pipe);
    c->pipe = INVALID_HANDLE_VALUE;
    if (connected) Emit(*c, BrokerEventKind::Disconnected);
    c->finished = true;
}

void ToolBroker::DisconnectClient(const std::wstring& client) {
    std::lock_guard lock(mu_);
    for (const auto& c : connections_)
        if (!c->finished && _wcsicmp(c->client.c_str(), client.c_str()) == 0) c->closing = true;
}

void ToolBroker::Emit(const Connection& c, BrokerEventKind kind, const std::string& tool, int64_t call,
                      const ToolOutcome* outcome) {
    if (!options_.on_event) return;
    BrokerEvent event;
    event.kind = kind;
    event.connection = c.id;
    event.client = c.client;
    event.tool = tool;
    event.call = call;
    if (outcome) {
        event.ok = outcome->ok;
        if (!outcome->ok) event.code = outcome->error.code;
        event.changed_files = outcome->changed_files;
    }
    try {
        options_.on_event(event);
    } catch (...) {
    }
}

void ToolBroker::RunCall(std::shared_ptr<Connection> c, std::shared_ptr<Call> call) {
    const uint64_t started = UnixMillisecondsNow();
    ToolOutcome outcome;
    bool answered_early = false;
    Emit(*c, BrokerEventKind::CallStarted, call->tool, call->id);
    const auto found = tools_.find(call->tool);
    if (found == tools_.end()) {
        outcome = ToolOutcome::Fail("unsupported", "Pulse has no tool by this name.");
    } else {
        const ToolSpec& spec = found->second;
        bool allowed = true;
        if (spec.cls == PermissionClass::Change) {
            outcome = ToolOutcome::Fail("access_denied", "Changing files this way is not available.");
            allowed = false;
        } else if (spec.cls == PermissionClass::Create && spec.check && !spec.check(call->args, outcome)) {
            if (outcome.ok) outcome = ToolOutcome::Fail("invalid_args", "The arguments are not valid.");
            allowed = false;
        } else if (spec.cls == PermissionClass::Create) {
            const std::wstring folder = spec.target_folder ? spec.target_folder(call->args) : std::wstring{};
            const bool rule = options_.always_allowed && !folder.empty() &&
                              options_.always_allowed(c->client, spec.name, folder);
            if (!rule) {
                Decision decision = Decision::Deny;
                if (options_.confirm) {
                    Send(*c, proto::MakeProgress(call->id, proto::kWaitingUser));
                    Emit(*c, BrokerEventKind::WaitingUser, call->tool, call->id);
                    decision = options_.confirm(ConfirmRequest{c->client, spec.name, folder, call->args}, call->cancelled,
                                                options_.confirm_timeout_ms);
                }
                if (call->cancelled) {
                    outcome = ToolOutcome::Fail("cancelled", "The call was cancelled.");
                    allowed = false;
                } else if (decision == Decision::Timeout) {
                    outcome = ToolOutcome::Fail("user_timeout", "Nobody answered the confirmation in Pulse.", true);
                    allowed = false;
                } else if (decision != Decision::Allow) {
                    outcome = ToolOutcome::Fail("user_denied", "The user declined this in Pulse.");
                    allowed = false;
                }
            }
        }
        if (allowed) {
            // The handler runs on its own thread so a slow one can be timed out.
            // A timeout or cancel is answered at once; the call (its id and its
            // slot) stays taken until the handler really returns.
            auto task = std::async(std::launch::async, [&spec, &c, &call]() -> ToolOutcome {
                try {
                    return spec.run(ToolCall{c->client, call->args, call->cancelled});
                } catch (...) {
                    return ToolOutcome::Fail("internal", "The tool failed unexpectedly.");
                }
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(spec.timeout_ms);
            bool finished = false;
            while (!finished) {
                finished = task.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
                if (finished) break;
                const bool late = std::chrono::steady_clock::now() >= deadline;
                if (late || call->cancelled) {
                    call->cancelled = true;
                    outcome = late ? ToolOutcome::Fail("timeout", "The tool took too long.", true)
                                   : ToolOutcome::Fail("cancelled", "The call was cancelled.");
                    Send(*c, proto::MakeError(call->id, outcome.error.code, outcome.error.message,
                                              outcome.error.retryable));
                    answered_early = true;
                    break;
                }
            }
            if (finished) {
                outcome = task.get();
                if (call->cancelled && outcome.ok) outcome = ToolOutcome::Fail("cancelled", "The call was cancelled.");
            } else {
                // Already answered. If the handler still changed files, keep its
                // undo group undoable and record the change.
                const ToolOutcome late_result = task.get();
                if (late_result.ok) {
                    outcome.changed_files = late_result.changed_files;
                    outcome.undo_group = late_result.undo_group;
                }
            }
        }
    }
    if (outcome.undo_group) {
        std::lock_guard lock(mu_);
        ai_undo_groups_.insert(outcome.undo_group);
    }
    // Before the answer goes out, so the client's next call cannot be seen first.
    Emit(*c, BrokerEventKind::CallFinished, call->tool, call->id, &outcome);
    if (!answered_early)
        Send(*c, outcome.ok ? proto::MakeResult(call->id, outcome.data)
                        : proto::MakeError(call->id, outcome.error.code, outcome.error.message, outcome.error.retryable));
    call->answered = true;
    Log(*c, call->tool, call->args, outcome, started);
    call->done = true;
}

void ToolBroker::Log(const Connection& c, const std::string& tool, const json::Value& args, const ToolOutcome& outcome,
                     uint64_t started) {
    if (options_.activity_directory.empty()) return;
    Activity a;
    a.time = started;
    a.client = c.client;
    a.tool = tool.size() <= 128 ? tool : tool.substr(0, 128);
    SummarizeArgs(args, a);
    a.outcome = outcome.ok ? "ok" : outcome.error.code;
    a.changed_files = outcome.changed_files;   // can be set on a late timeout too
    a.undo_group = outcome.undo_group;
    const uint64_t now = UnixMillisecondsNow();
    a.duration_ms = static_cast<uint32_t>(now > started ? (std::min)(now - started, uint64_t{0xFFFFFFFF}) : 0);
    AppendActivity(options_.activity_directory, a);
}

} // namespace pulse::app::ai
