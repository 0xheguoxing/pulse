#include "ops_manager.h"
#include "elevated_delete.h"
#include "elevated_transfer_client.h"
#include "transfer_rate_estimator.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <sherrors.h>
#include <atomic>
#include <vector>

namespace pulse::ops {
namespace {
bool PermissionFailure(HRESULT hr) {
    return hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD) ||
        hr == HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED) ||
        hr == static_cast<HRESULT>(0x80270021L) || hr == static_cast<HRESULT>(0x80270022L);
}
// Localized text for the failures users actually hit; the elevated helper
// has no localization and FormatMessage knows nothing about COPYENGINE_E_*.
std::wstring KnownDeleteError(HRESULT hr) {
    switch (static_cast<uint32_t>(hr)) {
    case 0x80070005u:  // E_ACCESSDENIED
    case 0x80270021u:  // COPYENGINE_E_ACCESS_DENIED_SRC
    case 0x80270022u:  // COPYENGINE_E_ACCESS_DENIED_DEST
        return l10n::Get(l10n::StringId::AccessDeniedTheItemMayBeProtectedItsPermissionsMayNotAllowDe).c_str();
    case 0x80070020u:  // ERROR_SHARING_VIOLATION
    case 0x80070021u:  // ERROR_LOCK_VIOLATION
    case 0x80270027u:  // COPYENGINE_E_SHARING_VIOLATION_SRC
    case 0x80270028u:  // COPYENGINE_E_SHARING_VIOLATION_DEST
        return l10n::Get(l10n::StringId::TheItemIsInUseByAnotherProgramCloseItAndTryAgain).c_str();
    case static_cast<uint32_t>(COPYENGINE_E_RECYCLE_BIN_NOT_FOUND):
        return l10n::Get(l10n::StringId::ThisLocationCannotBeSafelySentToTheRecycleBinTheItemWasNotDe).c_str();
    default:
        return {};
    }
}
std::wstring DeleteError(const ShellTransferResult& result) {
    if (auto known = KnownDeleteError(result.hr); !known.empty()) return known;
    if (!result.error.empty()) return result.error;
    if (result.hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return l10n::Get(l10n::StringId::AdministratorPermissionWasNotGrantedRetryOrSkipThisItem).c_str();
    wchar_t* text = nullptr;
    std::wstring error;
    if (FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(result.hr), 0,
            reinterpret_cast<LPWSTR>(&text), 0, nullptr) && text) error = text;
    if (text) LocalFree(text);
    if (!error.empty()) return error;
    wchar_t code[16];
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(result.hr));
    return std::wstring(l10n::Get(l10n::StringId::CouldNotDeleteThisItemError).c_str()) + code +
           l10n::Get(l10n::StringId::Text3072).c_str();
}

// Permanent delete without the Shell. IFileOperation spends ~20 ms per item
// on setup; a handle with POSIX delete semantics removes the name at once,
// so a folder is empty, and deletable, as soon as its children are gone.
// FILE_DISPOSITION_INFO_EX is spelled out because the Windows 8.1 build
// targets headers older than it.
constexpr auto kFileDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr DWORD kDispositionDelete = 0x1;
constexpr DWORD kDispositionPosixSemantics = 0x2;
constexpr DWORD kDispositionIgnoreReadonly = 0x10;
std::atomic<bool> g_disposition_ex_unsupported{false};

std::wstring NativePath(const std::wstring& path) {
    if (path.size() < 240 || path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring JoinChild(const std::wstring& dir, const wchar_t* name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

// ERROR_SUCCESS, or why the item is still there.
DWORD DeleteNativeItem(const std::wstring& path, DWORD attributes) {
    const std::wstring api_path = NativePath(path);
    if (!g_disposition_ex_unsupported.load(std::memory_order_relaxed)) {
        HANDLE handle = CreateFileW(api_path.c_str(), DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return GetLastError();
        struct { DWORD flags; } info{kDispositionDelete | kDispositionPosixSemantics | kDispositionIgnoreReadonly};
        const BOOL deleted = SetFileInformationByHandle(handle, kFileDispositionInfoEx, &info, sizeof(info));
        const DWORD code = deleted ? ERROR_SUCCESS : GetLastError();
        CloseHandle(handle);
        if (deleted) return ERROR_SUCCESS;
        // Before Windows 10 1809, or a file system without it (FAT, some
        // shares): use the classic calls from now on.
        if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED &&
            code != ERROR_INVALID_FUNCTION)
            return code;
        g_disposition_ex_unsupported.store(true, std::memory_order_relaxed);
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(api_path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    const BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(api_path.c_str())
                                                                 : DeleteFileW(api_path.c_str());
    if (removed) return ERROR_SUCCESS;
    const DWORD code = GetLastError();
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(api_path.c_str(), attributes);
    return code;
}

struct NativeDeleteOutcome {
    bool done = false;       // the source no longer exists
    bool mutated = false;    // something under it was deleted
    bool cancelled = false;
};

// Deletes one source, links as links (never entered). Stops at the first
// item it cannot delete and leaves the rest, with its error UI, to the Shell
// path. Lists the tree first so progress has a total.
template <typename Progress>
NativeDeleteOutcome DeleteNative(const std::wstring& source, std::atomic<bool>& cancel, Progress&& progress) {
    NativeDeleteOutcome outcome;
    // Never a volume root; anything that is not a plain path goes to the Shell.
    if (source.size() <= 3 || source.starts_with(L"::") || source.find(L'/') != std::wstring::npos)
        return outcome;
    const DWORD root_attributes = GetFileAttributesW(NativePath(source).c_str());
    if (root_attributes == INVALID_FILE_ATTRIBUTES) return outcome;
    struct Item { std::wstring path; DWORD attributes; };
    std::vector<Item> items;  // pre-order: every folder before its contents
    items.push_back({source, root_attributes});
    if ((root_attributes & FILE_ATTRIBUTE_DIRECTORY) && !(root_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::vector<size_t> pending{0};
        while (!pending.empty()) {
            if (cancel.load()) { outcome.cancelled = true; return outcome; }
            const std::wstring folder = items[pending.back()].path;
            pending.pop_back();
            WIN32_FIND_DATAW data{};
            const std::wstring pattern = JoinChild(NativePath(folder), L"*");
            HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
                return outcome;  // unreadable: the Shell path reports it
            }
            do {
                const wchar_t* name = data.cFileName;
                if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
                items.push_back({JoinChild(folder, name), data.dwFileAttributes});
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    pending.push_back(items.size() - 1);
            } while (FindNextFileW(find, &data));
            const DWORD code = GetLastError();
            FindClose(find);
            if (code != ERROR_NO_MORE_FILES) return outcome;
        }
    }
    // Reverse pre-order: contents before their folder.
    const size_t total = items.size();
    ULONGLONG last_report = 0;
    for (size_t i = total; i-- > 0;) {
        if (cancel.load()) { outcome.cancelled = true; return outcome; }
        const DWORD code = DeleteNativeItem(items[i].path, items[i].attributes);
        if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) return outcome;
        outcome.mutated = true;
        const ULONGLONG now = GetTickCount64();
        if (total > 1 && now - last_report >= 50) {
            last_report = now;
            progress(items[i].path, static_cast<float>((total - i) * 100.0 / total));
        }
    }
    outcome.done = true;
    return outcome;
}
}

void OpsManager::RunAuthorizedDelete(const OpRequest& req, uint64_t task_id) {
    transfer_cancel_.store(shell_cancel_requested_.load());
    transfer_pause_.store(false);
    transfer_active_.store(true);
    SetStatus([](OpStatus& status) { status.can_pause = false; });
    const bool permanent = req.type == OpType::RealDelete;
    std::vector<std::wstring> completed;
    size_t skipped = 0;
    HRESULT failure = S_OK;
    std::wstring error;
    bool defer_authorization = false;
    std::wstring authorization_error;
    const auto started = GetTickCount64();
    TransferRateEstimator rate;
    rate.SetEtaFloor(0.0);
    rate.Reset(started, 0);
    const auto update_rate = [&](OpStatus& status) {
        // Only confirmed source deletions count, including on the elevated path.
        // Skipped sources reduce remaining work without inflating the delete rate.
        rate.Observe(GetTickCount64(), completed.size(), req.sources.size() - skipped);
        status.items_per_second = rate.speed();
        status.peak_items_per_second = (std::max)(status.peak_items_per_second,
                                                status.items_per_second);
        status.eta_seconds = rate.eta_seconds();
    };
    // Set while the authorization view is up; leaving it is shown at once,
    // ordinary per-item progress is throttled.
    bool authorization_shown = false;
    RecycleCache recycle_cache;
    for (const auto& source : req.sources) {
        if (transfer_cancel_.load()) break;
        SetProgressStatus([&](OpStatus& status) {
            status.current_item = source;
            status.last_error.clear();
            status.phase = OpPhase::Running;
        });
        // After "Retry", stay on the authorization view until the attempt
        // actually deletes something or ends; the helper's "ready" signal and
        // its initial 0% progress otherwise flash the progress/estimate view
        // just before the same error brings the authorization view back.
        bool retrying = false;
        ElevatedTransferCallbacks callbacks;
        callbacks.authorization = [&](bool awaiting) {
            if (retrying && !awaiting) return;
            authorization_shown = awaiting;
            SetStatus([&](OpStatus& status) {
                status.authorization = awaiting ? AuthorizationState::Requesting : AuthorizationState::None;
                status.authorization_retrying = false;
                status.summary = awaiting
                    ? l10n::Get(l10n::StringId::RequestingAdministratorPermission).c_str()
                    : l10n::Get(l10n::StringId::Deleting).c_str();
            });
        };
        auto progress = [&](const std::wstring& item, float percent) {
            if (retrying) {
                if (percent <= 0) return;
                retrying = false;
            }
            const auto update = [&](OpStatus& status) {
                status.authorization = AuthorizationState::None;
                status.authorization_retrying = false;
                status.current_item = item.empty() ? source : item;
                status.completed_items = completed.size();
                status.percent = req.sources.empty() ? 0.0f :
                    static_cast<float>((completed.size() + skipped + (percent < 0 ? 0 : percent / 100.0)) *
                                       100.0 / req.sources.size());
                update_rate(status);
            };
            if (authorization_shown) { authorization_shown = false; SetStatus(update); }
            else SetProgressStatus(update);
        };
        callbacks.progress = [&](const OpStatus& status) { progress(status.current_item, status.percent); };
        ShellTransferResult result;
        bool native_mutated = false;
        if (permanent) {
            const auto native = DeleteNative(source, transfer_cancel_, progress);
            native_mutated = native.mutated;
            if (native.done || native.cancelled) {
                result.mutated = native.mutated;
                if (native.done) result.sources = {source};
                if (native.cancelled) {
                    result.cancelled = true;
                    result.hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                }
            }
        }
        // What the native pass could not delete takes the Shell path, which
        // brings the elevation and in-use prompts.
        const bool handled = !result.sources.empty() || result.cancelled;
        bool authorized = !handled && NeedsShellTransfer({source}, L"", true);
        for (; !handled;) {
            if (authorized && defer_authorization) {
                const auto choice = WaitForAuthorization(task_id, authorization_error, source);
                if (choice == AuthorizationChoice::Cancel) break;
                if (choice == AuthorizationChoice::Skip) { ++skipped; result = ShellTransferResult{}; break; }
                defer_authorization = false;
            }
            result = authorized
                ? DeleteWithElevatedHelper({source}, permanent, UiWindow(), transfer_cancel_, callbacks)
                : DeleteWithCurrentToken({source}, permanent, UiWindow(), transfer_cancel_, progress,
                                         permanent ? nullptr : &recycle_cache);
            if (!authorized && !result.mutated && PermissionFailure(result.hr) && !transfer_cancel_.load()) {
                authorized = true;
                continue;
            }
            if (SUCCEEDED(result.hr) || result.mutated || transfer_cancel_.load() || !authorized) break;
            authorization_error = DeleteError(result);
            const auto choice = WaitForAuthorization(task_id, authorization_error, source);
            if (choice == AuthorizationChoice::Retry) { retrying = true; continue; }
            if (choice == AuthorizationChoice::Skip) {
                ++skipped;
                defer_authorization = true;
                result = ShellTransferResult{};
            }
            break;
        }
        if (native_mutated) result.mutated = true;
        completed.insert(completed.end(), result.sources.begin(), result.sources.end());
        const auto finish_item = [&](OpStatus& status) {
            // A retried attempt on a reused elevated session never invokes the
            // authorization callback, so leave the Requesting view explicitly.
            status.authorization = AuthorizationState::None;
            status.authorization_retrying = false;
            status.completed_items = completed.size();
            update_rate(status);
        };
        if (handled && !authorization_shown) SetProgressStatus(finish_item);
        else { authorization_shown = false; SetStatus(finish_item); }
        // Also when a partly deleted item was then skipped.
        if (result.mutated && (FAILED(result.hr) || result.cancelled || result.sources.empty())) {
        CompletedOperation refresh;
        refresh.task_id = task_id;
            refresh.type = req.type;
            refresh.sources = {source};
            refresh.refresh_only = true;
            std::lock_guard lock(mutex_);
            completions_.push_back(std::move(refresh));
        }
        if (result.cancelled && !PermissionFailure(result.hr)) transfer_cancel_.store(true);
        if (FAILED(result.hr)) { failure = result.hr; error = DeleteError(result); break; }
    }
    if (!completed.empty()) {
        OpRequest finished = req;
        finished.sources = completed;
        PushUndo(finished, task_id);
    }
    const bool cancelled = transfer_cancel_.load();
    const auto lock_report = FAILED(failure) && !cancelled ? ProbeLock(req, task_id, failure, error) : LockReport{};
    diagnostics::runtime::Event("file_delete_result", {{"task", task_id}, {"permanent", permanent},
        {"hresult", static_cast<uint32_t>(failure)}, {"cancelled", cancelled},
        {"completed", completed.size()}, {"skipped", skipped}, {"elapsed_ms", GetTickCount64() - started}});
    SetStatus([&](OpStatus& status) {
        status.active = false;
        status.authorization = AuthorizationState::None;
        status.can_skip_authorization = false;
        status.can_pause = false;
        ++status.completed_ops;
        status.completed_items = completed.size();
        status.bytes_per_second = 0;
        status.items_per_second = 0;
        status.eta_seconds = 0;
        status.locked_path = lock_report.path;
        status.lock_owners = lock_report.owners;
        if (cancelled || FAILED(failure)) {
            status.phase = OpPhase::Failed;
            status.last_error = cancelled ? l10n::Get(l10n::StringId::Canceled).c_str() : error;
            status.summary = status.last_error;
        } else {
            status.phase = OpPhase::Completed;
            status.percent = 100;
            status.last_error.clear();
            status.summary = skipped ? l10n::Get(l10n::StringId::DeletionFinishedSkipped).c_str() + std::to_wstring(skipped)
                                     : l10n::Get(l10n::StringId::DeletionCompleted).c_str();
        }
    });
    transfer_active_.store(false);
}
}
