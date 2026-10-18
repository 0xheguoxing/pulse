#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulse::index {

// Dense "may be present" bits in front of the sparse overlay containers.
// Base-node scans look every node up in the patch and tombstone sets; with a
// few hundred thousand pending USN changes each miss cost a hash probe that
// usually missed the cache, slowing searches 10-1000x until the next merge.
// A bit is set on every insertion and cleared on erase, so lookups stay exact.
class OverlayBits {
public:
    bool Test(int32_t id) const noexcept {
        if (id < 0) return negative_;
        const size_t word = static_cast<uint32_t>(id) >> 6;
        return word < words_.size() && ((words_[word] >> (static_cast<uint32_t>(id) & 63u)) & 1u) != 0;
    }
    void Set(int32_t id) {
        if (id < 0) {
            negative_ = true;
            return;
        }
        const size_t word = static_cast<uint32_t>(id) >> 6;
        if (word >= words_.size()) words_.resize(word + 1, 0);
        words_[word] |= uint64_t{1} << (static_cast<uint32_t>(id) & 63u);
    }
    void Reset(int32_t id) noexcept {
        if (id < 0) return; // negative keys share one conservative bit
        const size_t word = static_cast<uint32_t>(id) >> 6;
        if (word < words_.size()) words_[word] &= ~(uint64_t{1} << (static_cast<uint32_t>(id) & 63u));
    }
    void Clear() noexcept {
        words_.clear();
        negative_ = false;
    }
private:
    std::vector<uint64_t> words_;
    bool negative_ = false;
};

template <class Value>
class OverlayMap {
public:
    using Map = std::unordered_map<int32_t, Value>;
    using iterator = typename Map::iterator;
    using const_iterator = typename Map::const_iterator;

    Value& operator[](int32_t id) {
        bits_.Set(id);
        return map_[id];
    }
    // An empty overlay (the state right after every merge) costs one size check.
    iterator find(int32_t id) { return !map_.empty() && bits_.Test(id) ? map_.find(id) : map_.end(); }
    const_iterator find(int32_t id) const { return !map_.empty() && bits_.Test(id) ? map_.find(id) : map_.end(); }
    bool contains(int32_t id) const { return !map_.empty() && bits_.Test(id) && map_.contains(id); }
    size_t count(int32_t id) const { return contains(id) ? 1u : 0u; }
    size_t erase(int32_t id) {
        bits_.Reset(id);
        return map_.erase(id);
    }
    void clear() noexcept {
        map_.clear();
        bits_.Clear();
    }
    bool empty() const noexcept { return map_.empty(); }
    size_t size() const noexcept { return map_.size(); }
    iterator begin() noexcept { return map_.begin(); }
    iterator end() noexcept { return map_.end(); }
    const_iterator begin() const noexcept { return map_.begin(); }
    const_iterator end() const noexcept { return map_.end(); }
private:
    Map map_;
    OverlayBits bits_;
};

class OverlaySet {
public:
    using Set = std::unordered_set<int32_t>;
    using iterator = Set::iterator;
    using const_iterator = Set::const_iterator;

    std::pair<iterator, bool> insert(int32_t id) {
        bits_.Set(id);
        return set_.insert(id);
    }
    bool contains(int32_t id) const { return !set_.empty() && bits_.Test(id) && set_.contains(id); }
    size_t count(int32_t id) const { return contains(id) ? 1u : 0u; }
    size_t erase(int32_t id) {
        bits_.Reset(id);
        return set_.erase(id);
    }
    void clear() noexcept {
        set_.clear();
        bits_.Clear();
    }
    bool empty() const noexcept { return set_.empty(); }
    size_t size() const noexcept { return set_.size(); }
    const_iterator begin() const noexcept { return set_.begin(); }
    const_iterator end() const noexcept { return set_.end(); }
private:
    Set set_;
    OverlayBits bits_;
};

} // namespace pulse::index
