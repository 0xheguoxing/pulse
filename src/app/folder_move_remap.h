// folder_move_remap.h - Multi-window M6e: where a path lives once the folder
// holding it was moved or renamed. Plain string work, no file system access.
#pragma once

#include <optional>
#include <stack>
#include <string>

namespace pulse::app {

class FolderMove {
public:
    // Either folder may end in a separator. A drive or share root never
    // moves, and virtual (pulse:) paths are not folders: such a move is
    // invalid and remaps nothing.
    FolderMove(const std::wstring& from, const std::wstring& to);

    bool Valid() const { return valid_; }

    // The path `path` has after the move: the new folder itself, or the same
    // subfolder under it. nullopt when `path` is neither the old folder nor
    // inside it. Names compare case-insensitively, like NTFS.
    std::optional<std::wstring> Remap(const std::wstring& path) const;

    // Rewrites every history entry inside the old folder, keeping the order.
    // Returns whether an entry changed.
    bool RemapHistory(std::stack<std::wstring>& history) const;

private:
    std::wstring old_root_;
    std::wstring new_root_;
    bool valid_ = false;
};

}  // namespace pulse::app
