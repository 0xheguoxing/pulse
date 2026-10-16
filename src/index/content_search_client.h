#pragma once
#include "content_search.h"
#include "content_index.h"
#include "content_result_store.h"
#include "content_search_session.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <map>
namespace pulse::index {
// Finds Pulse.Content.exe when it is not beside the running exe: in releases
// it comes with the content enhancement pack, which pulse.exe verifies. An
// empty path means the pack is missing, outdated or failed verification, and
// the search fails with ERROR_FILE_NOT_FOUND.
using ContentAgentLocator = std::wstring (*)();
void SetContentAgentLocator(ContentAgentLocator locator) noexcept;
// A long-lived user-mode index agent serves cached TEXT queries. Explicit
// transient scans and duplicate scans retain the isolated per-request agent.
// Cancel/SearchAsync only signal and enqueue; neither joins a thread on the UI.
class ContentSearchClient {
public:
    ContentSearchClient() = default;
    ~ContentSearchClient() { Stop(); }
    ContentSearchClient(const ContentSearchClient&) = delete;
    ContentSearchClient& operator=(const ContentSearchClient&) = delete;
    void Start(HWND notify, UINT update_message, bool persistent = true, bool read_only = false);
    void Start(HWND notify, UINT update_message, ContentAgentMode mode);
    void Stop();
    void Suspend();
    void Resume();
    bool InstantMode() const { return mode_ != ContentAgentMode::LegacyWriter; }
    void SearchAsync(ContentSearchRequest request);
    void Cancel(uint64_t session_id = 0);
    bool TakeUpdate(ContentSearchUpdate& update);
    uint64_t CurrentGeneration() const { return generation_.load(); }
    void Configure(const ContentIndexConfig& config);
    ContentIndexConfig GetConfig() const;
    bool ConfigurationReady() const { return config_ready_.load(); }
    ContentIndexStatus GetStatus() const;
    void Pause(bool paused);
    void Rebuild();
private:
    friend struct ContentRefreshTestPeer;
    struct Command { uint32_t type; std::vector<uint8_t> payload; };
    void Worker();
    struct ActiveQuery {
        uint64_t session_id = 0, generation = 0;
        HANDLE cancel = nullptr;
        std::atomic<bool> done{false};
        std::thread thread;
        ~ActiveQuery() { if (thread.joinable()) thread.join(); if (cancel) CloseHandle(cancel); }
    };
    void Run(ContentSearchRequest request, const std::shared_ptr<ActiveQuery>& active);
    bool IsCurrent(uint64_t generation);
    bool EnsurePersistent();
    void Control(const Command& command);
    void Enqueue(Command command);
    void ShutdownAgent();
    void Publish(ContentSearchUpdate update);
    static std::wstring ExePath();
    bool persistent_enabled_ = true;
    bool read_only_ = false;
    ContentAgentMode mode_ = ContentAgentMode::LegacyWriter;
    std::atomic<bool> suspended_{false};
    HWND notify_ = nullptr;
    UINT update_message_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> config_ready_{false};
    std::atomic<uint64_t> generation_{0};
    std::mutex process_mu_;
    std::mutex persistent_mu_;
    HANDLE process_ = nullptr;
    HANDLE persistent_process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE cancel_event_ = nullptr;
    std::wstring persistent_token_;
    std::thread worker_;
    mutable std::mutex state_mu_;
    std::condition_variable wake_;
    std::optional<ContentSearchRequest> pending_;
    std::deque<ContentSearchRequest> pending_requests_;
    std::map<uint64_t, std::shared_ptr<ActiveQuery>> active_queries_;
    std::map<uint64_t, uint64_t> current_sessions_;
    std::deque<Command> commands_;
    ContentIndexConfig config_;
    ContentIndexStatus status_;
    std::mutex updates_mu_;
    std::deque<ContentSearchUpdate> updates_;
};
} // namespace pulse::index
