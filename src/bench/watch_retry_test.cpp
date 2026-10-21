#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <vector>
#include "../app/dir_notify_queue.h"

namespace {
std::atomic<int> attempts{0};
std::atomic<DWORD> injected_error{ERROR_INVALID_FUNCTION};
bool async_failure = false;
HANDLE reached = nullptr, release_read = nullptr;
std::mutex delays_mutex;
std::vector<DWORD> delays;
int wait_limit = 4;
BOOL WINAPI TestRead(HANDLE directory, LPVOID buffer, DWORD length, BOOL subtree,
                    DWORD filter, LPDWORD bytes, LPOVERLAPPED overlapped,
                    LPOVERLAPPED_COMPLETION_ROUTINE completion) {
    const int attempt = ++attempts;
    if (attempt == 20) {
        SetEvent(reached); // Bound the unfixed hot loop without hiding failure.
        WaitForSingleObject(release_read, 5000);
    }
    const DWORD error = injected_error.load();
    if (error == ERROR_SUCCESS || (error == ERROR_BUSY && attempt > 3))
        return ReadDirectoryChangesW(directory, buffer, length, subtree, filter, bytes, overlapped, completion);
    if (async_failure) { SetEvent(overlapped->hEvent); return TRUE; }
    SetLastError(error);
    return FALSE;
}
BOOL WINAPI TestResult(HANDLE directory, LPOVERLAPPED overlapped, LPDWORD bytes, BOOL wait) {
    if (async_failure) { SetLastError(injected_error); return FALSE; }
    return GetOverlappedResult(directory, overlapped, bytes, wait);
}
DWORD WINAPI TestWait(HANDLE event, DWORD duration) {
    if (duration != INFINITE) {
        size_t count;
        { std::lock_guard lock(delays_mutex); delays.push_back(duration); count = delays.size(); }
        if (count >= static_cast<size_t>(wait_limit)) {
            SetEvent(reached);
            return WaitForSingleObject(event, duration); // Stop must wake the real wait.
        }
        return WaitForSingleObject(event, 2); // Keep policy tests fast; record original deadlines.
    }
    return WaitForSingleObject(event, duration);
}
}
#define ReadDirectoryChangesW TestRead
#define GetOverlappedResult TestResult
#define WaitForSingleObject TestWait
#include "../fs/fs_watch.cpp"
#undef ReadDirectoryChangesW
#undef GetOverlappedResult
#undef WaitForSingleObject

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}
void Drain() {
    for (int i = 0; i < 300 && pulse::fs::active_watchers.load(); ++i) Sleep(10);
    Check(pulse::fs::active_watchers.load() == 0, "stopped watcher drains all owned resources");
    if (pulse::fs::active_watchers.load()) std::exit(2);
}
void RunFailure(const std::wstring& path, DWORD error, bool async, int limit) {
    injected_error = error; async_failure = async; attempts = 0; wait_limit = limit;
    { std::lock_guard lock(delays_mutex); delays.clear(); }
    ResetEvent(reached); ResetEvent(release_read);
    std::atomic<int> callbacks{0};
    pulse::fs::DirWatch watch;
    Check(watch.Start(path, [&](bool, auto) { ++callbacks; }), "watcher starts on an owned real directory");
    Check(WaitForSingleObject(reached, 3000) == WAIT_OBJECT_0, "failure policy reaches a bounded checkpoint");
    Check(!watch.Armed(), "failed watcher is explicitly unarmed during retry wait");
    const auto start = GetTickCount64();
    watch.Stop();
    Check(GetTickCount64() - start < 500, "Stop interrupts retry without joining a blocked provider");
    const int stopped_callbacks = callbacks.load();
    SetEvent(release_read);
    Drain();
    Check(callbacks == stopped_callbacks, "no callbacks after Stop");
    std::lock_guard lock(delays_mutex);
    if (error == ERROR_INVALID_FUNCTION) {
        Check(delays == std::vector<DWORD>(static_cast<size_t>(limit), 30000),
              "unsupported notifications use a 30-second cancellable polling interval");
        Check(callbacks <= limit + (async ? 1 : 0), "unsupported errors have bounded refresh notifications");
    } else {
        Check(delays == std::vector<DWORD>({250, 500, 1000, 2000, 4000, 8000, 8000}),
              "transient retry grows exponentially and caps at eight seconds");
        Check(callbacks <= (async ? 2 : 1), "one unavailable episode does not emit a refresh storm");
    }
    Check(attempts == limit, "successful directory reopen cannot bypass the retry delay");
}
void RunRecovery(const std::filesystem::path& root) {
    injected_error = ERROR_BUSY; async_failure = false; attempts = 0; wait_limit = 4;
    { std::lock_guard lock(delays_mutex); delays.clear(); }
    ResetEvent(reached); ResetEvent(release_read);
    HANDLE reconciled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE changed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!reconciled || !changed) std::exit(2);
    pulse::fs::DirWatch watch;
    Check(watch.Start(root.wstring(), [&](bool overflow, auto events) {
        if (overflow && attempts >= 4) SetEvent(reconciled);
        if (!events.empty()) SetEvent(changed);
    }), "transient recovery watcher starts");
    Check(WaitForSingleObject(reconciled, 3000) == WAIT_OBJECT_0 && watch.Armed(),
          "three failures recover to a real pending read and reconcile the missed interval");
    injected_error = ERROR_ACCESS_DENIED;
    HANDLE marker = CreateFileW((root / L"recovered.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(marker != INVALID_HANDLE_VALUE, "create owned file after recovery");
    if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
    Check(WaitForSingleObject(changed, 3000) == WAIT_OBJECT_0, "real local change is delivered after recovery");
    Check(WaitForSingleObject(reached, 3000) == WAIT_OBJECT_0, "next failure reaches interruptible retry");
    watch.Stop(); SetEvent(release_read); Drain();
    { std::lock_guard lock(delays_mutex);
      Check(delays == std::vector<DWORD>({250, 500, 1000, 250}), "healthy read resets backoff for a later failure"); }
    CloseHandle(reconciled); CloseHandle(changed);
    DeleteFileW((root / L"recovered.txt").c_str());
}
// #58-12: a full change buffer is not a failure. Every overflow asks for one
// fresh listing; the handle stays open and armed, with no retry backoff.
void RunOverflow(const std::wstring& path) {
    injected_error = ERROR_NOTIFY_ENUM_DIR; async_failure = true; attempts = 0; wait_limit = 5;
    { std::lock_guard lock(delays_mutex); delays.clear(); }
    ResetEvent(reached); ResetEvent(release_read);
    std::atomic<int> overflows{0}, details{0};
    pulse::fs::DirWatch watch;
    Check(watch.Start(path, [&](bool overflow, auto events) {
        if (overflow) ++overflows;
        if (!events.empty()) ++details;
    }), "OVERFLOW-01 watcher starts");
    Check(WaitForSingleObject(reached, 3000) == WAIT_OBJECT_0, "OVERFLOW-02 repeated overflows keep reading");
    Check(watch.Armed(), "OVERFLOW-03 the watch stays armed through an overflow");
    const auto start = GetTickCount64();
    watch.Stop();
    Check(GetTickCount64() - start < 500, "OVERFLOW-04 Stop interrupts the overflow pause");
    SetEvent(release_read);
    Drain();
    std::lock_guard lock(delays_mutex);
    Check(delays.size() >= 5 && std::all_of(delays.begin(), delays.end(), [](DWORD d) { return d == 100; }),
          "OVERFLOW-05 only the short overflow pause, no failure backoff");
    Check(overflows >= 5 && details == 0, "OVERFLOW-06 each overflow asks for a fresh listing");
}
void RunQueue() {
    struct Batch { std::wstring path; bool overflow; std::vector<pulse::fs::DirNotifyEvent> events; };
    std::vector<Batch> queue;
    const std::vector<pulse::fs::DirNotifyEvent> events{{FILE_ACTION_ADDED, L"item", {}}};
    pulse::app::QueueDirNotify(queue, L"a", false, events);
    pulse::app::QueueDirNotify(queue, L"b", false, events);
    pulse::app::QueueDirNotify(queue, L"a", false, events);
    Check(queue.size() == 3, "ordinary event batches retain their ordering");
    for (int i = 0; i < 10000; ++i) {
        pulse::app::QueueDirNotify(queue, L"a", true, {});
        pulse::app::QueueDirNotify(queue, L"a", false, events);
    }
    Check(queue.size() == 2 && queue[0].path == L"b" && queue[0].events.size() == 1 &&
          queue[1].path == L"a" && queue[1].overflow && queue[1].events.empty(),
          "10000 repeated overflows occupy one batch per path and preserve another path");
    queue.clear();
    pulse::app::QueueDirNotify(queue, L"a", false, events);
    Check(queue.size() == 1 && !queue[0].overflow && queue[0].events.size() == 1,
          "new events are accepted after the queue is drained");
}
}
int wmain() {
    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"watch-retry-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    if (!std::filesystem::create_directory(root)) return 2;
    reached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_read = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!reached || !release_read) return 2;
    RunFailure(root.wstring(), ERROR_INVALID_FUNCTION, false, 4);
    RunFailure(root.wstring(), ERROR_ACCESS_DENIED, false, 7);
    RunFailure(root.wstring(), ERROR_INVALID_FUNCTION, true, 4);
    RunFailure(root.wstring(), ERROR_ACCESS_DENIED, true, 7);
    RunRecovery(root);
    RunOverflow(root.wstring());
    RunQueue();
    CloseHandle(reached); CloseHandle(release_read);
    Check(std::filesystem::remove(root), "owned fixture removed");
    std::cout << "Failures: " << failures << std::endl;
    return failures ? 1 : 0;
}