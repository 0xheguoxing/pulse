#pragma once
#include "../fs/fs_watch.h"
#include <algorithm>
#include <utility>

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

} // namespace pulse::app
