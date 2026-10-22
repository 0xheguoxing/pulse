#pragma once
#include "../fs/fs_enum.h"
#include "../ui/ui_view_model.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::app {

// How folders are ordered against files in the details list.
enum class FolderSortMode : uint8_t {
    FoldersFirst = 0,  // folders stay on top, reversed order included (File Explorer)
    FollowDirection,   // folders lead ascending order and trail descending order
    Mixed,             // one order for folders and files
};

constexpr FolderSortMode FolderSortModeFromInt(int value) noexcept {
    return value == 1 ? FolderSortMode::FollowDirection
         : value == 2 ? FolderSortMode::Mixed
                      : FolderSortMode::FoldersFirst;
}

// The worker sorts on its own thread; keep the current pref in one atomic slot
// instead of threading it through every WorkItem and snapshot patch.
void SetFolderSortMode(FolderSortMode mode) noexcept;
FolderSortMode CurrentFolderSortMode() noexcept;

bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir);
// Explicit mode for tests and callers that must not read the global pref.
bool EntryLess(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir, FolderSortMode folders);

// Sorts `entries` in EntryLess order (grouping and folder mode included).
// Faster than std::sort with EntryLess on large lists; `tick` runs once per
// comparison and may throw to abandon the sort, which leaves `entries` untouched.
void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir,
                 const std::function<void()>& tick = {});

// Folder totals for Size order (#58), keyed by lower-cased folder name.
using FolderSizeLookup = std::unordered_map<std::wstring, uint64_t>;
// Size order as EntryLess sorts it (grouping and folder mode included), except
// that folders compare by their totals in `sizes`; folders without one follow
// the rest in either direction. `tick` runs once per comparison and may throw
// to abandon the sort, which leaves `entries` untouched.
void SortEntriesBySize(std::vector<fs::DirEntry>& entries, ui::SortDirection dir,
                       const FolderSizeLookup& sizes, const std::function<void()>& tick = {});
// Order-independent fingerprint of `sizes`, to notice changed totals. Empty is 0.
uint64_t FolderSizeSignature(const FolderSizeLookup& sizes);

} // namespace pulse::app