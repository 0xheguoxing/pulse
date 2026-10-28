// network_drives.h - Mapped network drives (网络驱动器) for the network page,
// including remembered ones that are not connected right now. Worker-only:
// the multiple provider router may take a moment even for local state.
#pragma once
#include <string>
#include <vector>

namespace pulse::app {

struct MappedNetworkDrive {
    wchar_t letter = 0;      // L'Z'
    std::wstring remote;     // \\server\share
    bool connected = false;  // false: remembered, reconnects on first use
};

// Sorted by drive letter. Empty when the system reports none or on error.
std::vector<MappedNetworkDrive> ReadMappedNetworkDrives();

} // namespace pulse::app
