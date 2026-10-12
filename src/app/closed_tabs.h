// closed_tabs.h - Tabs closed by hand, kept so Ctrl+Shift+T brings them back
// (multi-window M7f, plan §8.4). The tabs one action closed (close others,
// close to the right, close a group) come back together, each at its old
// place. Closed windows (closed_windows.h) share the numbering, so
// Ctrl+Shift+T reopens whichever was closed last.
#pragma once
#include "closed_windows.h"
#include "session.h"

#include <windows.h>

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace pulse::app {

struct ClosedTab {
    LayoutTabSnapshot tab;
    size_t index = 0;            // its place before the action
    HWND window = nullptr;       // closed there; it goes back there while that window is open
    GroupSessionSnapshot group;  // the group it was in (id 0: none)
    uint64_t seq = 0;            // the action (AppCore::close_seq); one action, one number
};

// Most recently closed last. Holds at most kCapacity tabs; the oldest go.
class ClosedTabStack {
public:
    static constexpr size_t kCapacity = 25;

    // The tabs of one action, all with the same seq, in their old order.
    void Push(std::vector<ClosedTab> batch) {
        for (ClosedTab& tab : batch) items_.push_back(std::move(tab));
        while (items_.size() > kCapacity) items_.pop_front();
    }
    // The tabs of the latest action, in the order they were pushed.
    std::vector<ClosedTab> TakeLatestBatch() {
        std::vector<ClosedTab> batch;
        if (items_.empty()) return batch;
        const uint64_t seq = items_.back().seq;
        size_t first = items_.size();
        while (first > 0 && items_[first - 1].seq == seq) --first;
        for (size_t i = first; i < items_.size(); ++i) batch.push_back(std::move(items_[i]));
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(first), items_.end());
        return batch;
    }
    uint64_t LatestSeq() const noexcept { return items_.empty() ? 0 : items_.back().seq; }
    size_t Size() const noexcept { return items_.size(); }
    bool Empty() const noexcept { return items_.empty(); }

private:
    std::deque<ClosedTab> items_;
};

enum class ReopenKind { Nothing, Tabs, Window };

// Ctrl+Shift+T: whichever was closed last; nothing when nothing was.
inline ReopenKind NextToReopen(const ClosedTabStack& tabs, const ClosedWindowStack& windows) {
    if (tabs.Empty()) return windows.Empty() ? ReopenKind::Nothing : ReopenKind::Window;
    if (windows.Empty()) return ReopenKind::Tabs;
    return tabs.LatestSeq() > windows.LatestSeq() ? ReopenKind::Tabs : ReopenKind::Window;
}

}  // namespace pulse::app
