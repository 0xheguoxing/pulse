#include "broker_client.h"
#include "../common/current_user_security.h"
#include "../ipc/ai_tool_protocol.h"
#include <chrono>

namespace pulse::ai_host {
namespace proto = ipc::ai;
namespace {

constexpr char kNotRunning[] =
    "Pulse is not running, or AI clients are not allowed to connect (Pulse > Settings > AI & MCP). "
    "Start Pulse or turn the option on, then try again.";
constexpr char kRefused[] =
    "Pulse refused the connection. The AI pack must match the running Pulse: update it in Pulse under "
    "Settings > Enhancement packs, and configure the pulse.exe of the Pulse you run.";
constexpr char kNotPulse[] =
    "The program serving Pulse's tools is not the Pulse that started Pulse.AI, so the connection was refused.";

class Handle {
public:
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

bool FileIdentity(const wchar_t* path, BY_HANDLE_FILE_INFORMATION& info) {
    Handle file(CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    return file && GetFileInformationByHandle(file.get(), &info);
}

// The process runs `image` (the same file, not just the same path), in this
// session, as this user.
bool ServerRuns(ULONG pid, const std::wstring& image) {
    DWORD mine = 0, theirs = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &mine) || !ProcessIdToSessionId(pid, &theirs) || mine != theirs)
        return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return false;
    const std::wstring sid = ProcessUserSidString(process.get());
    if (sid.empty() || sid != CurrentUserSidString()) return false;
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)) return false;
    path.resize(size);
    BY_HANDLE_FILE_INFORMATION running{}, expected{};
    return FileIdentity(path.c_str(), running) && FileIdentity(image.c_str(), expected) &&
           running.dwVolumeSerialNumber == expected.dwVolumeSerialNumber &&
           running.nFileIndexHigh == expected.nFileIndexHigh && running.nFileIndexLow == expected.nFileIndexLow;
}

enum class Server { Pulse, Other, Gone };

// Who serves the pipe. Gone: the server already dropped us, as the broker
// does with a peer it rejects (before reading anything).
Server CheckServer(HANDLE pipe, const std::wstring& image) {
    if (image.empty()) return Server::Other;
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid) || pid == 0) {
        const DWORD error = GetLastError();
        return error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
            ? Server::Gone : Server::Other;
    }
    return ServerRuns(pid, image) ? Server::Pulse : Server::Other;
}

} // namespace

struct BrokerClient::Pending {
    bool done = false;
    bool waiting_user = false;
    BrokerReply reply;
};

struct BrokerClient::Connection {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex write_mu;
    std::atomic<bool> stop{false};
    // Guarded by BrokerClient::mu_.
    bool closed = false;
    bool any_frame = false;
    bool refused = false;
    BrokerReply refusal;
    std::map<int64_t, Pending*> pending;
    ~Connection() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    }
};

BrokerClient::BrokerClient(std::wstring pipe_name, std::wstring server_image, std::string pack_version)
    : pipe_name_(std::move(pipe_name)), server_image_(std::move(server_image)), pack_version_(std::move(pack_version)) {}

BrokerClient::~BrokerClient() {
    Shutdown();
    for (auto& reader : readers_)
        if (reader.joinable()) reader.join();
}

void BrokerClient::SetClient(std::string name, std::string version) {
    std::lock_guard lock(mu_);
    if (!name.empty()) client_name_ = std::move(name);
    client_version_ = std::move(version);
}

std::shared_ptr<BrokerClient::Connection> BrokerClient::Connect(BrokerReply& failure) {
    std::lock_guard connecting(connect_mu_);
    std::string name, version;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) {
            failure = BrokerReply::Fail("pulse_not_running", "Pulse.AI is shutting down.", true);
            return nullptr;
        }
        if (connection_ && !connection_->closed) return connection_;
        name = client_name_;
        version = client_version_;
    }
    if (pipe_name_.empty()) {
        failure = BrokerReply::Fail("pulse_not_running", kNotRunning, true);
        return nullptr;
    }
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Identification only: a server can tell who we are, not act as us.
        pipe = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY && attempt == 0 && WaitNamedPipeW(pipe_name_.c_str(), 2000)) continue;
        failure = error == ERROR_PIPE_BUSY
            ? BrokerReply::Fail("busy", "Pulse is busy with other AI clients; try again shortly.", true)
            : BrokerReply::Fail("pulse_not_running", kNotRunning, true);
        return nullptr;
    }
    auto connection = std::make_shared<Connection>();
    connection->pipe = pipe;
    switch (CheckServer(pipe, server_image_)) {
        case Server::Pulse: break;
        case Server::Gone: failure = BrokerReply::Fail("pack_outdated", kRefused); return nullptr;
        case Server::Other: failure = BrokerReply::Fail("access_denied", kNotPulse); return nullptr;
    }
    if (!proto::WriteFrame(pipe, proto::MakeHello(name, version, pack_version_),
                           GetTickCount64() + proto::kHelloTimeoutMs, [this] { return stopping_.load(); })) {
        failure = BrokerReply::Fail("pack_outdated", kRefused);
        return nullptr;
    }
    std::lock_guard lock(mu_);
    if (shutdown_) {
        failure = BrokerReply::Fail("pulse_not_running", "Pulse.AI is shutting down.", true);
        return nullptr;
    }
    connection_ = connection;
    readers_.emplace_back([this, connection] { Read(connection); });
    return connection;
}

void BrokerClient::Read(std::shared_ptr<Connection> c) {
    auto stopped = [&] { return stopping_.load() || c->stop.load(); };
    for (;;) {
        json::Value m;
        if (proto::ReadFrame(c->pipe, m, 0, stopped) != proto::FrameStatus::Ok) break;
        int64_t id = 0;
        const json::Value* id_value = m.find("id");
        if (!id_value || !id_value->get_int(id, 0, proto::kMaxCallId)) continue;
        const std::string type = m.get_string("type");
        std::lock_guard lock(mu_);
        c->any_frame = true;
        if (type == proto::kResult && id == 0) {
            // About the connection itself (client_not_approved); the broker closes next.
            const json::Value* error = m.find("error");
            c->refused = true;
            c->refusal = BrokerReply::Fail(error ? error->get_string("code", "internal") : "internal",
                                           error ? error->get_string("message") : std::string{},
                                           error && error->get_bool("retryable"));
            continue;
        }
        const auto it = c->pending.find(id);
        if (it == c->pending.end()) continue;
        Pending& p = *it->second;
        if (type == proto::kProgress) {
            if (m.get_string("state") == proto::kWaitingUser) p.waiting_user = true;
        } else if (type == proto::kResult) {
            if (m.get_bool("ok")) {
                p.reply.ok = true;
                if (const json::Value* data = m.find("data")) p.reply.data = *data;
            } else {
                const json::Value* error = m.find("error");
                p.reply = BrokerReply::Fail(error ? error->get_string("code", "internal") : "internal",
                                            error ? error->get_string("message") : std::string{},
                                            error && error->get_bool("retryable"));
            }
            p.done = true;
            c->pending.erase(it);
        }
        cv_.notify_all();
    }
    BrokerReply reason;
    {
        std::lock_guard lock(mu_);
        if (c->refused) reason = c->refusal;
        else if (stopping_) reason = BrokerReply::Fail("pulse_not_running", "Pulse.AI is shutting down.", true);
        else if (c->any_frame) reason = BrokerReply::Fail("pulse_not_running", "Pulse closed the connection.", true);
        else reason = BrokerReply::Fail("pack_outdated", kRefused);
    }
    Drop(*c, reason);
}

void BrokerClient::Drop(Connection& c, const BrokerReply& reason) {
    std::lock_guard lock(mu_);
    c.closed = true;
    c.stop = true;
    for (auto& [id, pending] : c.pending) {
        pending->reply = reason;
        pending->done = true;
    }
    c.pending.clear();
    if (connection_.get() == &c) connection_.reset();
    cv_.notify_all();
}

bool BrokerClient::Send(Connection& c, const json::Value& message) {
    std::lock_guard writing(c.write_mu);
    {
        std::lock_guard lock(mu_);
        if (c.closed) return false;
    }
    return proto::WriteFrame(c.pipe, message, GetTickCount64() + proto::kFrameBodyTimeoutMs,
                             [&] { return stopping_.load() || c.stop.load(); });
}

BrokerReply BrokerClient::Call(const std::string& tool, const json::Value& args, const std::atomic<bool>& cancelled,
                               const std::function<void()>& on_waiting_user) {
    if (cancelled) return BrokerReply::Fail("cancelled", "The call was cancelled.");
    BrokerReply failure;
    const std::shared_ptr<Connection> c = Connect(failure);
    if (!c) return failure;
    Pending p;
    int64_t id = 0;
    {
        std::lock_guard lock(mu_);
        if (c->closed) return BrokerReply::Fail("pulse_not_running", "Pulse closed the connection.", true);
        id = ++next_id_;
        c->pending[id] = &p;
    }
    const bool sent = Send(*c, proto::MakeCall(id, tool, args));
    std::unique_lock lock(mu_);
    if (!sent) {
        // The pipe is going away; the reader knows why (e.g. client_not_approved
        // just arrived) and ends this call with that reason.
        cv_.wait_for(lock, std::chrono::seconds(2), [&] { return p.done; });
        if (p.done) return p.reply;
        c->pending.erase(id);
        return c->any_frame ? BrokerReply::Fail("pulse_not_running", "Pulse closed the connection.", true)
                            : BrokerReply::Fail("pack_outdated", kRefused);
    }
    bool reported = false;
    bool cancel_sent = false;
    ULONGLONG cancel_deadline = 0;
    for (;;) {
        if (p.done) return p.reply;
        if (p.waiting_user && !reported) {
            reported = true;
            lock.unlock();
            if (on_waiting_user && !cancelled) on_waiting_user();
            lock.lock();
            continue;
        }
        if (cancelled && !cancel_sent) {
            cancel_sent = true;
            cancel_deadline = GetTickCount64() + 3000;
            lock.unlock();
            Send(*c, proto::MakeCancel(id));
            lock.lock();
            continue;
        }
        if (cancel_sent && GetTickCount64() >= cancel_deadline) {
            c->pending.erase(id);
            return BrokerReply::Fail("cancelled", "The call was cancelled.");
        }
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
}

void BrokerClient::Shutdown() {
    std::shared_ptr<Connection> c;
    {
        std::lock_guard lock(mu_);
        shutdown_ = true;
        c = connection_;
    }
    stopping_ = true;
    if (c) {
        CancelIoEx(c->pipe, nullptr);
        Drop(*c, BrokerReply::Fail("pulse_not_running", "Pulse.AI is shutting down.", true));
    }
}

} // namespace pulse::ai_host
