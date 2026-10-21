#include "ai_service.h"
#include "ai_activity_log.h"
#include "ai_broker.h"
#include "ai_pack.h"
#include "ai_settings.h"
#include "../common/runtime_log.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse::app::ai {
namespace {

struct Service {
    std::mutex mu;                               // guards the members below
    std::thread starter;
    std::unique_ptr<ToolBroker> broker;
    std::shared_ptr<const Settings> settings;    // what the broker's callbacks read
    AiToolsHost host;
    bool starting = false;
    bool stopping = false;
};

Service& Get() {
    static auto* service = new Service;  // process lifetime, like the other app services
    return *service;
}

std::shared_ptr<const Settings> CurrentSettings() {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    return s.settings;
}

bool ApproveClient(const std::wstring& client) {
    const auto settings = CurrentSettings();
    if (!settings) return false;
    for (const ClientRecord& record : settings->clients)
        if (record.name == client) return record.grant != ClientGrant::Denied;
    // Not seen before. M5 asks in Pulse on the first connection; until then a
    // verified Pulse.AI.exe may connect: Create still needs an always-allow rule.
    return true;
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
    options.activity_directory = activity;
    // No confirm callback until the confirmation UI (M5): Create runs only
    // under an always-allow rule.
    auto broker = std::make_unique<ToolBroker>(std::move(options));
    AiToolsHost host;
    {
        std::lock_guard lock(s.mu);
        if (s.stopping) return;
        s.settings = settings;
        host = s.host;
    }
    RegisterAiTools(*broker, host);
    DWORD error = ERROR_SUCCESS;
    if (!broker->Start(error)) {
        Event("ai_service_start_failed", {{"error", static_cast<uint64_t>(error)}},
              diagnostics::runtime::Level::Warning);
        return;
    }
    std::unique_lock lock(s.mu);
    if (s.stopping) {
        lock.unlock();
        broker->Stop();
        return;
    }
    s.broker = std::move(broker);
}

} // namespace

void StartAiServiceAsync(const AiToolsHost& host) {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    if (s.broker || s.starting) return;
    s.host = host;
    if (s.starter.joinable()) s.starter.join();   // a finished earlier attempt
    s.starting = true;
    s.stopping = false;
    try {
        s.starter = std::thread([&s] {
            StartNow(s);
            std::lock_guard done(s.mu);
            s.starting = false;
        });
    } catch (...) {
        s.starting = false;
    }
}

void StopAiService() {
    Service& s = Get();
    std::thread starter;
    {
        std::lock_guard lock(s.mu);
        s.stopping = true;
        starter = std::move(s.starter);
    }
    if (starter.joinable()) starter.join();
    std::unique_ptr<ToolBroker> broker;
    {
        std::lock_guard lock(s.mu);
        broker = std::move(s.broker);
        s.settings.reset();
    }
    if (broker) broker->Stop();
}

bool AiServiceRunning() {
    Service& s = Get();
    std::lock_guard lock(s.mu);
    return s.broker && s.broker->running();
}

} // namespace pulse::app::ai
