// lan_discovery.h - 局域网设备: a short, passive-first look for file servers on
// the current network. mDNS (_smb._tcp / _device-info) through
// DnsServiceBrowse and WS-Discovery through Function Discovery. No subnet
// scan, no SMB sign-in, nothing at all on a Public network.
#pragma once
#include "lan_device.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

struct LanDiscoveryResult {
    std::vector<LanDevice> devices;
    std::wstring network_key;     // the connected network's id (cache key), empty when unknown
    bool public_network = false;  // every connected network is Public: nothing was sent
    bool offline = false;         // no connected network
    bool mdns_unavailable = false;  // DnsServiceBrowse missing (Windows before 10 1809)
    bool cancelled = false;
};

// The connected network's id and whether it is Public, without sending
// anything. Worker-only (COM). Initializes COM internally.
void ReadLanNetworkIdentity(LanDiscoveryResult& out);

// Worker-only. Browses for up to `budget_ms`, polling `cancel` meanwhile.
// Initializes COM internally (an existing STA or MTA is also fine).
LanDiscoveryResult RunLanDiscovery(const std::function<bool()>& cancel, DWORD budget_ms = 3000);

} // namespace pulse::app
