#pragma once
#include "../fs/fs_watch.h"
#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace pulse::app {

// Caller holds the notification queue mutex. A pending full reconciliation
// supersedes per-entry patches for the same path, but not other paths. A
// newly armed watch's overflow (fs::ArmedAt) may ask for nothing, so the
// changes after it still queue.
template<class Batch>
void QueueDirNotify(std::vector<Batch>& queue, const std::wstring& path,
                    bool overflow, std::vector<fs::DirNotifyEvent> events) {
    if (std::any_of(queue.begin(), queue.end(), [&](const auto& batch) {
        return batch.path == path && batch.overflow && fs::ArmedAt(batch.overflow, batch.events) == 0;
    })) return;
    if (overflow) {
        std::erase_if(queue, [&](const auto& batch) { return batch.path == path; });
        if (fs::ArmedAt(overflow, events) == 0) events.clear();
    }
    queue.push_back({path, overflow, std::move(events)});
}

// Paces the patches of a folder that keeps changing. A bulk delete or copy
// reports one item at a time, and each patch copies the whole listing and
// redraws on the UI thread: O(n) per item, a second or more of UI time per
// thousand items in a large folder. A folder patched less than kPaceMs ago
// keeps its new batches until then, so a burst lands as one patch per
// kPaceMs; the first change after a calm spell still lands at once. Batches
// of one folder keep their order. UI thread only.
template<class Batch>
class DirNotifyPacer {
public:
    static constexpr ULONGLONG kPaceMs = 200;

    // Appends the held batches that are due, then `incoming`, to `due`, in
    // order, except those of a folder still inside its pace window, which are
    // held. A folder whose released batches change its listing (anything but
    // a size or time change) starts a new window. `incoming` is left empty.
    void Take(std::vector<Batch>& incoming, ULONGLONG now, std::vector<Batch>& due) {
        std::erase_if(patched_, [&](const auto& item) { return now - item.second >= kPaceMs; });
        std::vector<Batch> pending = std::move(held_);
        held_.clear();
        pending.insert(pending.end(), std::make_move_iterator(incoming.begin()),
                       std::make_move_iterator(incoming.end()));
        incoming.clear();
        std::vector<std::wstring> released;
        for (auto& batch : pending) {
            if (Pacing(batch.path)) {
                held_.push_back(std::move(batch));
                continue;
            }
            if ((batch.overflow || std::any_of(batch.events.begin(), batch.events.end(), [](const auto& event) {
                    return event.action != FILE_ACTION_MODIFIED;
                })) && std::find(released.begin(), released.end(), batch.path) == released.end())
                released.push_back(batch.path);
            due.push_back(std::move(batch));
        }
        for (auto& path : released) patched_.emplace_back(std::move(path), now);
    }
    bool Holding() const { return !held_.empty(); }

private:
    bool Pacing(const std::wstring& path) const {
        return std::any_of(patched_.begin(), patched_.end(), [&](const auto& item) { return item.first == path; });
    }
    std::vector<std::pair<std::wstring, ULONGLONG>> patched_;  // folder, when its window began
    std::vector<Batch> held_;
};

} // namespace pulse::app
