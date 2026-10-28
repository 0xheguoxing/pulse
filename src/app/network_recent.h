// network_recent.h - 最近访问 for the network page: servers and shares the user
// actually opened, newest first, capped. Machine runtime data (stored next to
// the other per-machine files, never part of settings export). Pure; the app
// owns loading and saving.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

struct NetworkRecentItem {
    std::wstring path;      // \\host or \\host\share, as first typed (case kept)
    uint64_t last_visit = 0;  // FILETIME ticks (UTC)
};

class NetworkRecentList {
public:
    static constexpr size_t kCap = 20;

    // Records a visit to `path` (any UNC path; reduced to its share root).
    // Returns false when the path is not UNC.
    bool Touch(const std::wstring& path, uint64_t now);
    bool Remove(const std::wstring& path);
    void Clear() { items_.clear(); }
    const std::vector<NetworkRecentItem>& Items() const { return items_; }

    std::wstring ToJson() const;
    // Replaces the list; malformed members are skipped. False when the text
    // is not a recent-list object at all.
    bool FromJson(const std::wstring& text);

private:
    std::vector<NetworkRecentItem> items_;
};

} // namespace pulse::app
