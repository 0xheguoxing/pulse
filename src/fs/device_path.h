// device_path.h - pulse:device: locations (phones, see device_source.h).
//
//   pulse:device:<key>\<storage>\DCIM\Camera
//
// <key> is DeviceKeyFor(device id): 12 lowercase hex digits, stable for a
// device, so the path can go into the session and the history. Segments
// are display names; empty, "." and ".." segments are rejected.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace pulse::fs {

inline constexpr wchar_t kDevicePathPrefix[] = L"pulse:device:";

struct DevicePath {
    std::wstring device;              // the key
    std::wstring storage;             // empty = the device root
    std::vector<std::wstring> parts;  // folders / file below the storage
    bool operator==(const DevicePath&) const = default;
    bool IsDeviceRoot() const { return storage.empty(); }
    bool IsStorageRoot() const { return !storage.empty() && parts.empty(); }
};

bool IsDevicePath(const std::wstring& path);
std::optional<DevicePath> ParseDevicePath(const std::wstring& path);
std::wstring FormatDevicePath(const DevicePath& path);
// `folder` plus one more segment (a storage below a device root).
DevicePath DeviceChild(const DevicePath& folder, const std::wstring& name);
// FNV-1a 64 of the case-folded id, as 12 hex digits.
std::wstring DeviceKeyFor(const std::wstring& device_id);
bool IsValidDeviceKey(const std::wstring& key);

} // namespace pulse::fs
