// device_fake.cpp - FakeDeviceSource: a local folder stands in for phones
// (device_source.h). Tests only: the app builds it in isolated test instances
// with PULSE_TEST_FAKE_DEVICE=<root>, never otherwise.
//
//   <root>\<device folder>\device.ini     optional, UTF-8 key=value lines:
//       name=Xiaomi 14        kind=android|iphone    locked=1
//       delay_ms=50           (sleep before every batch)
//       disconnect_after=300  (Enumerate fails as Disconnected after N rows)
//       hang_ms=20000         (every call sleeps first: timeout tests)
//   <root>\<device folder>\<storage folder>\storage.ini   optional:
//       total=128000000000  free=64000000000  fs=FAT32  removable=1  read_only=1
//
// Storages are the device folder's sub-folders, in name order. A device is
// gone when its folder is: Enumerate then reports Disconnected.
#include "device_source.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cwctype>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

namespace pulse::fs {

namespace {

using Ini = std::map<std::wstring, std::wstring>;

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::wstring Trim(std::wstring text) {
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) ++start;
    return text.substr(start);
}

Ini ReadIni(const std::wstring& file) {
    Ini ini;
    std::ifstream in(file, std::ios::binary);
    if (!in) return ini;
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string bytes = buffer.str();
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF) bytes.erase(0, 3);  // BOM
    std::wstringstream lines(Widen(bytes));
    std::wstring line;
    while (std::getline(lines, line)) {
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || line[0] == L';' || line[0] == L'#') continue;
        std::wstring key = Trim(line.substr(0, eq));
        for (auto& c : key) c = static_cast<wchar_t>(towlower(c));
        ini[key] = Trim(line.substr(eq + 1));
    }
    return ini;
}

uint64_t Number(const Ini& ini, const wchar_t* key) {
    const auto it = ini.find(key);
    return it == ini.end() ? 0 : _wcstoui64(it->second.c_str(), nullptr, 10);
}

bool Flag(const Ini& ini, const wchar_t* key) { return Number(ini, key) != 0; }

std::vector<std::wstring> SubFolders(const std::wstring& dir) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        names.push_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return names;
}

class FakeDeviceSource final : public DeviceSource {
public:
    explicit FakeDeviceSource(std::wstring root) : root_(std::move(root)) {}

    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;
        folder_by_key_.clear();
        for (const auto& folder : SubFolders(root_)) {
            const std::wstring dir = root_ + L"\\" + folder;
            const Ini ini = ReadIni(dir + L"\\device.ini");
            Hang(ini);
            DeviceInfo device;
            device.id = L"fake:" + folder;
            device.key = DeviceKeyFor(device.id);
            device.name = ini.contains(L"name") ? ini.at(L"name") : folder;
            device.kind = ini.contains(L"kind") && _wcsicmp(ini.at(L"kind").c_str(), L"iphone") == 0
                ? DeviceKind::IPhone : DeviceKind::Android;
            device.locked = Flag(ini, L"locked");
            if (!device.locked) {
                for (const auto& storage_folder : SubFolders(dir)) {
                    const Ini sini = ReadIni(dir + L"\\" + storage_folder + L"\\storage.ini");
                    StorageInfo storage;
                    storage.id = dir + L"\\" + storage_folder;
                    storage.name = storage_folder;
                    storage.total = Number(sini, L"total");
                    storage.free = Number(sini, L"free");
                    storage.file_system = sini.contains(L"fs") ? sini.at(L"fs") : std::wstring();
                    storage.removable = Flag(sini, L"removable");
                    storage.read_only = Flag(sini, L"read_only") || device.kind == DeviceKind::IPhone;
                    device.storages.push_back(std::move(storage));
                }
                device.locked = device.storages.empty();
            }
            folder_by_key_[device.key] = folder;
            devices.push_back(std::move(device));
        }
        return devices;
    }

    DeviceError Enumerate(const DevicePath& folder, const DeviceBatchSink& sink,
                          const std::atomic_bool& cancel) override {
        if (folder.IsDeviceRoot()) return DeviceError::NotFound;
        std::wstring local;
        Ini ini;
        const DeviceError resolved = Resolve(folder, &local, &ini);
        if (resolved != DeviceError::None) return resolved;
        Hang(ini);
        const uint64_t delay = Number(ini, L"delay_ms");
        const uint64_t disconnect_after = Number(ini, L"disconnect_after");
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW((local + L"\\*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                       nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return DeviceError::NotFound;
        std::vector<DirEntry> batch;
        uint64_t rows = 0;
        DeviceError result = DeviceError::None;
        const auto flush = [&]() {
            if (batch.empty()) return true;
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            if (cancel.load()) return false;
            const bool more = sink(std::move(batch));
            batch.clear();
            return more;
        };
        do {
            if (cancel.load()) { result = DeviceError::Cancelled; break; }
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            if (folder.IsStorageRoot() && _wcsicmp(data.cFileName, L"storage.ini") == 0) continue;
            if (disconnect_after && rows >= disconnect_after) { result = DeviceError::Disconnected; break; }
            DirEntry entry;
            entry.name = data.cFileName;
            entry.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) entry.attrs |= FILE_ATTRIBUTE_HIDDEN;
            entry.size = entry.is_dir ? 0 : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            entry.mtime = data.ftLastWriteTime;
            entry.ctime = data.ftCreationTime;
            entry.full_path = FormatDevicePath(DeviceChild(folder, entry.name));
            batch.push_back(std::move(entry));
            ++rows;
            if (batch.size() >= kDeviceBatchSize && !flush()) {
                result = cancel.load() ? DeviceError::Cancelled : DeviceError::None;
                batch.clear();
                break;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        if (result == DeviceError::None && !flush() && cancel.load()) result = DeviceError::Cancelled;
        if (result == DeviceError::Cancelled || cancel.load()) return DeviceError::Cancelled;
        return result;
    }

    DeviceItemCaps Caps(const DevicePath& path) override {
        DeviceItemCaps caps;
        std::wstring local;
        Ini ini;
        if (path.IsDeviceRoot() || Resolve(path, &local, &ini) != DeviceError::None) return caps;
        caps.can_copy = true;
        const std::wstring storage_dir = root_ + L"\\" + folder_by_key_[path.device] + L"\\" + path.storage;
        const Ini sini = ReadIni(storage_dir + L"\\storage.ini");
        const bool read_only = Flag(sini, L"read_only") ||
            (ini.contains(L"kind") && _wcsicmp(ini.at(L"kind").c_str(), L"iphone") == 0);
        if (read_only) return caps;
        const DWORD attrs = GetFileAttributesW(local.c_str());
        caps.can_delete = !path.IsStorageRoot();
        caps.can_rename = !path.IsStorageRoot();
        caps.can_create = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        return caps;
    }

    HRESULT ShellItem(const DevicePath& path, IShellItem** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        std::wstring local;
        Ini ini;
        const DeviceError resolved = Resolve(path, &local, &ini);
        if (resolved == DeviceError::Disconnected) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        if (resolved != DeviceError::None) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        return SHCreateItemFromParsingName(local.c_str(), nullptr, IID_PPV_ARGS(out));
    }

private:
    static void Hang(const Ini& ini) {
        if (const uint64_t hang = Number(ini, L"hang_ms"))
            std::this_thread::sleep_for(std::chrono::milliseconds(hang));
    }

    DeviceError Resolve(const DevicePath& path, std::wstring* local, Ini* ini) {
        if (!folder_by_key_.contains(path.device)) ListDevices();
        const auto it = folder_by_key_.find(path.device);
        if (it == folder_by_key_.end()) return DeviceError::Disconnected;
        const std::wstring dir = root_ + L"\\" + it->second;
        const DWORD dir_attrs = GetFileAttributesW(dir.c_str());
        if (dir_attrs == INVALID_FILE_ATTRIBUTES) return DeviceError::Disconnected;
        *ini = ReadIni(dir + L"\\device.ini");
        if (Flag(*ini, L"locked")) return DeviceError::Locked;
        std::wstring at = dir;
        if (!path.storage.empty()) at += L"\\" + path.storage;
        for (const auto& part : path.parts) at += L"\\" + part;
        if (GetFileAttributesW(at.c_str()) == INVALID_FILE_ATTRIBUTES) return DeviceError::NotFound;
        *local = at;
        return DeviceError::None;
    }

    std::wstring root_;
    std::map<std::wstring, std::wstring> folder_by_key_;
};

} // namespace

std::unique_ptr<DeviceSource> MakeFakeDeviceSource(const std::wstring& root) {
    return std::make_unique<FakeDeviceSource>(root);
}

} // namespace pulse::fs
