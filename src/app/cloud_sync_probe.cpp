#include "cloud_sync_probe.h"
#include <objbase.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cwctype>
#include <thread>
#include <wrl/client.h>

namespace pulse::app {
namespace {
// {E77E90DF-6271-4F5B-834F-2DD1F245DDA4}, 3: System.StorageProviderState.
constexpr PROPERTYKEY kStorageProviderState = {
    {0xE77E90DF, 0x6271, 0x4F5B, {0x83, 0x4F, 0x2D, 0xD1, 0xF2, 0x45, 0xDD, 0xA4}}, 3};
constexpr DWORD kIdleExitMs = 5000;

// Isolated test instances only: PULSE_TEST_CLOUD_SYNC="deck.pptx=6;a.docx=7"
// answers by file name, so shots can show states no fixture can produce.
// Returns nullptr outside such an instance.
CloudSyncProbe::Query FixtureQuery() {
    if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) == 0) return nullptr;
    wchar_t spec[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_CLOUD_SYNC", spec, ARRAYSIZE(spec));
    if (n == 0 || n >= ARRAYSIZE(spec)) return nullptr;
    std::unordered_map<std::wstring, uint32_t> names;
    std::wstring all(spec);
    size_t start = 0;
    while (start < all.size()) {
        const size_t end = (std::min)(all.find(L';', start), all.size());
        const std::wstring item = all.substr(start, end - start);
        const size_t eq = item.find(L'=');
        if (eq != std::wstring::npos && eq > 0) {
            std::wstring name = item.substr(0, eq);
            for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
            names[name] = static_cast<uint32_t>(_wtoi(item.c_str() + eq + 1));
        }
        start = end + 1;
    }
    return [names](const std::wstring& path) {
        std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
        for (auto& c : name) c = static_cast<wchar_t>(towlower(c));
        const auto it = names.find(name);
        return it != names.end() ? it->second : QueryStorageProviderState(path);
    };
}
} // namespace

uint32_t QueryStorageProviderState(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IShellItem2> item;
    if (path.empty() ||
        FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
        return 0;
    ULONG value = 0;
    return SUCCEEDED(item->GetUInt32(kStorageProviderState, &value)) ? static_cast<uint32_t>(value) : 0;
}

CloudSyncProbe& CloudSyncProbe::Instance() {
    // Never destroyed: the detached worker may still be finishing a query
    // while the process exits.
    static CloudSyncProbe* probe = [] {
        // A fixture answers inline so an offscreen shot's single frame has it.
        if (Query fixture = FixtureQuery()) return new CloudSyncProbe(std::move(fixture), Mode::Inline);
        return new CloudSyncProbe(&QueryStorageProviderState);
    }();
    return *probe;
}

CloudSyncProbe::CloudSyncProbe(Query query, Mode mode)
    : query_(std::move(query)), mode_(mode) {}

fs::CloudSync CloudSyncProbe::Lookup(const std::wstring& path, uint64_t modified, ULONGLONG now) {
    if (path.empty()) return fs::CloudSync::Idle;
    std::lock_guard lock(mutex_);
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxEntries) {
            // Bounded: forget settled rows (they are re-read when drawn again).
            for (auto e = entries_.begin(); e != entries_.end();)
                e = e->second.queued ? std::next(e) : entries_.erase(e);
        }
        it = entries_.emplace(path, Entry{}).first;
        it->second.modified = modified;
        Enqueue(path, it->second);
        return it->second.known ? it->second.state : fs::CloudSync::Idle;
    }
    Entry& entry = it->second;
    if (!entry.queued && (entry.modified != modified ||
                          (entry.state == fs::CloudSync::Syncing && now - entry.checked >= kSyncingRecheckMs))) {
        entry.modified = modified;
        Enqueue(path, entry);
    }
    return entry.known ? entry.state : fs::CloudSync::Idle;
}

uint64_t CloudSyncProbe::Revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
}

void CloudSyncProbe::Enqueue(const std::wstring& path, Entry& entry) {
    if (mode_ == Mode::Inline) {
        const fs::CloudSync state = fs::CloudSyncFromProviderState(query_(path));
        if (!entry.known || entry.state != state) ++revision_;
        entry.known = true;
        entry.state = state;
        entry.checked = GetTickCount64();
        return;
    }
    entry.queued = true;
    queue_.push_back(path);
    if (mode_ != Mode::Background) return;
    if (worker_running_) {
        wake_.notify_one();
        return;
    }
    worker_running_ = true;
    try {
        std::thread([this] { Worker(); }).detach();
    } catch (...) {
        worker_running_ = false;
    }
}

bool CloudSyncProbe::TakeOne(std::wstring& path, uint64_t& modified) {
    while (!queue_.empty()) {
        path = std::move(queue_.front());
        queue_.pop_front();
        const auto it = entries_.find(path);
        if (it == entries_.end()) continue;
        modified = it->second.modified;
        return true;
    }
    return false;
}

void CloudSyncProbe::Store(const std::wstring& path, uint64_t modified, fs::CloudSync state) {
    const auto it = entries_.find(path);
    if (it == entries_.end()) return;
    Entry& entry = it->second;
    entry.queued = false;
    entry.checked = GetTickCount64();
    // A newer modification arrived while reading: read again.
    if (entry.modified != modified) {
        Enqueue(path, entry);
        return;
    }
    if (!entry.known || entry.state != state) ++revision_;
    entry.known = true;
    entry.state = state;
}

void CloudSyncProbe::Worker() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock lock(mutex_);
    for (;;) {
        std::wstring path;
        uint64_t modified = 0;
        if (!TakeOne(path, modified)) {
            if (!wake_.wait_for(lock, std::chrono::milliseconds(kIdleExitMs), [this] { return !queue_.empty(); })) {
                worker_running_ = false;
                break;
            }
            continue;
        }
        lock.unlock();
        const fs::CloudSync state = fs::CloudSyncFromProviderState(query_(path));
        lock.lock();
        Store(path, modified, state);
    }
    lock.unlock();
    if (SUCCEEDED(com)) CoUninitialize();
}

size_t CloudSyncProbe::RunPendingForTest() {
    size_t done = 0;
    std::unique_lock lock(mutex_);
    std::wstring path;
    uint64_t modified = 0;
    while (TakeOne(path, modified)) {
        lock.unlock();
        const fs::CloudSync state = fs::CloudSyncFromProviderState(query_(path));
        lock.lock();
        Store(path, modified, state);
        ++done;
    }
    return done;
}

size_t CloudSyncProbe::QueuedForTest() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

} // namespace pulse::app
