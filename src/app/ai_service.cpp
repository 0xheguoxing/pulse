#include "ai_service.h"
#include "ai_activity_log.h"
#include "ai_broker.h"
#include "ai_confirm.h"
#include "ai_pack.h"
#include "ai_settings_ui.h"
#include "ai_status.h"
#include "ai_tools_ui.h"
#include "../common/runtime_log.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace pulse::app::ai {
namespace {

constexpr DWORD kConnectQuestionMs = 120000;

struct Service {
    std::mutex mu;                               // guards the members below
    std::unique_ptr<ToolBroker> broker;
    std::shared_ptr<const Settings> settings;    // what the broker's callbacks read
    AiToolsHost host;
    std::set<std::wstring> session_clients;      // "只允许这次": until Pulse exits
    std::atomic<bool> asking_stop{false};        // open questions give up
    std::mutex file_mu;                          // ai.json read-modify-write
    // Starts and stops run in order on one worker thread, so a quick
    // off / on can never interleave them.
    std::mutex jobs_mu;
    std::condition_variable jobs_cv;
    std::deque<std::function<void()>> jobs;
    std::thread worker;
    bool worker_exit = false;
};

Service& Get() {
    static auto* service = new Service;  // process lifetime, like the other app services
    return *service;
}

void WorkerLoop(Service& s) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(s.jobs_mu);
            s.jobs_cv.wait(lock, [&] { return s.worker_exit || !s.jobs.empty(); });
            if (s.jobs.empty()) return;   // exiting and nothing left
            job = std::move(s.jobs.front());
            s.jobs.pop_front();
        }
        try {
            job();
        } catch (...) {
        }
    }
}

void Enqueue(Service& s, std::function<void()> job) {
    std::lock_guard lock(s.jobs_mu);
    if (s.worker_exit) return;
    if (!s.worker.joinable()) {
        try {
            s.worker = std::thread([&s] { WorkerLoop(s); });
        } catch (...) {
            return;
        }
    }
    s.jobs.push_back(std::move(job));
    s.jobs_cv.notify_one();
}

std::shared_ptr<const Settings> CurrentSettings() {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    return s.settings;
}

HWND HostWindow() {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    return s.host.ui_window;
}

// The status bar follows every event; a call that changed files also gets a
// notification in Pulse (with 撤销), shown on the UI thread.
void OnEvent(const BrokerEvent& event) {
    OnAiBrokerEvent(event);
    if (event.kind != BrokerEventKind::CallFinished || !event.ok || !event.changed_files) return;
    auto copy = std::make_shared<BrokerEvent>(event);
    PostAiUiTask(HostWindow(), [copy](AppState* s) {
        if (s) pulse::NoteAiChange(*s, *copy);
    });
}

bool ApproveClient(const std::wstring& client) {
    Service& s = Get();
    const auto settings = CurrentSettings();
    if (!settings) return false;
    for (const ClientRecord& record : settings->clients) {
        if (record.name != client) continue;
        if (record.grant == ClientGrant::Allowed) return true;
        if (record.grant == ClientGrant::Denied) return false;
    }
    {
        std::lock_guard lock(s.mu);
        if (s.session_clients.count(client)) return true;
    }
    // Not decided yet: ask in Pulse.
    switch (AskToConnect(HostWindow(), client, s.asking_stop, kConnectQuestionMs)) {
        case ConnectAnswer::Allow:
            SetAiClientGrant(client, ClientGrant::Allowed);
            return true;
        case ConnectAnswer::Session: {
            std::lock_guard lock(s.mu);
            s.session_clients.insert(client);
            return true;
        }
        case ConnectAnswer::Deny:
            break;
    }
    return false;
}

// 始终允许这个文件夹: once per client, tool and folder.
void AddAllowRule(const AllowRule& add) {
    UpdateAiSettings([&](Settings& settings) {
        for (const AllowRule& rule : settings.allow)
            if (rule.client == add.client && rule.tool == add.tool && _wcsicmp(rule.folder.c_str(), add.folder.c_str()) == 0)
                return;
        settings.allow.push_back(add);
    });
}

Decision Confirm(const ConfirmRequest& request, const std::atomic<bool>& cancelled, DWORD timeout_ms) {
    switch (AskToCreate(HostWindow(), request, cancelled, Get().asking_stop, timeout_ms)) {
        case CreateAnswer::Allow: return Decision::Allow;
        case CreateAnswer::AlwaysHere:
            AddAllowRule({request.client, request.tool, request.folder});
            return Decision::Allow;
        case CreateAnswer::Timeout: return Decision::Timeout;
        case CreateAnswer::Deny: break;
    }
    return Decision::Deny;
}

bool AlwaysAllowed(const std::wstring& client, const std::string& tool, const std::wstring& folder) {
    const auto settings = CurrentSettings();
    return settings && IsAlwaysAllowed(*settings, PermissionClass::Create, client, tool, folder);
}

// Worker thread: everything here may touch the disk.
void StartNow(Service& s) {
    using diagnostics::runtime::Event;
    if (!AiPackReady(QueryAiPackState())) return;   // not installed: nothing listens
    bool malformed = false;
    auto settings = std::make_shared<const Settings>(LoadSettings(&malformed));
    if (malformed) Event("ai_settings_malformed", {}, diagnostics::runtime::Level::Warning);
    const std::wstring activity = ActivityDirectory();
    if (!activity.empty()) PruneActivity(activity, settings->log_days, UnixMillisecondsNow());
    if (!settings->mcp_enabled) return;
    const std::wstring pipe = AiToolPipeName();
    if (pipe.empty()) return;

    BrokerOptions options;
    options.pipe_name = pipe;
    options.verify_peer = IsAiHostPipeClient;
    options.approve_client = ApproveClient;
    options.always_allowed = AlwaysAllowed;
    options.confirm = Confirm;
    options.on_event = OnEvent;
    options.activity_directory = activity;
    auto broker = std::make_unique<ToolBroker>(std::move(options));
    AiToolsHost host;
    {
        std::lock_guard lock(s.mu);
        s.settings = settings;
        host = s.host;
    }
    // The tools read the switches as they are now (run_actions, reveal) and
    // store 始终允许这个文件夹 from Pulse's own questions (run_actions).
    host.settings = CurrentSettings;
    host.add_rule = AddAllowRule;
    RegisterAiTools(*broker, host);
    DWORD error = ERROR_SUCCESS;
    if (!broker->Start(error)) {
        Event("ai_service_start_failed", {{"error", static_cast<uint64_t>(error)}},
              diagnostics::runtime::Level::Warning);
        return;
    }
    {
        std::lock_guard lock(s.mu);
        s.broker = std::move(broker);
    }
    SetAiStatusWindow(host.ui_window);
    SetAiListening(true);
}

// Worker thread: stops the broker and waits for its threads.
void StopNow(Service& s) {
    s.asking_stop = true;   // a broker thread may be waiting on a question
    std::unique_ptr<ToolBroker> broker;
    {
        std::lock_guard lock(s.mu);
        broker = std::move(s.broker);
        s.settings.reset();
    }
    if (broker) broker->Stop();
    SetAiListening(false);
}

} // namespace

void StartAiServiceAsync(const AiToolsHost& host) {
    Service& s = Get();
    {
        std::lock_guard lock(s.mu);
        s.host = host;
    }
    Enqueue(s, [&s] {
        {
            std::lock_guard lock(s.mu);
            if (s.broker) return;   // already listening
        }
        s.asking_stop = false;
        StartNow(s);
    });
}

void StopAiServiceAsync() {
    Service& s = Get();
    Enqueue(s, [&s] { StopNow(s); });
}

void StopAiService() {
    Service& s = Get();
    s.asking_stop = true;   // at once, even while a start is still running
    Enqueue(s, [&s] { StopNow(s); });
    std::thread worker;
    {
        std::lock_guard lock(s.jobs_mu);
        s.worker_exit = true;
        s.jobs_cv.notify_all();
        worker = std::move(s.worker);
    }
    if (worker.joinable()) worker.join();
}

bool AiServiceRunning() {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    return s.broker && s.broker->running();
}

bool UpdateAiSettings(const std::function<void(Settings&)>& change) {
    Service& s = Get();
    std::lock_guard file(s.file_mu);
    bool malformed = false;
    Settings settings = LoadSettings(&malformed);
    if (malformed) return false;   // never overwrite a file that does not parse
    change(settings);
    if (!SaveSettings(settings)) return false;
    std::lock_guard lock(s.mu);
    if (s.settings) s.settings = std::make_shared<const Settings>(std::move(settings));
    return true;
}

Settings ReadAiSettings(bool* malformed) {
    Service& s = Get();
    std::lock_guard file(s.file_mu);
    return LoadSettings(malformed);
}

bool SetAiMcpEnabled(bool enabled) {
    if (!UpdateAiSettings([enabled](Settings& settings) { settings.mcp_enabled = enabled; })) return false;
    if (enabled) {
        AiToolsHost host;
        {
            Service& s = Get();
            std::lock_guard lock(s.mu);
            host = s.host;
        }
        if (!host.ui_window) return false;   // never started (test instance)
        StartAiServiceAsync(host);
    } else {
        StopAiServiceAsync();
    }
    return true;
}

bool IsAiSessionClient(const std::wstring& client) {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    return s.session_clients.count(client) != 0;
}

void SetAiClientGrant(const std::wstring& client, ClientGrant grant) {
    UpdateAiSettings([&](Settings& settings) {
        for (ClientRecord& record : settings.clients)
            if (record.name == client) {
                record.grant = grant;
                return;
            }
        settings.clients.push_back({client, grant, 0});
    });
    if (grant == ClientGrant::Denied) DisconnectAiClient(client);
}

void ForgetAiClient(const std::wstring& client) {
    UpdateAiSettings([&](Settings& settings) {
        std::erase_if(settings.clients, [&](const ClientRecord& r) { return r.name == client; });
        std::erase_if(settings.allow, [&](const AllowRule& r) { return r.client == client; });
    });
    {
        Service& s = Get();
        std::lock_guard lock(s.mu);
        s.session_clients.erase(client);
    }
    DisconnectAiClient(client);
}

void RemoveAiAllowRule(const AllowRule& rule) {
    UpdateAiSettings([&](Settings& settings) {
        std::erase_if(settings.allow, [&](const AllowRule& r) {
            return r.client == rule.client && r.tool == rule.tool && _wcsicmp(r.folder.c_str(), rule.folder.c_str()) == 0;
        });
    });
}

void DisconnectAiClient(const std::wstring& client) {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    if (s.broker) s.broker->DisconnectClient(client);
}

} // namespace pulse::app::ai
