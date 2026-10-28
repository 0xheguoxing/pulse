#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pulse::index {
// Retain totals for directories only. File metadata stays in the existing MFT
// index; an edit touches just the old and new ancestor chains.
// A build stores totals in two arrays sorted by id (12 bytes per directory
// instead of a hash node and bucket each); directories created afterwards
// live in a small map until the next build.
class FolderSizeIndex {
public:
    struct Item { int32_t parent = -1; uint64_t bytes = 0; bool directory = false, alive = false; };
    void Reset() {
        valid_ = false; live_ = 0;
        std::vector<int32_t>().swap(ids_);
        std::vector<uint64_t>().swap(totals_);
        extra_.clear();
    }
    bool Valid() const { return valid_; }
    uint64_t Builds() const { return builds_; }
    // Background builds fill a private copy that is published under the engine's
    // write lock; both outcomes count as one aggregation attempt.
    void Adopt(FolderSizeIndex&& built) {
        ids_ = std::move(built.ids_); totals_ = std::move(built.totals_); extra_ = std::move(built.extra_);
        live_ = built.live_; valid_ = built.valid_; ++builds_;
    }
    void NoteFailedBuild() { Reset(); ++builds_; }
    std::optional<uint64_t> Get(int32_t id) const {
        const uint64_t* total = valid_ ? Find(id) : nullptr;
        return total ? std::optional(*total) : std::nullopt;
    }
    template<class Read> bool Build(int32_t count, Read read) {
        Reset(); ++builds_;
        std::vector<uint64_t> sums(static_cast<size_t>(count));
        std::vector<uint32_t> children(static_cast<size_t>(count));
        std::vector<int32_t> queue;
        queue.reserve(static_cast<size_t>(count));
        size_t live = 0, directories = 0;
        for (int32_t i = 0; i < count; ++i) {
            const auto n = read(i);
            if (!n.alive) continue;
            ++live; sums[i] = n.directory ? 0 : n.bytes;
            if (n.directory) ++directories;
            if (n.parent >= count || n.parent == i) return false;
            if (n.parent >= 0 && read(n.parent).alive) {
                if (!read(n.parent).directory) return false;
                ++children[n.parent];
            }
        }
        for (int32_t i = 0; i < count; ++i) if (read(i).alive && !children[i]) queue.push_back(i);
        for (size_t at = 0; at < queue.size(); ++at) {
            const auto i = queue[at]; const auto n = read(i);
            if (n.parent >= 0 && read(n.parent).alive) {
                if (UINT64_MAX - sums[n.parent] < sums[i]) { Reset(); return false; }
                sums[n.parent] += sums[i];
                if (--children[n.parent] == 0) queue.push_back(n.parent);
            }
        }
        if (queue.size() != live) return false;
        ids_.reserve(directories);
        totals_.reserve(directories);
        for (int32_t i = 0; i < count; ++i) {
            const auto n = read(i);
            if (!n.alive || !n.directory) continue;
            if (sums[i] == kRemoved) { Reset(); return false; }
            ids_.push_back(i);
            totals_.push_back(sums[i]);
        }
        live_ = ids_.size();
        valid_ = true;
        return true;
    }
    template<class Read> void Replace(int32_t id, Item before, Item after, Read read) {
        if (!valid_) return;
        if (before.alive && after.alive && before.directory != after.directory) { Reset(); return; }
        const uint64_t old_size = before.directory ? Get(id).value_or(0) : before.bytes;
        const uint64_t new_size = after.directory ? (before.alive ? old_size : 0) : after.bytes;
        auto adjust = [&](int32_t parent, uint64_t bytes, bool add) {
            // Malformed chains must never hang the index service.
            for (size_t depth = 0; parent >= 0; ++depth) {
                if (depth > live_) return false;
                const auto n = read(parent);
                if (!n.alive) return true;
                uint64_t* total = Find(parent);
                if (!total) return false;
                // kRemoved marks an erased slot, so a total stays below it.
                if (add ? kRemoved - 1 - *total < bytes : *total < bytes) return false;
                if (add) *total += bytes; else *total -= bytes;
                parent = n.parent;
            }
            return true;
        };
        if ((before.alive && !adjust(before.parent, old_size, false)) ||
            (after.alive && !adjust(after.parent, new_size, true))) { Reset(); return; }
        if (after.alive && after.directory) Set(id, new_size);
        else Erase(id);
    }
private:
    static constexpr uint64_t kRemoved = UINT64_MAX;
    uint64_t* Slot(int32_t id) {
        const auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
        return it != ids_.end() && *it == id ? &totals_[static_cast<size_t>(it - ids_.begin())] : nullptr;
    }
    uint64_t* Find(int32_t id) {
        if (uint64_t* slot = Slot(id)) return *slot == kRemoved ? nullptr : slot;
        const auto it = extra_.find(id);
        return it == extra_.end() ? nullptr : &it->second;
    }
    const uint64_t* Find(int32_t id) const { return const_cast<FolderSizeIndex*>(this)->Find(id); }
    void Set(int32_t id, uint64_t total) {
        if (uint64_t* slot = Slot(id)) {
            if (*slot == kRemoved) ++live_;
            *slot = total;
        } else if (extra_.insert_or_assign(id, total).second) {
            ++live_;
        }
    }
    void Erase(int32_t id) {
        if (uint64_t* slot = Slot(id)) {
            if (*slot != kRemoved) { *slot = kRemoved; --live_; }
        } else if (extra_.erase(id)) {
            --live_;
        }
    }
    bool valid_ = false;
    uint64_t builds_ = 0;
    size_t live_ = 0;
    std::vector<int32_t> ids_;
    std::vector<uint64_t> totals_;
    std::unordered_map<int32_t, uint64_t> extra_;
};
}
