// device_registry.cpp - Connected phones (device_registry.h).
#include "device_registry.h"
#include "app_messages.h"

#include <dbt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <cstdio>
#include <cwctype>
#include <string>

namespace pulse::app {

namespace {

constexpr GUID kWpdInterfaceClass = {0x6AC27878, 0xA6FA, 0x4155, {0xBA, 0x85, 0xF9, 0x8F, 0x49, 0x1D, 0x4F, 0x33}};
constexpr uint32_t kQuietMs = 500;          // arrivals come in bursts (device + storages)
constexpr uint32_t kLockedRecheckMs = 2000; // waiting for 允许 / 信任 on the phone
constexpr uint32_t kLockedChecksMax = 150;  // 5 minutes
constexpr uint32_t kListBudgetMs = 15000;   // a list call stuck in the driver
constexpr uint32_t kFakePollMs = 1000;      // test source: no notifications, so poll

std::wstring EnvValue(const wchar_t* name) {
    wchar_t value[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
    return n > 0 && n < ARRAYSIZE(value) ? std::wstring(value, n) : std::wstring();
}

const wchar_t* KindName(fs::DeviceKind kind) {
    switch (kind) {
    case fs::DeviceKind::IPhone: return L"iphone";
    case fs::DeviceKind::Other: return L"other";
    default: return L"android";
    }
}

// Isolated test instances: the published list as text, for end-to-end checks.
void WriteTestDeviceList(const std::vector<fs::DeviceInfo>& devices, uint64_t revision) {
    const std::wstring dir = EnvValue(L"PULSE_TEST_DATA_DIR");
    if (dir.empty()) return;
    std::wstring text = L"revision=" + std::to_wstring(revision) + L"\n";
    for (const auto& device : devices) {
        text += L"device " + device.key + L" name=" + device.name + L" kind=" + KindName(device.kind) +
                L" locked=" + (device.locked ? L"1" : L"0") + L" storages=" +
                std::to_wstring(device.storages.size()) + L" open=" + fs::DeviceOpenTarget(device) + L"\n";
        for (const auto& storage : device.storages)
            text += L"  storage " + storage.name + L" fs=" + storage.file_system + L" total=" +
                    std::to_wstring(storage.total) + L" free=" + std::to_wstring(storage.free) + L" ro=" +
                    (storage.read_only ? L"1" : L"0") + L" removable=" + (storage.removable ? L"1" : L"0") + L"\n";
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes, nullptr,
                        nullptr);
    const std::wstring file = dir + L"\\devices.txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}

} // namespace

void CALLBACK DeviceRegistry::OnTimer(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER) {
    // A stuck list call (driver not answering) keeps the last list; the next
    // arrival or locked re-check asks again on a fresh thread.
    Instance().worker_.Post([] { Instance().RefreshOnWorker(); }, kListBudgetMs);
}

DeviceRegistry& DeviceRegistry::Instance() {
    static DeviceRegistry* registry = new DeviceRegistry();  // lives until the process ends
    return *registry;
}

DeviceRegistry::DeviceRegistry() = default;

void DeviceRegistry::Start(HWND notify) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return;
        started_ = true;
        notify_ = notify;
        // Tests only: a local folder per phone instead of the shell.
        if (!EnvValue(L"PULSE_TEST_DATA_DIR").empty()) {
            fake_root_ = EnvValue(L"PULSE_TEST_FAKE_DEVICE");
            test_source_ = !fake_root_.empty();
        }
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = kWpdInterfaceClass;
        if (notify && !test_source_)
            registration_ = RegisterDeviceNotificationW(notify, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        timer_ = CreateThreadpoolTimer(OnTimer, nullptr, nullptr);
    }
    if (test_source_ || fs::AnyPortableDevicePresent()) ScheduleRefresh(0);
}

void DeviceRegistry::Stop() {
    PTP_TIMER timer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || !started_) return;
        stopped_ = true;
        if (registration_) UnregisterDeviceNotification(registration_);
        registration_ = nullptr;
        notify_ = nullptr;
        timer = timer_;
        timer_ = nullptr;
    }
    if (timer) {
        SetThreadpoolTimer(timer, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(timer, TRUE);
        CloseThreadpoolTimer(timer);
    }
    worker_.Stop(300);
}

void DeviceRegistry::OnDevicesBroadcast() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        locked_checks_ = 0;
    }
    ScheduleRefresh(kQuietMs);
}

void DeviceRegistry::ScheduleRefresh(uint32_t delay_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!timer_ || stopped_) return;
    // Re-arming replaces the due time: a burst of broadcasts lists once.
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(delay_ms == 0 ? 1 : delay_ms) * 10000);
    FILETIME ft{due.LowPart, due.HighPart};
    SetThreadpoolTimer(timer_, &ft, 0, 0);
}

fs::DeviceSource* DeviceRegistry::SourceOnWorker() {
    if (!source_) source_ = test_source_ ? fs::MakeFakeDeviceSource(fake_root_) : fs::MakeShellDeviceSource();
    return source_.get();
}

void DeviceRegistry::RefreshOnWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
    }
    Publish(SourceOnWorker()->ListDevices());
}

void DeviceRegistry::Publish(std::vector<fs::DeviceInfo> devices) {
    bool changed = false;
    bool recheck = false;
    bool poll = false;
    HWND notify = nullptr;
    uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        changed = devices != devices_ || revision_ == 0;
        if (changed) {
            devices_ = devices;
            ++revision_;
            folder_ids_.clear();  // ids belong to one connection
            for (const auto& device : devices_)
                known_[device.key] = Known{device.name, fs::SkipsStorageLevel(device)};
        }
        revision = revision_;
        notify = notify_;
        const bool any_locked = std::any_of(devices_.begin(), devices_.end(),
                                            [](const fs::DeviceInfo& d) { return d.locked; });
        recheck = any_locked && locked_checks_ < kLockedChecksMax;
        if (recheck) ++locked_checks_;
        poll = test_source_;
    }
    if (changed) {
        if (test_source_ || !EnvValue(L"PULSE_TEST_DATA_DIR").empty()) WriteTestDeviceList(devices, revision);
        if (notify) PostMessageW(notify, WM_DEVICES_CHANGED, 0, 0);
    }
    if (recheck) ScheduleRefresh(kLockedRecheckMs);
    else if (poll) ScheduleRefresh(kFakePollMs);  // fake phones come and go as folders
}

fs::DeviceError DeviceRegistry::ListFolder(const std::wstring& path, std::vector<fs::DirEntry>& out,
                                           const std::function<bool()>& cancelled) {
    const auto folder = fs::ParseDevicePath(path);
    if (!folder) return fs::DeviceError::NotFound;
    std::optional<fs::DeviceInfo> device;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return fs::DeviceError::Cancelled;
        for (const auto& d : devices_)
            if (d.key == folder->device) device = d;
    }
    if (!device) return fs::DeviceError::Disconnected;
    if (device->locked) return fs::DeviceError::Locked;
    if (folder->IsDeviceRoot()) {
        // A phone with several storages (phone + SD card): one folder each.
        for (const auto& storage : device->storages) {
            fs::DirEntry e;
            e.name = storage.name;
            e.full_path = fs::FormatDevicePath(fs::DeviceChild(*folder, storage.name));
            e.is_dir = true;
            e.attrs = FILE_ATTRIBUTE_DIRECTORY | (storage.read_only ? FILE_ATTRIBUTE_READONLY : 0);
            e.shell_id = storage.id;
            out.push_back(std::move(e));
        }
        RememberFolderIds(path, L"", out);
        return fs::DeviceError::None;
    }
    // Below a storage: the device thread lists it. The result comes back
    // through `job`; a stuck call or a cancelled view just stops waiting.
    struct Job {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        fs::DeviceError error = fs::DeviceError::None;
        std::vector<fs::DirEntry> rows;
        std::atomic_bool cancel{false};
    };
    auto job = std::make_shared<Job>();
    worker_.Post(
        [this, job, where = *folder] {
            std::vector<fs::DirEntry> rows;
            fs::DeviceSource* source = SourceOnWorker();
            const fs::DeviceError error = source->Enumerate(
                where,
                [&](std::vector<fs::DirEntry>&& batch) {
                    for (auto& e : batch) rows.push_back(std::move(e));
                    return !job->cancel.load();
                },
                job->cancel);
            if (error == fs::DeviceError::None)
                RememberFolderIds(fs::FormatDevicePath(where), source->ShellId(where), rows);
            std::lock_guard<std::mutex> lock(job->mutex);
            if (job->done) return;
            job->done = true;
            job->error = error;
            job->rows = std::move(rows);
            job->cv.notify_all();
        },
        kListBudgetMs,
        [job] {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->cancel = true;
            if (job->done) return;
            job->done = true;
            job->error = fs::DeviceError::TimedOut;
            job->cv.notify_all();
        });
    std::unique_lock<std::mutex> lock(job->mutex);
    while (!job->done) {
        job->cv.wait_for(lock, std::chrono::milliseconds(100));
        if (job->done) break;
        lock.unlock();
        const bool stop = cancelled && cancelled();
        lock.lock();
        if (stop && !job->done) {
            job->cancel = true;
            job->done = true;  // the task drops its rows when it finishes
            return fs::DeviceError::Cancelled;
        }
    }
    if (job->error == fs::DeviceError::None) out = std::move(job->rows);
    return job->error;
}

namespace {
std::wstring FolderIdKey(std::wstring path) {
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    for (auto& c : path) c = static_cast<wchar_t>(towlower(c));
    return path;
}
} // namespace

void DeviceRegistry::RememberFolderIds(const std::wstring& folder, const std::wstring& folder_id,
                                       const std::vector<fs::DirEntry>& rows) {
    constexpr size_t kMaxFolderIds = 20000;  // a phone's folder tree, seen this session
    std::lock_guard<std::mutex> lock(mutex_);
    if (folder_ids_.size() > kMaxFolderIds) folder_ids_.clear();
    if (!folder_id.empty()) folder_ids_[FolderIdKey(folder)] = folder_id;
    for (const auto& e : rows) {
        if (!e.is_dir || e.shell_id.empty() || e.full_path.empty()) continue;
        folder_ids_[FolderIdKey(e.full_path)] = e.shell_id;
    }
}

std::wstring DeviceRegistry::FolderShellId(const std::wstring& path) const {
    const auto where = fs::ParseDevicePath(path);
    if (!where || where->IsDeviceRoot()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = folder_ids_.find(FolderIdKey(fs::FormatDevicePath(*where))); it != folder_ids_.end())
        return it->second;
    if (where->IsStorageRoot()) {
        for (const auto& device : devices_) {
            if (device.key != where->device) continue;
            for (const auto& storage : device.storages)
                if (_wcsicmp(storage.name.c_str(), where->storage.c_str()) == 0) return storage.id;
        }
    }
    return {};
}

bool DeviceRegistry::FolderWritable(const std::wstring& path) const {
    const auto where = fs::ParseDevicePath(path);
    if (!where || where->IsDeviceRoot() || where->storage.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& device : devices_) {
        if (device.key != where->device) continue;
        if (device.locked || device.kind == fs::DeviceKind::IPhone) return false;
        for (const auto& storage : device.storages)
            if (_wcsicmp(storage.name.c_str(), where->storage.c_str()) == 0) return !storage.read_only;
    }
    return false;
}

bool DeviceRegistry::Describe(const std::wstring& key, std::wstring* name, bool* skips_storage) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = known_.find(key);
    if (it == known_.end()) return false;
    if (name) *name = it->second.name;
    if (skips_storage) *skips_storage = it->second.skips_storage;
    return true;
}

std::vector<fs::DeviceInfo> DeviceRegistry::Devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

uint64_t DeviceRegistry::Revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

} // namespace pulse::app
