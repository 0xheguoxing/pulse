// device_path.cpp - pulse:device: path parsing (device_path.h).
#include "device_path.h"
#include "device_source.h"

#include <windows.h>
#include <cwchar>
#include <cwctype>

namespace pulse::fs {

namespace {

bool ValidSegment(const std::wstring& segment) {
    if (segment.empty() || segment == L"." || segment == L"..") return false;
    for (const wchar_t c : segment)
        if (c < 0x20 || c == L'/') return false;
    return true;
}

} // namespace

bool IsDevicePath(const std::wstring& path) {
    return path.size() >= std::size(kDevicePathPrefix) - 1 &&
           _wcsnicmp(path.c_str(), kDevicePathPrefix, std::size(kDevicePathPrefix) - 1) == 0;
}

bool IsValidDeviceKey(const std::wstring& key) {
    if (key.size() != 12) return false;
    for (const wchar_t c : key)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    return true;
}

std::optional<DevicePath> ParseDevicePath(const std::wstring& path) {
    if (!IsDevicePath(path)) return std::nullopt;
    std::wstring rest = path.substr(std::size(kDevicePathPrefix) - 1);
    if (!rest.empty() && rest.back() == L'\\') rest.pop_back();  // "key\" = "key"
    std::vector<std::wstring> segments;
    size_t start = 0;
    for (;;) {
        const size_t slash = rest.find(L'\\', start);
        segments.push_back(rest.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start));
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    DevicePath parsed;
    parsed.device = segments.front();
    for (auto& c : parsed.device) c = static_cast<wchar_t>(towlower(c));
    if (!IsValidDeviceKey(parsed.device)) return std::nullopt;
    for (size_t i = 1; i < segments.size(); ++i) {
        if (!ValidSegment(segments[i])) return std::nullopt;
        if (i == 1) parsed.storage = std::move(segments[i]);
        else parsed.parts.push_back(std::move(segments[i]));
    }
    return parsed;
}

std::wstring FormatDevicePath(const DevicePath& path) {
    std::wstring out = kDevicePathPrefix + path.device;
    if (path.storage.empty()) return out;
    out += L'\\';
    out += path.storage;
    for (const auto& part : path.parts) {
        out += L'\\';
        out += part;
    }
    return out;
}

DevicePath DeviceChild(const DevicePath& folder, const std::wstring& name) {
    DevicePath child = folder;
    if (child.storage.empty()) child.storage = name;
    else child.parts.push_back(name);
    return child;
}

std::wstring DeviceKeyFor(const std::wstring& device_id) {
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t raw : device_id) {
        const wchar_t c = static_cast<wchar_t>(towupper(raw));
        hash ^= static_cast<uint16_t>(c);
        hash *= 1099511628211ull;
    }
    wchar_t text[17]{};
    swprintf_s(text, L"%016llx", static_cast<unsigned long long>(hash));
    return std::wstring(text, 12);
}

std::wstring DeviceOpenTarget(const DeviceInfo& device) {
    DevicePath root;
    root.device = device.key;
    if (SkipsStorageLevel(device) && !device.locked) root.storage = device.storages.front().name;
    return FormatDevicePath(root);
}

} // namespace pulse::fs
