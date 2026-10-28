#include "elevated_transfer.h"
#include "source_listing.h"
#include "../common/path_utils.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <filesystem>
#include <new>
#include <unordered_map>
#include <utility>
#include <sherrors.h>

namespace pulse::ops {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
DWORD ProbeAccess(const std::wstring& path, DWORD access) {
    HANDLE handle = CreateFileW(path.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(handle);
    return ERROR_SUCCESS;
}
bool Denied(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
}
std::wstring ItemPath(IShellItem* item) {
    if (!item) return {};
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return {};
    std::wstring result;
    try { if (raw) result = raw; }
    catch (...) { CoTaskMemFree(raw); throw; }
    CoTaskMemFree(raw);
    return result;
}
class Sink final : public IFileOperationProgressSink {
public:
    Sink(ShellTransferResult& result, std::atomic<bool>& cancel,
         const std::vector<std::wstring>& sources, ShellTransferProgress progress)
        : result_(result), cancel_(cancel), sources_(sources), progress_(std::move(progress)) {}
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileOperationProgressSink) return E_NOINTERFACE;
        *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG refs = --refs_; if (!refs) delete this; return refs;
    }
    HRESULT CheckCancel() const { return cancel_.load() ? kCancelled : S_OK; }
    IFACEMETHODIMP StartOperations() override { return CheckCancel(); }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override {
        if (FAILED(hr) && SUCCEEDED(result_.hr)) result_.hr = hr;
        return S_OK;
    }
    HRESULT Before(IShellItem* item) noexcept {
        try {
            current_ = ItemPath(item);
            ComPtr<IUnknown> identity;
            if (item && SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&identity))))
                pending_.push_back({std::move(identity), current_});
            if (progress_) progress_(current_, -1.0f);
        }
        catch (...) { return E_OUTOFMEMORY; }
        return CheckCancel();
    }
    HRESULT After(DWORD flags, IShellItem* source, HRESULT hr, IShellItem* created, bool move) noexcept {
        if (flags & TSF_OVERWRITE_EXIST) result_.undo_safe = false;
        if (FAILED(hr) && SUCCEEDED(result_.hr)) result_.hr = hr;
        try {
            // A moved Shell item may resolve to its new location. Retain the
            // pre-operation path by COM identity, including nested work.
            auto from = ItemPath(source);
            ComPtr<IUnknown> identity;
            if (source && SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&identity)))) {
                for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                    if (it->identity.Get() != identity.Get()) continue;
                    from = it->source;
                    pending_.erase(it);
                    break;
                }
            }
            const auto to = ItemPath(created);
            bool completed = hr == S_OK && created;
            // The Shell reports a completed same-volume move as
            // DONT_PROCESS_CHILDREN. Accept that precise status only with the
            // returned destination and independent evidence the source is gone.
            // USER_IGNORED, MERGE, PENDING and other success codes are not done.
            if (move && hr == COPYENGINE_S_DONT_PROCESS_CHILDREN && created &&
                !from.empty() && !to.empty() && !path::EqualInsensitive(from, to)) {
                const DWORD source_attributes = GetFileAttributesW(from.c_str());
                const DWORD source_error = source_attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
                completed = source_attributes == INVALID_FILE_ATTRIBUTES &&
                    (source_error == ERROR_FILE_NOT_FOUND || source_error == ERROR_PATH_NOT_FOUND) &&
                    GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES;
            }
            if (completed) {
                result_.mutated = true;
                if (!to.empty()) for (const auto& original : sources_) {
                    if (!path::EqualInsensitive(path::StripExtendedPathPrefix(original), from)) continue;
                    bool recorded = false;
                    for (const auto& prior : result_.sources)
                        if (path::EqualInsensitive(prior, original)) recorded = true;
                    if (!recorded) {
                        result_.sources.push_back(original);
                        try { result_.destinations.push_back(to); }
                        catch (...) { result_.sources.pop_back(); throw; }
                    }
                    break;
                }
            }
        } catch (...) { result_.hr = E_OUTOFMEMORY; return E_OUTOFMEMORY; }
        return CheckCancel();
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR) override { return Before(item); }
    IFACEMETHODIMP PostCopyItem(DWORD flags, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override { return After(flags, item, hr, created, false); }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR) override { return Before(item); }
    IFACEMETHODIMP PostMoveItem(DWORD flags, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override { return After(flags, item, hr, created, true); }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return CheckCancel(); }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return CheckCancel(); }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return CheckCancel(); }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return CheckCancel(); }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return CheckCancel(); }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return CheckCancel(); }
    IFACEMETHODIMP UpdateProgress(UINT total, UINT done) override {
        try { if (progress_) progress_(current_, total ? static_cast<float>(100.0 * done / total) : -1.0f); }
        catch (...) { return E_FAIL; }
        return CheckCancel();
    }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
    ShellTransferResult& result_;
    std::atomic<bool>& cancel_;
    const std::vector<std::wstring>& sources_;
    ShellTransferProgress progress_;
    std::wstring current_;
    struct PendingItem { ComPtr<IUnknown> identity; std::wstring source; };
    std::vector<PendingItem> pending_;
};
}
DWORD ShellTransferFlags(ShellCollisionPolicy policy) {
    DWORD flags = FOF_SILENT | FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT;
    if (policy == ShellCollisionPolicy::Replace) flags |= FOF_NOCONFIRMATION;
    if (policy == ShellCollisionPolicy::KeepBoth) flags |= FOF_RENAMEONCOLLISION;
    return flags;
}
bool TargetNeedsElevation(const std::wstring& destination, DWORD required_access) {
    auto candidate = std::filesystem::path(destination);
    while (!candidate.empty()) {
        const DWORD error = ProbeAccess(candidate.wstring(), required_access);
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return Denied(error);
        const auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
        // Missing descendants are created below the nearest existing ancestor.
        // That ancestor needs a directory entry, not an ordinary file entry.
        required_access = FILE_ADD_SUBDIRECTORY;
    }
    return false;
}
bool NeedsShellTransfer(const std::vector<std::wstring>& sources,
                        const std::wstring& destination, bool move, const std::wstring& target_name) {
    constexpr DWORD kAnyEntry = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    const SourceListing listing(sources);
    DWORD required_access = 0;
    for (const auto& source : sources) {
        // Past this point no source can ask for more.
        if (required_access == kAnyEntry) break;
        const ListedSource* listed = listing.Find(source);
        const DWORD attributes = listed ? listed->attributes : GetFileAttributesW(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            required_access |= kAnyEntry;
        } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            const auto target = std::filesystem::path(destination) /
                (target_name.empty() ? std::filesystem::path(source).filename() : std::filesystem::path(target_name));
            const DWORD existing = GetFileAttributesW(target.c_str());
            // A real directory merge creates no entry in its parent. As before,
            // this root-only probe does not promise access to descendants.
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                existing == INVALID_FILE_ATTRIBUTES || !(existing & FILE_ATTRIBUTE_DIRECTORY))
                required_access |= FILE_ADD_SUBDIRECTORY;
        } else {
            // Replacements also stage a temporary file in the target directory.
            required_access |= FILE_ADD_FILE;
        }
    }
    if (required_access && TargetNeedsElevation(destination, required_access)) return true;
    if (!move) return false;
    // A folder that lets this user delete its children lets any of them go,
    // whatever each child's own entry says. Asking the folder once spares an
    // open per item, which dominated moves of thousands of files.
    std::unordered_map<std::wstring, bool> folder_allows;
    for (const auto& source : sources) {
        const auto parent = std::filesystem::path(source).parent_path().wstring();
        auto folder = folder_allows.find(parent);
        if (folder == folder_allows.end())
            folder = folder_allows.emplace(parent,
                !parent.empty() && ProbeAccess(parent, FILE_DELETE_CHILD) == ERROR_SUCCESS).first;
        if (folder->second) continue;
        if (Denied(ProbeAccess(source, DELETE))) return true;
    }
    return false;
}
ShellTransferResult TransferWithShell(const std::vector<std::wstring>& sources,
    const std::wstring& destination, bool move, HWND owner, std::atomic<bool>& cancel,
    ShellCollisionPolicy policy, ShellTransferProgress progress) {
    ShellTransferResult result;
    if (cancel.load()) { result.hr = kCancelled; result.cancelled = true; return result; }
    if (sources.empty()) return result;
    result.undo_safe = policy != ShellCollisionPolicy::Replace;
    for (const auto& source : sources) {
        const DWORD attributes = GetFileAttributesW(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) result.undo_safe = false;
        const auto target = std::filesystem::path(destination) / std::filesystem::path(source).filename();
        const DWORD existing = GetFileAttributesW(target.c_str());
        const DWORD error = existing == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
        if (existing != INVALID_FILE_ATTRIBUTES ||
            (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) result.undo_safe = false;
    }
    ComPtr<IFileOperation> operation;
    result.hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
    if (FAILED(result.hr)) return result;
    result.hr = operation->SetOwnerWindow(owner);
    if (FAILED(result.hr)) return result;
    result.hr = operation->SetOperationFlags(ShellTransferFlags(policy));
    if (FAILED(result.hr)) return result;
    ComPtr<IShellItem> folder;
    result.hr = SHCreateItemFromParsingName(path::StripExtendedPathPrefix(destination).c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(result.hr)) return result;
    ComPtr<Sink> sink;
    sink.Attach(new (std::nothrow) Sink(result, cancel, sources, std::move(progress)));
    if (!sink) { result.hr = E_OUTOFMEMORY; return result; }
    DWORD cookie = 0;
    result.hr = operation->Advise(sink.Get(), &cookie);
    if (FAILED(result.hr)) return result;
    // Queue everything before execution. A setup failure executes nothing.
    for (const auto& source : sources) {
        if (cancel.load()) { result.hr = kCancelled; break; }
        ComPtr<IShellItem> item;
        result.hr = SHCreateItemFromParsingName(path::StripExtendedPathPrefix(source).c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(result.hr)) break;
        result.hr = move ? operation->MoveItem(item.Get(), folder.Get(), nullptr, nullptr)
                         : operation->CopyItem(item.Get(), folder.Get(), nullptr, nullptr);
        if (FAILED(result.hr)) break;
    }
    if (SUCCEEDED(result.hr)) {
        const HRESULT performed = operation->PerformOperations();
        if (FAILED(performed) && SUCCEEDED(result.hr)) result.hr = performed;
        BOOL aborted = FALSE;
        const HRESULT queried = operation->GetAnyOperationsAborted(&aborted);
        if (FAILED(queried) && SUCCEEDED(result.hr)) result.hr = queried;
        result.cancelled = aborted != FALSE;
    }
    operation->Unadvise(cookie);
    result.cancelled = result.cancelled || cancel.load() || result.hr == kCancelled || result.hr == E_ABORT;
    if (result.cancelled && SUCCEEDED(result.hr)) result.hr = kCancelled;
    return result;
}
} // namespace pulse::ops
