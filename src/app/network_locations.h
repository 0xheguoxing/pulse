#pragma once
#include <windows.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

inline std::wstring NetworkLocationKey(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() >= 8 && CompareStringOrdinal(path.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL)
        path = L"\\\\" + path.substr(8);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

inline bool SameNetworkLocation(const std::wstring& a, const std::wstring& b) {
    const auto left = NetworkLocationKey(a), right = NetworkLocationKey(b);
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

struct NetworkLocation {
    std::wstring name, path;
    bool shell_link = false;
};

enum class NetworkDiscoveryState {
    Unknown,
    Enabled,
    Disabled,
};

struct NetworkLocationScan {
    std::vector<NetworkLocation> items;
    // Unknown unless Windows Firewall can conclusively report every active profile.
    NetworkDiscoveryState discovery = NetworkDiscoveryState::Unknown;
    DWORD error = 0;
    bool cancelled = false;
};

// Worker-only: reads local shortcut metadata without resolving or probing targets,
// and checks the firewall discovery group when using the real NetHood root.
// Initializes COM internally; an existing STA or MTA is also supported. An absent
// NetHood is empty; explicit roots skip host firewall state for deterministic tests.
// Errors/cancellation never return partial results.
NetworkLocationScan ReadNetworkLocations(const std::wstring& root = {},
    const std::function<bool()>& cancel = {});
// Returns Disabled only when every active profile is affirmatively disabled.
// Empty/unknown or mixed results stay conservative so an empty device list is
// never mistaken for a disabled discovery setting.
NetworkDiscoveryState SummarizeNetworkDiscoveryProfiles(
    const std::vector<NetworkDiscoveryState>& active_profiles);
std::vector<NetworkLocation> MergeNetworkLocations(const std::vector<NetworkLocation>& pinned,
    const std::vector<NetworkLocation>& system);

} // namespace pulse::app
