// transfer_engine.cpp — See transfer_engine.h.
#include "transfer_engine.h"
#include <winioctl.h>
#include <algorithm>
#include <chrono>

namespace pulse::ops {
namespace {

enum class Storage { Solid, Spinning, Remote, Removable, Unknown };

std::wstring VolumeRoot(const std::wstring& path) {
    if (path.empty()) return {};
    std::vector<wchar_t> root(32768);
    if (!GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size()))) return {};
    return root.data();
}

Storage StorageOf(const std::wstring& path) {
    const std::wstring root = VolumeRoot(path);
    if (root.empty()) return Storage::Unknown;
    switch (GetDriveTypeW(root.c_str())) {
    case DRIVE_REMOTE: return Storage::Remote;
    case DRIVE_REMOVABLE:
    case DRIVE_CDROM: return Storage::Removable;
    case DRIVE_FIXED: break;
    default: return Storage::Unknown;
    }
    // \\?\Volume{...}\ works for drive letters and mount points alike; the
    // device is opened without the trailing backslash and without access.
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume, MAX_PATH)) return Storage::Unknown;
    std::wstring device = volume;
    if (!device.empty() && device.back() == L'\\') device.pop_back();
    HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return Storage::Unknown;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                    &seek, sizeof(seek), &returned, nullptr);
    CloseHandle(handle);
    // A volume over several disks has no single answer.
    if (!ok || returned < sizeof(seek)) return Storage::Unknown;
    return seek.IncursSeekPenalty ? Storage::Spinning : Storage::Solid;
}

unsigned ThreadsFor(Storage storage) {
    switch (storage) {
    case Storage::Solid: {
        const unsigned cores = (std::max)(1u, std::thread::hardware_concurrency());
        return (std::min)(8u, (std::max)(2u, 2 * cores));
    }
    case Storage::Removable: return 1;
    default: return 2;
    }
}

constexpr auto kTakeWait = std::chrono::milliseconds(100);

}  // namespace

unsigned TransferThreadCount(const std::wstring& source, const std::wstring& dest_dir) {
    return (std::min)(ThreadsFor(StorageOf(source)), ThreadsFor(StorageOf(dest_dir)));
}

TransferTarget DescribeTransferTarget(const std::wstring& source, const std::wstring& dest_dir) {
    TransferTarget target;
    const Storage storage = StorageOf(dest_dir);
    target.slow = storage == Storage::Remote || storage == Storage::Removable;
    const std::wstring source_root = VolumeRoot(source);
    const std::wstring dest_root = VolumeRoot(dest_dir);
    DWORD flags = 0;
    if (!dest_root.empty() && _wcsicmp(source_root.c_str(), dest_root.c_str()) == 0 &&
        GetVolumeInformationW(dest_root.c_str(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) {
        constexpr DWORD kSupportsBlockRefcounting = 0x08000000;  // FILE_SUPPORTS_BLOCK_REFCOUNTING
        target.block_clone = (flags & kSupportsBlockRefcounting) != 0;
    }
    return target;
}

SmallCopyPool::SmallCopyPool(unsigned threads, std::atomic<bool>& cancel, std::atomic<bool>& pause,
                             std::mutex& control_mutex, std::condition_variable& control_cv)
    : cancel_(cancel), pause_(pause), control_mutex_(control_mutex), control_cv_(control_cv) {
    threads = (std::max)(1u, threads);
    // Deep enough that the transfer thread hands over every small file and
    // reaches the large ones while the pool works.
    capacity_ = (std::max<size_t>)(threads * 4, 1024);
    threads_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { Worker(); });
}

SmallCopyPool::~SmallCopyPool() {
    Stop();
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }
    work_cv_.notify_all();
    for (auto& thread : threads_) thread.join();
}

bool SmallCopyPool::HasRoom() const {
    std::lock_guard lock(mutex_);
    return outstanding_ < capacity_;
}

void SmallCopyPool::Submit(SmallCopyJob job) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            done_.push_back({job.index, false});
        } else {
            queue_.push_back(std::move(job));
        }
        ++outstanding_;
    }
    work_cv_.notify_one();
    done_cv_.notify_all();
}

std::vector<SmallCopyResult> SmallCopyPool::Take(bool wait) {
    std::unique_lock lock(mutex_);
    if (wait) done_cv_.wait_for(lock, kTakeWait, [&] { return !done_.empty() || outstanding_ == 0; });
    std::vector<SmallCopyResult> taken;
    taken.swap(done_);
    outstanding_ -= taken.size();
    return taken;
}

size_t SmallCopyPool::Outstanding() const {
    std::lock_guard lock(mutex_);
    return outstanding_;
}

void SmallCopyPool::Stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        for (const auto& job : queue_) done_.push_back({job.index, false});
        queue_.clear();
    }
    done_cv_.notify_all();
}

void SmallCopyPool::Worker() {
    for (;;) {
        SmallCopyJob job;
        {
            std::unique_lock lock(mutex_);
            work_cv_.wait(lock, [&] { return closing_ || !queue_.empty(); });
            if (queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        // Pause between files, like the RunTransfer thread.
        while (pause_.load() && !cancel_.load()) {
            std::unique_lock control(control_mutex_);
            control_cv_.wait_for(control, kTakeWait, [&] { return cancel_.load() || !pause_.load(); });
        }
        bool dropped = cancel_.load();
        {
            std::lock_guard lock(mutex_);
            dropped = dropped || stopping_;
        }
        SmallCopyResult result = dropped ? SmallCopyResult{job.index, false} : Run(job);
        {
            std::lock_guard lock(mutex_);
            done_.push_back(std::move(result));
        }
        done_cv_.notify_all();
    }
}

SmallCopyResult SmallCopyPool::Run(const SmallCopyJob& job) {
    SmallCopyResult result;
    result.index = job.index;
    result.ran = true;
    // Same flags as the RunTransfer thread's direct copy; links are copied
    // as links, and a file system without COPY_SYMLINK retries without it.
    COPYFILE2_EXTENDED_PARAMETERS parameters{};
    parameters.dwSize = sizeof(parameters);
    parameters.dwCopyFlags = COPY_FILE_COPY_SYMLINK | COPY_FILE_FAIL_IF_EXISTS;
    result.hr = CopyFile2(job.source.c_str(), job.destination.c_str(), &parameters);
    if (result.hr == HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER)) {
        parameters.dwCopyFlags = COPY_FILE_FAIL_IF_EXISTS;
        result.hr = CopyFile2(job.source.c_str(), job.destination.c_str(), &parameters);
    }
    if (FAILED(result.hr)) {
        // A name lost to another writer is that writer's file.
        if (result.hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) &&
            result.hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
            DeleteFileW(job.destination.c_str());
        return result;
    }
    if (job.move) {
        if (job.attributes & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(job.source.c_str(), job.attributes & ~FILE_ATTRIBUTE_READONLY);
        if (DeleteFileW(job.source.c_str())) result.source_removed = true;
        else result.delete_error = GetLastError();
    }
    return result;
}

}  // namespace pulse::ops
