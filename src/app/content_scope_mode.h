// content_scope_mode.h - which folders an unscoped content search reads.
//
// Content search reads files live (Instant mode), so its cost grows with the
// folders it walks. By default an unscoped search (no path: prefix, not "current
// folder only") covers the user's Desktop, Documents and Downloads; the user can
// switch to all drives in Settings or in the first-use prompt. Folder-scoped
// searches are unchanged: they read whatever folder they name.
//
// The mode only narrows the shared all-drives configuration (shared_scope). A
// hand-picked root list already is the user's scope and is left alone.
//
// Header-only on purpose: the main window and the global search window (its
// own content client) both read the mode, set once per tick by the app.
#pragma once

#include "../common/windows_compat.h"
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace pulse::app {

// AppPrefs::content_scope values. Unset searches like UserFolders and asks
// once when the user first switches the address bar to content search.
enum class ContentScopeMode : int { Unset = 0, UserFolders = 1, AllDrives = 2 };

inline std::atomic<int>& ContentScopeModeSlot() noexcept {
    static std::atomic<int> mode{0};
    return mode;
}
inline void SetContentScopeMode(int mode) noexcept {
    ContentScopeModeSlot().store(mode, std::memory_order_relaxed);
}
inline ContentScopeMode CurrentContentScopeMode() noexcept {
    const int mode = ContentScopeModeSlot().load(std::memory_order_relaxed);
    return mode == 1 ? ContentScopeMode::UserFolders
         : mode == 2 ? ContentScopeMode::AllDrives : ContentScopeMode::Unset;
}

namespace content_scope_detail {
inline std::wstring Key(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return path;
}
inline bool Under(const std::wstring& path, const std::wstring& root) {
    return path == root || (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
        (root.back() == L'\\' || path[root.size()] == L'\\'));
}
inline bool IsDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}
} // namespace content_scope_detail

// Keeps existing folders, drops duplicates (case-insensitive, trailing slashes)
// and folders inside another kept folder, e.g. Documents redirected under
// Desktop. Order follows the input. `exists` is injectable for tests.
template <class Exists>
std::vector<std::wstring> NormalizeContentScopeFolders(const std::vector<std::wstring>& folders, Exists exists) {
    using namespace content_scope_detail;
    std::vector<std::wstring> kept, keys;
    for (const auto& folder : folders) {
        if (folder.empty() || !exists(folder)) continue;
        std::wstring display = folder;
        while (display.size() > 3 && (display.back() == L'\\' || display.back() == L'/')) display.pop_back();
        const auto key = Key(display);
        if (std::any_of(keys.begin(), keys.end(), [&](const auto& old) { return Under(key, old); })) continue;
        for (size_t i = keys.size(); i-- > 0;) {
            if (Under(keys[i], key)) { keys.erase(keys.begin() + static_cast<ptrdiff_t>(i)); kept.erase(kept.begin() + static_cast<ptrdiff_t>(i)); }
        }
        keys.push_back(key);
        kept.push_back(std::move(display));
    }
    return kept;
}

// Desktop, Documents and Downloads of the signed-in user, as they exist now
// (redirected to OneDrive or another drive counts).
inline std::vector<std::wstring> UserContentFolders() {
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads};
    std::vector<std::wstring> folders;
    for (const auto* id : ids) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr, &path)) && path) folders.emplace_back(path);
        if (path) CoTaskMemFree(path);
    }
    return NormalizeContentScopeFolders(folders, content_scope_detail::IsDirectory);
}

// request.roots for an unscoped content search: the user folders when the
// configuration is the shared all-drives scope and the mode is not AllDrives;
// empty (search the whole configuration) otherwise.
inline std::vector<std::wstring> DefaultContentSearchRoots(bool shared_scope, ContentScopeMode mode) {
    if (!shared_scope || mode == ContentScopeMode::AllDrives) return {};
    return UserContentFolders();
}
inline std::vector<std::wstring> DefaultContentSearchRoots(bool shared_scope) {
    return DefaultContentSearchRoots(shared_scope, CurrentContentScopeMode());
}

// Old releases (before Instant mode) kept a full-text database of every drive
// in content-v1.sqlite; nothing reads it now. Deletes the database, then its
// -wal/-shm/-journal companions, and keeps .config and .status, which Instant
// mode still uses. If the database itself cannot be deleted (open elsewhere)
// the companions stay too and found && !deleted tells the caller to retry later.
struct LegacyContentCleanup {
    bool found = false;
    bool deleted = false;
    uint64_t bytes = 0;
};
inline LegacyContentCleanup DeleteLegacyContentDatabase(const std::wstring& database) {
    LegacyContentCleanup result;
    if (database.empty()) return result;
    auto size_of = [](const std::wstring& path, uint64_t& bytes) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        bytes = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        return true;
    };
    uint64_t bytes = 0;
    if (!size_of(database, bytes)) return result;
    result.found = true;
    if (!DeleteFileW(database.c_str())) return result;
    result.deleted = true;
    result.bytes = bytes;
    for (const wchar_t* suffix : {L"-wal", L"-shm", L"-journal"}) {
        const std::wstring companion = database + suffix;
        uint64_t size = 0;
        if (size_of(companion, size) && DeleteFileW(companion.c_str())) result.bytes += size;
    }
    return result;
}

} // namespace pulse::app
