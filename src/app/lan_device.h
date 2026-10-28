// lan_device.h - Devices found on the local network (局域网设备) and how
// answers from different protocols collapse into one row. Pure; the
// Windows-facing browse lives in lan_discovery.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

enum LanSource : uint8_t {
    kLanSourceMdns = 1,  // _smb._tcp.local (macOS, Synology, QNAP, Samba with Avahi)
    kLanSourceWsd = 2,   // WS-Discovery via Function Discovery (Windows PCs, many NAS)
};

struct LanDevice {
    std::wstring name;   // friendly name ("DS920", "Mac mini")
    std::wstring host;   // resolvable name ("ds920.local", "DESKTOP-7")
    std::wstring ip;     // IPv4 when known
    std::wstring model;  // "DS920+" from _device-info / WSD, may be empty
    uint8_t sources = 0;
};

// "DS920._smb._tcp.local" -> "DS920"; DNS-SD escapes (\032, \.) are decoded.
std::wstring MdnsInstanceName(std::wstring_view ptr_target);
// TXT strings "model=MacMini9,1" -> "MacMini9,1"; empty when absent.
std::wstring TxtValue(const std::vector<std::wstring>& txt, std::wstring_view key);
// Adds or merges `device` into `list`: the same IP, or the same host / name
// (case-insensitive, ".local" ignored) is one device. Returns true when the
// list changed.
bool MergeLanDevice(std::vector<LanDevice>& list, const LanDevice& device);
// \\host for opening: the host name when known, else the IP.
std::wstring LanDeviceUnc(const LanDevice& device);

// What a device row looks like (DirEntry::net_kind): NAS brands and model
// prefixes (Synology DS/RS, QNAP TS/TVS, TrueNAS, Unraid ...), Macs by their
// _device-info model (MacMini9,1, iMac21,1 ...), everything else a PC.
enum LanDeviceKindValue : uint8_t { kLanKindNone = 0, kLanKindPc = 1, kLanKindNas = 2, kLanKindMac = 3 };
uint8_t LanDeviceKind(const LanDevice& device);

} // namespace pulse::app
