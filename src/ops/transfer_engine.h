// transfer_engine.h — Parallel copying of small files for OpsManager::RunTransfer.
//
// RunTransfer keeps everything that has to happen in order on its own
// thread: folders (always created before what they contain), conflicts and
// their prompts, links, large files with progress and pause, verification.
// Small files with no conflict are handed to this pool; their results come
// back to the RunTransfer thread, which applies them, so the completed
// lists, undo records and rollback stay in the original order.
#pragma once
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::ops {

// Copy threads for a transfer from `source` into `dest_dir`: several on
// solid-state storage, 2 on a spinning disk or a share, 1 on a removable
// drive. When the storage cannot be identified, 2.
unsigned TransferThreadCount(const std::wstring& source, const std::wstring& dest_dir);

struct TransferTarget {
    bool slow = false;         // a share or a removable drive
    bool spinning = false;     // a fixed disk with a seek penalty (HDD)
    bool block_clone = false;  // same volume, and it clones blocks (ReFS)
};
TransferTarget DescribeTransferTarget(const std::wstring& source, const std::wstring& dest_dir);

struct SmallCopyJob {
    size_t index = 0;           // entry index in RunTransfer
    std::wstring source;
    std::wstring destination;   // known not to exist; its folder exists
    DWORD attributes = 0;       // as scanned
    bool move = false;          // delete the source once copied
};

struct SmallCopyResult {
    size_t index = 0;
    bool ran = false;           // false: dropped after Stop() or cancel
    HRESULT hr = S_OK;          // the copy
    DWORD delete_error = ERROR_SUCCESS;  // the source delete of a move
    bool source_removed = false;
};

class SmallCopyPool {
public:
    // `pause`/`cancel` are RunTransfer's flags; workers check them between
    // files. A paused worker waits on `control_cv` under `control_mutex`.
    SmallCopyPool(unsigned threads, std::atomic<bool>& cancel, std::atomic<bool>& pause,
                  std::mutex& control_mutex, std::condition_variable& control_cv);
    ~SmallCopyPool();
    SmallCopyPool(const SmallCopyPool&) = delete;
    SmallCopyPool& operator=(const SmallCopyPool&) = delete;

    // True when a job can be queued without blocking.
    bool HasRoom() const;
    void Submit(SmallCopyJob job);
    // Finished results so far; with `wait`, blocks until at least one is
    // available or nothing is outstanding.
    std::vector<SmallCopyResult> Take(bool wait);
    size_t Outstanding() const;
    // Queued jobs are dropped (reported with ran = false); running ones finish.
    void Stop();

private:
    void Worker();
    SmallCopyResult Run(const SmallCopyJob& job);

    std::atomic<bool>& cancel_;
    std::atomic<bool>& pause_;
    std::mutex& control_mutex_;
    std::condition_variable& control_cv_;
    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::deque<SmallCopyJob> queue_;
    std::vector<SmallCopyResult> done_;
    size_t outstanding_ = 0;    // queued + running + done but not taken
    size_t capacity_ = 0;
    bool stopping_ = false;
    bool closing_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace pulse::ops
