#include "network_recent.h"

#include "network_locations.h"
#include "server_address.h"
#include "../common/json_utils.h"

#include <algorithm>
#include <cwchar>

namespace pulse::app {

bool NetworkRecentList::Touch(const std::wstring& path, uint64_t now) {
    const std::wstring root = UncShareRoot(path);
    if (root.empty()) return false;
    auto it = std::find_if(items_.begin(), items_.end(),
                           [&](const NetworkRecentItem& item) { return SameNetworkLocation(item.path, root); });
    if (it != items_.end()) items_.erase(it);
    items_.insert(items_.begin(), NetworkRecentItem{root, now});
    if (items_.size() > kCap) items_.resize(kCap);
    return true;
}

bool NetworkRecentList::Remove(const std::wstring& path) {
    const size_t before = items_.size();
    std::erase_if(items_, [&](const NetworkRecentItem& item) { return SameNetworkLocation(item.path, path); });
    return items_.size() != before;
}

std::wstring NetworkRecentList::ToJson() const {
    std::wstring out = L"{\"version\":1,\"items\":[";
    for (size_t i = 0; i < items_.size(); ++i) {
        if (i) out += L',';
        out += L"{\"path\":\"";
        json::Escape(items_[i].path, out);
        out += L"\",\"t\":\"" + std::to_wstring(items_[i].last_visit) + L"\"}";
    }
    out += L"]}";
    return out;
}

bool NetworkRecentList::FromJson(const std::wstring& text) {
    const std::wstring array = json::ExtractArray(text, L"items");
    if (array.empty()) return false;
    std::vector<NetworkRecentItem> loaded;
    json::ForEachElement(array, [&](const std::wstring& raw) {
        if (loaded.size() >= kCap) return;
        const std::wstring root = UncShareRoot(json::ExtractString(raw, L"path"));
        if (root.empty()) return;
        const bool duplicate = std::any_of(loaded.begin(), loaded.end(),
            [&](const NetworkRecentItem& item) { return SameNetworkLocation(item.path, root); });
        if (duplicate) return;
        const std::wstring stamp = json::ExtractString(raw, L"t");
        loaded.push_back({root, stamp.empty() ? 0 : wcstoull(stamp.c_str(), nullptr, 10)});
    });
    items_ = std::move(loaded);
    return true;
}

} // namespace pulse::app
