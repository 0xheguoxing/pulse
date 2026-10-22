#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../ui/ui_view_model.h"
#include <vector>

namespace pulse::app {

enum class NotifyPatch { Applied, NeedFullEnum };

bool FillDirEntry(const std::wstring& dir, const std::wstring& name, fs::DirEntry& out);

// Patches keep the rows on screen in place like File Explorer (#13): a
// renamed or modified row stays put, a new row goes to the end of its group
// (see entry_order_hold.h). The list is re-sorted by a full enumeration.
NotifyPatch ApplyDirNotify(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                           const fs::DirNotifyEvent& event,
                           ui::SortColumn col, ui::SortDirection sort_dir);

// Same result as calling ApplyDirNotify for each event in order, in
// O(entries + events log events) instead of O(entries * events).
NotifyPatch ApplyDirNotifyBatch(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                                const std::vector<fs::DirNotifyEvent>& events,
                                ui::SortColumn col, ui::SortDirection sort_dir);

} // namespace pulse::app
