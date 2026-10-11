// folder_move_remap.cpp - Multi-window M6e: see folder_move_remap.h.
#include "folder_move_remap.h"

#include <cwchar>
#include <vector>

namespace pulse::app {
namespace {

std::wstring TrimSeparators(std::wstring path) {
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    return path;
}

// Length of the root a path starts with, separator included: "C:\",
// "\\server\share\", or either behind the extended "\\?\" / "\\?\UNC\"
// prefix Pulse keeps its folder paths in. 0 when it is no file system path.
size_t RootLength(const std::wstring& path) {
    size_t at = 0;
    bool unc = false;
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        at = 8;
        unc = true;
    } else if (path.starts_with(L"\\\\?\\")) {
        at = 4;
    } else if (path.starts_with(L"\\\\")) {
        at = 2;
        unc = true;
    }
    if (!unc) return path.size() >= at + 3 && path[at + 1] == L':' && path[at + 2] == L'\\' ? at + 3 : 0;
    const size_t server_end = path.find(L'\\', at);
    if (server_end == std::wstring::npos || server_end == at) return 0;
    const size_t share_end = path.find(L'\\', server_end + 1);
    if (share_end == std::wstring::npos || share_end == server_end + 1) return 0;
    return share_end + 1;
}

// A folder that can move: something below a drive or share root.
bool IsMovableFolder(const std::wstring& trimmed) {
    const size_t root = RootLength(trimmed);
    return root != 0 && trimmed.size() > root;
}

}  // namespace

FolderMove::FolderMove(const std::wstring& from, const std::wstring& to)
    : old_root_(TrimSeparators(from)), new_root_(TrimSeparators(to)) {
    valid_ = IsMovableFolder(old_root_) && IsMovableFolder(new_root_) && old_root_ != new_root_;
}

std::optional<std::wstring> FolderMove::Remap(const std::wstring& path) const {
    if (!valid_ || path.size() < old_root_.size() ||
        _wcsnicmp(path.c_str(), old_root_.c_str(), old_root_.size()) != 0)
        return std::nullopt;
    // "C:\proj" holds "C:\proj\src" but not its sibling "C:\project".
    if (path.size() > old_root_.size() && path[old_root_.size()] != L'\\') return std::nullopt;
    return new_root_ + path.substr(old_root_.size());
}

bool FolderMove::RemapHistory(std::stack<std::wstring>& history) const {
    if (!valid_ || history.empty()) return false;
    std::vector<std::wstring> entries;
    entries.reserve(history.size());
    while (!history.empty()) {
        entries.push_back(std::move(history.top()));
        history.pop();
    }
    bool changed = false;
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (auto moved = Remap(*it); moved && *moved != *it) {
            *it = std::move(*moved);
            changed = true;
        }
        history.push(std::move(*it));
    }
    return changed;
}

}  // namespace pulse::app
