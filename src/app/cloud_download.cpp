#include "cloud_download.h"
#include "../fs/cloud_hydrate.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse::app {
namespace {

struct Job {
    std::wstring path;
    uint64_t size = 0;
    HWND notify = nullptr;
    UINT message = 0;
    LPARAM id = 0;
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> done{0};
    std::mutex thread_mutex;
    HANDLE thread = nullptr;  // the worker's own handle, for CancelSynchronousIo
};

std::mutex g_mutex;
std::shared_ptr<Job> g_job;
LPARAM g_next_id = 0;
// Last finished job, so the UI can map a Done / Failed event to its file.
LPARAM g_last_id = 0;
std::wstring g_last_path;

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void Cancel(const std::shared_ptr<Job>& job) {
    if (!job) return;
    job->cancel = true;
    std::lock_guard lock(job->thread_mutex);
    if (job->thread) CancelSynchronousIo(job->thread);
}

// --test-instance only: stand in for a sync engine (slow chunks so progress
// can be watched; clear OFFLINE afterwards the way a provider clears its
// placeholder bits once the file is here).
DWORD TestDelayMs() {
    wchar_t value[16]{};
    if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return 0;
    if (!GetEnvironmentVariableW(L"PULSE_TEST_HYDRATE_DELAY_MS", value, ARRAYSIZE(value))) return 0;
    return static_cast<DWORD>(std::clamp(_wtoi(value), 0, 5000));
}
bool TestClearsOffline() {
    return GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0 &&
           GetEnvironmentVariableW(L"PULSE_TEST_HYDRATE_CLEAR", nullptr, 0) > 0;
}

void Run(const std::shared_ptr<Job>& job) {
    {
        std::lock_guard lock(job->thread_mutex);
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &job->thread, 0,
                        FALSE, DUPLICATE_SAME_ACCESS);
    }
    const DWORD delay = TestDelayMs();
    ULONGLONG last_post = 0;
    DWORD error = ERROR_SUCCESS;
    const fs::HydrateResult result = fs::HydrateByReading(job->path, job->cancel, [&](uint64_t bytes) {
        job->done.store(bytes, std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        if (now - last_post >= 120) {
            last_post = now;
            PostMessageW(job->notify, job->message, kCloudDownloadProgress, job->id);
        }
        if (delay) Sleep(delay);
    }, &error);
    if (result == fs::HydrateResult::Done && TestClearsOffline()) {
        const DWORD attrs = GetFileAttributesW(job->path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_OFFLINE))
            SetFileAttributesW(job->path.c_str(), attrs & ~FILE_ATTRIBUTE_OFFLINE);
    }
    {
        std::lock_guard lock(job->thread_mutex);
        if (job->thread) { CloseHandle(job->thread); job->thread = nullptr; }
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_job == job) g_job.reset();
        g_last_id = job->id;
        g_last_path = job->path;
    }
    const WPARAM event = result == fs::HydrateResult::Done ? kCloudDownloadDone
                       : result == fs::HydrateResult::Cancelled ? kCloudDownloadCancelled
                       : kCloudDownloadFailed;
    PostMessageW(job->notify, job->message, event, job->id);
}

} // namespace

bool StartCloudDownload(const std::wstring& path, uint64_t size, HWND notify, UINT message) {
    if (path.empty() || !notify || !message) return false;
    auto job = std::make_shared<Job>();
    job->path = path;
    job->size = size;
    job->notify = notify;
    job->message = message;
    std::shared_ptr<Job> previous;
    {
        std::lock_guard lock(g_mutex);
        if (g_job && SamePath(g_job->path, path)) return true;  // already on its way
        previous = std::move(g_job);
        job->id = ++g_next_id;
        g_job = job;
    }
    Cancel(previous);
    try {
        std::thread([job] { Run(job); }).detach();
    } catch (...) {
        std::lock_guard lock(g_mutex);
        if (g_job == job) g_job.reset();
        return false;
    }
    return true;
}

int CloudDownloadPercent(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    if (!g_job || path.empty() || !SamePath(g_job->path, path)) return -1;
    const uint64_t done = g_job->done.load(std::memory_order_relaxed);
    if (!g_job->size) return 0;
    return static_cast<int>((std::min)(uint64_t{100}, done * 100 / g_job->size));
}

std::wstring CloudDownloadPathFor(LPARAM job) {
    std::lock_guard lock(g_mutex);
    if (g_job && g_job->id == job) return g_job->path;
    return job == g_last_id ? g_last_path : std::wstring();
}

void CancelCloudDownloads() {
    std::shared_ptr<Job> job;
    {
        std::lock_guard lock(g_mutex);
        job = std::move(g_job);
    }
    Cancel(job);
}

} // namespace pulse::app
