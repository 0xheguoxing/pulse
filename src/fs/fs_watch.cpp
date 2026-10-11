// fs_watch.cpp
#include "fs_watch.h"
#include "fs_enum.h"
#include <winioctl.h>
#include <algorithm>

namespace pulse::fs {

namespace {

std::vector<DirNotifyEvent> ParseNotifyBuffer(const BYTE* data, DWORD bytes,
                                              std::wstring& pending_old, bool emit_old) {
    std::vector<DirNotifyEvent> out;
    if (!data || bytes < sizeof(FILE_NOTIFY_INFORMATION)) return out;
    const BYTE* p = data;
    const BYTE* end = data + bytes;
    while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const size_t name_chars = info->FileNameLength / sizeof(WCHAR);
        const BYTE* name_end = reinterpret_cast<const BYTE*>(info->FileName) + info->FileNameLength;
        if (name_end > end) break;
        std::wstring name(info->FileName, name_chars);
        if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            pending_old = std::move(name);
            if (emit_old) out.push_back({FILE_ACTION_RENAMED_OLD_NAME, pending_old, {}});
        } else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            DirNotifyEvent ev;
            ev.action = FILE_ACTION_RENAMED_NEW_NAME;
            ev.name = std::move(name);
            ev.old_name = std::move(pending_old);
            pending_old.clear();
            out.push_back(std::move(ev));
        } else {
            DirNotifyEvent ev;
            ev.action = info->Action;
            ev.name = std::move(name);
            out.push_back(std::move(ev));
        }
        if (info->NextEntryOffset == 0) break;
        p += info->NextEntryOffset;
    }
    return out;
}

} // namespace

namespace { std::atomic<unsigned> active_watchers{0}; }

struct DirWatch::State {
    std::wstring path_;
    bool subtree_ = false;
    ChangeCallback callback_;
    std::mutex callback_mutex_;
    std::atomic<HANDLE> hDir_{INVALID_HANDLE_VALUE};
    HANDLE hStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> running_{true};
    std::atomic<bool> armed_{false};
    OVERLAPPED overlapped_{};
    BY_HANDLE_FILE_INFORMATION identity_{};
    bool identity_valid_ = false;
    std::wstring pending_rename_old_;
    DWORD buffer_[64 * 1024 / sizeof(DWORD)]{};
    // Handle lease (an RH oplock) on hDir_. Without it the open handle made
    // renaming or moving this folder's parents fail with ERROR_ACCESS_DENIED,
    // for Pulse's own operations and for every other program. Whoever needs
    // the handle gone breaks the lease and waits; the watch closes it, steps
    // aside and looks again. Volumes that grant no lease are watched as before.
    HANDLE lease_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED lease_overlapped_{};
    REQUEST_OPLOCK_OUTPUT_BUFFER lease_output_{};
    bool lease_pending_ = false;
    ~State() {
        const HANDLE directory = hDir_.load();
        if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory);
        if (hStop_) CloseHandle(hStop_);
        if (lease_event_) CloseHandle(lease_event_);
        --active_watchers;
    }
    static HANDLE OpenDirectory(const std::wstring& path);
    static bool ReadIdentity(HANDLE handle, BY_HANDLE_FILE_INFORMATION& identity);
    bool WatchedPathReplaced() const;
    bool ReopenDirectory();
    void RequestLease(HANDLE directory, DWORD level, DWORD flags);
    void DropLease(HANDLE directory);
    bool LeaseBreakWantsHandle(HANDLE directory);
    void Notify(bool overflow, std::vector<DirNotifyEvent> events);
    void WorkerThread();
};

DirWatch::DirWatch() = default;
DirWatch::~DirWatch() { Stop(); }

bool DirWatch::Start(const std::wstring& path, ChangeCallback cb, bool subtree) {
    Stop();
    if (path.empty()) return false;
    // Bound retired workers even when a network provider ignores cancellation.
    if (active_watchers.fetch_add(1) >= 128) { --active_watchers; return false; }
    std::shared_ptr<State> state;
    try { state = std::make_shared<State>(); }
    catch (...) { --active_watchers; return false; }
    if (!state->hStop_) return false;
    path_ = NormalizePath(path);
    state->path_ = path_;
    state->subtree_ = subtree;
    state->callback_ = std::move(cb);
    try { thread_ = std::thread([state] { state->WorkerThread(); }); }
    catch (...) { return false; }
    state_ = std::move(state);
    return true;
}

bool DirWatch::Armed() const { return state_ && state_->armed_.load(); }

void DirWatch::Stop() {
    if (!state_) return;
    state_->running_ = false;
    SetEvent(state_->hStop_);
    {
        // Callbacks only dispatch notifications. Quiesce them before owners die.
        std::lock_guard lock(state_->callback_mutex_);
        state_->callback_ = {};
    }
    if (thread_.joinable()) {
        CancelSynchronousIo(thread_.native_handle());
        thread_.detach();
    }
    state_.reset(); // The worker owns handles and pending OVERLAPPED until drained.
}

HANDLE DirWatch::State::OpenDirectory(const std::wstring& path) {
    return CreateFileW(
        path.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
}

bool DirWatch::State::ReadIdentity(HANDLE handle, BY_HANDLE_FILE_INFORMATION& identity) {
    return handle != INVALID_HANDLE_VALUE &&
           GetFileInformationByHandle(handle, &identity) != FALSE;
}

bool DirWatch::State::WatchedPathReplaced() const {
    if (!identity_valid_) return false;
    HANDLE current = OpenDirectory(path_);
    if (current == INVALID_HANDLE_VALUE) return true;
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool readable = ReadIdentity(current, identity);
    CloseHandle(current);
    if (!readable) return true;
    return identity.dwVolumeSerialNumber != identity_.dwVolumeSerialNumber ||
           identity.nFileIndexHigh != identity_.nFileIndexHigh ||
           identity.nFileIndexLow != identity_.nFileIndexLow;
}

bool DirWatch::State::ReopenDirectory() {
    armed_ = false;
    const HANDLE old = hDir_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    DropLease(old);
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    identity_valid_ = false;

    while (running_) {
        HANDLE directory = OpenDirectory(path_);
        if (directory != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION identity{};
            if (ReadIdentity(directory, identity)) {
                identity_ = identity;
                identity_valid_ = true;
                hDir_.store(directory, std::memory_order_release);
                RequestLease(directory, OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE,
                             REQUEST_OPLOCK_INPUT_FLAG_REQUEST);
                return true;
            }
            CloseHandle(directory);
        }
        if (WaitForSingleObject(hStop_, 1000) == WAIT_OBJECT_0) break;
    }
    return false;
}

void DirWatch::State::RequestLease(HANDLE directory, DWORD level, DWORD flags) {
    lease_pending_ = false;
    // A share keeps its handle semantics on the server.
    if (!lease_event_ || directory == INVALID_HANDLE_VALUE || path_.starts_with(L"\\\\?\\UNC\\") ||
        !path_.starts_with(L"\\\\?\\"))
        return;
    REQUEST_OPLOCK_INPUT_BUFFER input{};
    input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    input.StructureLength = sizeof(input);
    input.RequestedOplockLevel = level;
    input.Flags = flags;
    lease_output_ = {};
    ZeroMemory(&lease_overlapped_, sizeof(lease_overlapped_));
    lease_overlapped_.hEvent = lease_event_;
    ResetEvent(lease_event_);
    const BOOL done = DeviceIoControl(directory, FSCTL_REQUEST_OPLOCK, &input, sizeof(input), &lease_output_,
                                      sizeof(lease_output_), nullptr, &lease_overlapped_);
    // Pending is the granted state: the request completes when the lease breaks.
    lease_pending_ = !done && GetLastError() == ERROR_IO_PENDING;
}

void DirWatch::State::DropLease(HANDLE directory) {
    if (!lease_pending_) return;
    lease_pending_ = false;
    CancelIoEx(directory, &lease_overlapped_);
    WaitForSingleObject(lease_event_, INFINITE);  // the kernel is done with the OVERLAPPED
}

// The lease broke. A break that keeps read caching and takes the handle away
// means a rename or move of this folder or a folder above it, or an open that
// conflicts with ours, is waiting for the handle. A folder's contents changing
// breaks the whole lease instead: keep the handle and its pending read, renew.
bool DirWatch::State::LeaseBreakWantsHandle(HANDLE directory) {
    lease_pending_ = false;
    if (lease_overlapped_.Internal != 0) return false;  // refused or cancelled: watch without one
    const DWORD level = lease_output_.NewOplockLevel;
    if ((level & OPLOCK_LEVEL_CACHE_READ) && !(level & OPLOCK_LEVEL_CACHE_HANDLE)) return true;
    if (lease_output_.Flags & REQUEST_OPLOCK_OUTPUT_FLAG_ACK_REQUIRED) {
        // An acknowledgment to a level above none stays pending for the next break.
        RequestLease(directory, level, REQUEST_OPLOCK_INPUT_FLAG_ACK);
        if (lease_pending_) return false;
    }
    RequestLease(directory, OPLOCK_LEVEL_CACHE_READ | OPLOCK_LEVEL_CACHE_HANDLE, REQUEST_OPLOCK_INPUT_FLAG_REQUEST);
    return false;
}

void DirWatch::State::Notify(bool overflow, std::vector<DirNotifyEvent> events) {
    std::lock_guard lock(callback_mutex_);
    if (!running_ || !callback_) return;
    try {
        callback_(overflow, std::move(events));
    } catch (...) {
    }
}

void DirWatch::State::WorkerThread() {
    if (!ReopenDirectory()) return;
    bool newly_armed = true;
    DWORD retry_delay = 250;
    bool failure_notified = false;
    const auto recovered = [&] {
        if (failure_notified) Notify(true, {});
        failure_notified = false;
        retry_delay = 250;
        // Publish readiness only after the missed-change reconciliation has
        // been delivered. A queued read alone can still fail immediately.
        armed_ = true;
    };
    const auto retry_failure = [&](DWORD error) {
        armed_ = false;
        pending_rename_old_.clear();
        if (!failure_notified) Notify(true, {});
        failure_notified = true;
        const bool unsupported = error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED;
        const DWORD delay = unsupported ? 30000 : retry_delay;
        if (WaitForSingleObject(hStop_, delay) != WAIT_TIMEOUT || !running_) return false;
        if (unsupported) Notify(true, {}); // Bounded fallback for local providers as well as UNC.
        else retry_delay = (std::min)(retry_delay * 2, DWORD{8000});
        return ReopenDirectory();
    };
    while (running_) {
        ZeroMemory(&overlapped_, sizeof(overlapped_));
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped_.hEvent) break;
        HANDLE events[3] = { hStop_, overlapped_.hEvent, lease_event_ };
        const HANDLE directory = hDir_.load(std::memory_order_acquire);
        if (directory == INVALID_HANDLE_VALUE) {
            CloseHandle(overlapped_.hEvent);
            break;
        }

        BOOL ok = ReadDirectoryChangesW(
            directory,
            buffer_,
            static_cast<DWORD>(sizeof(buffer_)),
            subtree_ ? TRUE : FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
                FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_ATTRIBUTES |
                FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE |
                FILE_NOTIFY_CHANGE_CREATION |
                FILE_NOTIFY_CHANGE_SECURITY,
            nullptr,
            &overlapped_,
            nullptr);

        const DWORD read_error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && read_error != ERROR_IO_PENDING) {
            CloseHandle(overlapped_.hEvent);
            overlapped_.hEvent = nullptr;
            if (!running_) break;
            if (!retry_failure(read_error)) break;
            newly_armed = true;
            continue;
        }

        // An initial/reopened watch may have missed changes while
        // the handle was opening. Reconcile only after the read is armed.
        if (!failure_notified) armed_ = true;
        if (newly_armed && !failure_notified) Notify(true, {});
        newly_armed = false;

        bool replaced = false;
        bool stopped = false;
        bool stepped_aside = false;
        for (;;) {
            const DWORD wait = WaitForMultipleObjects(lease_pending_ ? 3 : 2, events, FALSE, 500);
            if (wait == WAIT_OBJECT_0 + 2) {
                if (!LeaseBreakWantsHandle(directory)) continue;
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                stepped_aside = true;
                break;
            }
            if (wait == WAIT_TIMEOUT) {
                if (!WatchedPathReplaced()) {
                    recovered(); // A pending read survived its first identity check.
                    continue;
                }
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                replaced = true;
                break;
            }
            if (wait == WAIT_OBJECT_0) {
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
                stopped = true;
                break;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIoEx(directory, &overlapped_);
                WaitForSingleObject(overlapped_.hEvent, INFINITE);
            }
            break;
        }

        DWORD transferred = 0;
        const BOOL got = GetOverlappedResult(directory, &overlapped_, &transferred, FALSE);
        const DWORD result_error = got ? ERROR_SUCCESS : GetLastError();
        CloseHandle(overlapped_.hEvent);
        overlapped_.hEvent = nullptr;

        if (stopped || !running_) break;

        if (stepped_aside) {
            // Close now so the waiting rename or move goes ahead, and let it
            // finish before opening the path again.
            armed_ = false;
            pending_rename_old_.clear();
            const HANDLE old = hDir_.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
            if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
            if (WaitForSingleObject(hStop_, 300) != WAIT_TIMEOUT || !running_) break;
            Notify(true, {});
            if (!ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        if (replaced) {
            pending_rename_old_.clear();
            Notify(true, {});
            if (!ReopenDirectory()) break;
            newly_armed = true;
            continue;
        }

        if (!got) {
            if (!retry_failure(result_error)) break;
            newly_armed = true;
            continue;
        }

        recovered();
        if (transferred == 0) {
            pending_rename_old_.clear();
            Notify(true, {});
            continue;
        }
        Notify(false, ParseNotifyBuffer(reinterpret_cast<const BYTE*>(buffer_), transferred,
                                        pending_rename_old_, subtree_));
    }
    armed_ = false;
    DropLease(hDir_.load(std::memory_order_acquire));
}

void DirWatchSet::Sync(const std::vector<std::wstring>& paths, Callback cb) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }
    std::vector<std::wstring> wanted;
    wanted.reserve(paths.size());
    for (const auto& path : paths) {
        if (path.empty() || IsVirtualPath(path)) continue;
        wanted.push_back(NormalizePath(path));
    }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    for (auto it = watches_.begin(); it != watches_.end();) {
        if (std::binary_search(wanted.begin(), wanted.end(), it->first)) ++it;
        else it = watches_.erase(it);
    }
    for (const auto& path : wanted) {
        if (watches_.contains(path)) continue;
        auto watch = std::make_unique<DirWatch>();
        const std::wstring key = path;
        if (!watch->Start(path, [this, key](bool overflow, std::vector<DirNotifyEvent> events) {
            Callback invoke;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                invoke = callback_;
            }
            if (invoke) invoke(key, overflow, std::move(events));
        })) {
            continue;
        }
        watches_[path] = std::move(watch);
    }
}

void DirWatchSet::Stop() {
    watches_.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = {};
}

bool DirWatchSet::Armed(const std::wstring& path) const {
    const auto it = watches_.find(NormalizePath(path));
    return it != watches_.end() && it->second && it->second->Armed();
}

} // namespace pulse::fs
