#include "network_drives.h"

#include <windows.h>
#include <winnetwk.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace pulse::app {
namespace {

// Calls fn(local, remote) for every disk resource in `scope`.
template <class Fn>
void EnumerateDisks(DWORD scope, Fn&& fn) {
    HANDLE handle = nullptr;
    if (WNetOpenEnumW(scope, RESOURCETYPE_DISK, 0, nullptr, &handle) != NO_ERROR) return;
    std::vector<BYTE> buffer(16 * 1024);
    for (;;) {
        DWORD count = static_cast<DWORD>(-1);
        DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD result = WNetEnumResourceW(handle, &count, buffer.data(), &size);
        if (result == ERROR_MORE_DATA && size > buffer.size()) {
            buffer.resize(size);
            continue;
        }
        if (result != NO_ERROR) break;
        const auto* items = reinterpret_cast<const NETRESOURCEW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            const wchar_t* local = items[i].lpLocalName;
            const wchar_t* remote = items[i].lpRemoteName;
            if (!local || !remote || !iswalpha(local[0]) || local[1] != L':' || local[2] != L'\0') continue;
            fn(static_cast<wchar_t>(towupper(local[0])), std::wstring(remote));
        }
    }
    WNetCloseEnum(handle);
}

}  // namespace

std::vector<MappedNetworkDrive> ReadMappedNetworkDrives() {
    std::vector<MappedNetworkDrive> drives;
    const auto add = [&](wchar_t letter, std::wstring remote, bool connected) {
        auto it = std::find_if(drives.begin(), drives.end(),
                               [&](const MappedNetworkDrive& d) { return d.letter == letter; });
        if (it != drives.end()) {
            it->connected = it->connected || connected;
            return;
        }
        drives.push_back({letter, std::move(remote), connected});
    };
    EnumerateDisks(RESOURCE_CONNECTED, [&](wchar_t letter, std::wstring remote) { add(letter, std::move(remote), true); });
    EnumerateDisks(RESOURCE_REMEMBERED, [&](wchar_t letter, std::wstring remote) { add(letter, std::move(remote), false); });
    std::sort(drives.begin(), drives.end(),
              [](const MappedNetworkDrive& a, const MappedNetworkDrive& b) { return a.letter < b.letter; });
    return drives;
}

} // namespace pulse::app
