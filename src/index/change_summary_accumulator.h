#pragma once
#include "change_tracking.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace pulse::index {
// Scratch state for one file identity. Most files touch only a few ancestors;
// long move histories switch to hashing instead of quadratic vector searches.
class ChangeSummaryAccumulator {
public:
    void Reset() { items_.clear(); lookup_.clear(); }
    void Add(ChangeSummary& summary, ChangeKind kind, uint64_t time, int priority) {
        size_t position = items_.size();
        if (items_.size() < kLinearLimit) {
            auto found = std::find_if(items_.begin(), items_.end(),
                [&](const Item& item) { return item.summary == &summary; });
            position = static_cast<size_t>(found - items_.begin());
        } else {
            if (lookup_.empty()) {
                lookup_.reserve(items_.size() * 2);
                for (size_t i = 0; i < items_.size(); ++i) lookup_.emplace(items_[i].summary, i);
            }
            position = lookup_.try_emplace(&summary, items_.size()).first->second;
        }
        if (position == items_.size()) items_.push_back(Item{&summary});
        auto& item = items_[position];
        item.time = (std::max)(item.time, time);
        if (priority >= item.priority || (item.kind == ChangeKind::Deleted && kind == ChangeKind::Created)) {
            item.kind = kind; item.priority = priority;
        }
    }
    void Commit() const {
        for (const auto& item : items_) {
            auto& summary = *item.summary;
            ++summary.count;
            summary.last_change = (std::max)(summary.last_change, item.time);
            ++summary.counts[static_cast<uint32_t>(item.kind)];
            summary.has_deleted |= item.kind == ChangeKind::Deleted;
        }
    }
    // Takes back what Commit() added for the same events, before the identity
    // is committed again with newer ones. Its ancestors and times only grow,
    // so last_change stays a valid maximum; has_deleted follows its count.
    void Retract() const {
        for (const auto& item : items_) {
            auto& summary = *item.summary;
            --summary.count;
            --summary.counts[static_cast<uint32_t>(item.kind)];
            summary.has_deleted = summary.counts[static_cast<uint32_t>(ChangeKind::Deleted)] != 0;
        }
    }
private:
    struct Item {
        ChangeSummary* summary;
        uint64_t time = 0;
        ChangeKind kind = ChangeKind::Modified;
        int priority = 1;
    };
    static constexpr size_t kLinearLimit = 32;
    std::vector<Item> items_;
    std::unordered_map<ChangeSummary*, size_t> lookup_;
};
}
