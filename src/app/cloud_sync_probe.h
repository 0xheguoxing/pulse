#pragma once
// cloud_sync_probe.h - "syncing" / "sync error" for the rows of a cloud folder
// that are actually on screen. File attributes already say online-only / local
// / always kept; only the sync engine knows about transfers and errors, and it
// publishes that as System.StorageProviderState on the shell item. Reading it
// is metadata only (Explorer does it for every visible item) and never
// downloads content.
//
// The renderer asks for the rows it draws; unknown or changed rows (path +
// modified time) are queued to one background thread that exits when idle.
// Syncing rows are re-read every two seconds so they settle on their own.
// Revision() moves when any answer changes, so a window repaints only then.
#include "../fs/cloud_state.h"
#include <windows.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pulse::app {

class CloudSyncProbe {
public:
    using Query = std::function<uint32_t(const std::wstring& path)>; // raw STORAGEPROVIDERSTATE

    static CloudSyncProbe& Instance();

    // Background: one worker thread. Manual: tests drive the queue with
    // RunPendingForTest. Inline: answered during Lookup (fixture shots only).
    enum class Mode : uint8_t { Background, Manual, Inline };
    explicit CloudSyncProbe(Query query, Mode mode = Mode::Background);
    CloudSyncProbe(const CloudSyncProbe&) = delete;
    CloudSyncProbe& operator=(const CloudSyncProbe&) = delete;

    // UI thread, per drawn row. Answers from the cache (Idle until known) and
    // queues the row when it is new, modified, or a syncing row due a re-read.
    fs::CloudSync Lookup(const std::wstring& path, uint64_t modified, ULONGLONG now = GetTickCount64());
    uint64_t Revision() const;

    // Test hooks.
    size_t RunPendingForTest();
    size_t QueuedForTest() const;
    static constexpr ULONGLONG kSyncingRecheckMs = 2000;
    static constexpr size_t kMaxEntries = 4096;

private:
    struct Entry {
        uint64_t modified = 0;
        fs::CloudSync state = fs::CloudSync::Idle;
        ULONGLONG checked = 0;
        bool queued = false;
        bool known = false;
    };
    void Enqueue(const std::wstring& path, Entry& entry);
    bool TakeOne(std::wstring& path, uint64_t& modified);
    void Store(const std::wstring& path, uint64_t modified, fs::CloudSync state);
    void Worker();

    Query query_;
    Mode mode_ = Mode::Background;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<std::wstring, Entry> entries_;
    std::deque<std::wstring> queue_;
    uint64_t revision_ = 0;
    bool worker_running_ = false;
};

// The real query: System.StorageProviderState of the shell item, 0 on failure.
uint32_t QueryStorageProviderState(const std::wstring& path);

} // namespace pulse::app
