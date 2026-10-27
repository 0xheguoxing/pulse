// device_shell.cpp - ShellDeviceSource: phones through the This PC namespace,
// the same items File Explorer shows (device_source.h). Runs on the device
// STA thread only.
#include "device_source.h"

#include "../common/shell_id.h"

#include <windows.h>
#include <cfgmgr32.h>
#include <objbase.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace pulse::fs {

namespace {

// GUID_DEVINTERFACE_WPD (portabledevice.h), spelled out to keep the WPD
// headers and DLLs out of Pulse.
constexpr GUID kWpdInterfaceClass = {0x6AC27878, 0xA6FA, 0x4155, {0xBA, 0x85, 0xF9, 0x8F, 0x49, 0x1D, 0x4F, 0x33}};

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

std::wstring DisplayName(IShellItem* item, SIGDN form) {
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(form, &raw)) || !raw) return {};
    std::wstring name(raw);
    CoTaskMemFree(raw);
    return name;
}

// File names keep their extension (System.FileName); the display name may
// hide it when Explorer hides known extensions.
std::wstring ItemName(IShellItem* item) {
    ComPtr<IShellItem2> item2;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2)))) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(item2->GetString(PKEY_FileName, &raw)) && raw) {
            std::wstring name(raw);
            CoTaskMemFree(raw);
            if (!name.empty()) return name;
        }
    }
    return DisplayName(item, SIGDN_NORMALDISPLAY);
}

uint64_t UInt64Property(IShellItem* item, const PROPERTYKEY& key) {
    ComPtr<IShellItem2> item2;
    ULONGLONG value = 0;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2))) && SUCCEEDED(item2->GetUInt64(key, &value)))
        return value;
    return 0;
}

std::wstring StringProperty(IShellItem* item, const PROPERTYKEY& key) {
    ComPtr<IShellItem2> item2;
    PWSTR raw = nullptr;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item2))) || FAILED(item2->GetString(key, &raw)) || !raw)
        return {};
    std::wstring text(raw);
    CoTaskMemFree(raw);
    return text;
}

template <typename Visit>
bool ForEachChild(IShellItem* folder, const std::atomic_bool* cancel, Visit&& visit) {
    ComPtr<IEnumShellItems> children;
    if (FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&children))) || !children)
        return false;
    for (;;) {
        if (cancel && cancel->load()) return true;
        ComPtr<IShellItem> child;
        const HRESULT hr = children->Next(1, &child, nullptr);
        if (hr != S_OK || !child) break;
        if (!visit(child.Get())) break;
    }
    return true;
}

// MTP / PTP devices in This PC: folders that are not file system items and
// whose parsing name carries the device interface path.
bool IsPortableDeviceItem(IShellItem* item, std::wstring* parsing) {
    SFGAOF attrs = 0;
    if (FAILED(item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attrs))) return false;
    if ((attrs & SFGAO_FILESYSTEM) || !(attrs & SFGAO_FOLDER)) return false;
    *parsing = DisplayName(item, SIGDN_DESKTOPABSOLUTEPARSING);
    const std::wstring lower = Lower(*parsing);
    return lower.find(L"\\\\?\\usb#") != std::wstring::npos ||
           lower.find(L"\\\\?\\swd#wpdbusenum") != std::wstring::npos;
}

class ShellDeviceSource final : public DeviceSource {
public:
    std::vector<DeviceInfo> ListDevices() override {
        std::vector<DeviceInfo> devices;
        ComPtr<IShellItem> computer;
        if (FAILED(SHGetKnownFolderItem(FOLDERID_ComputerFolder, KF_FLAG_DEFAULT, nullptr,
                                        IID_PPV_ARGS(&computer))) || !computer)
            return devices;
        std::unordered_map<std::wstring, std::wstring> parsing_by_key;
        ForEachChild(computer.Get(), nullptr, [&](IShellItem* child) {
            std::wstring parsing;
            if (!IsPortableDeviceItem(child, &parsing)) return true;
            DeviceInfo device;
            device.id = parsing;
            device.key = DeviceKeyFor(parsing);
            device.name = DisplayName(child, SIGDN_NORMALDISPLAY);
            // Apple's USB vendor id: iPhone / iPad, read-only (DCIM only).
            const std::wstring lower = Lower(parsing);
            device.kind = lower.find(L"vid_05ac") != std::wstring::npos ? DeviceKind::IPhone : DeviceKind::Android;
            ForEachChild(child, nullptr, [&](IShellItem* storage_item) {
                SFGAOF attrs = 0;
                if (FAILED(storage_item->GetAttributes(SFGAO_FOLDER, &attrs)) || !(attrs & SFGAO_FOLDER))
                    return true;
                StorageInfo storage;
                storage.id = shell_id::FromItem(storage_item);  // parsing names below the device do not parse back
                storage.name = DisplayName(storage_item, SIGDN_NORMALDISPLAY);
                storage.total = UInt64Property(storage_item, PKEY_Capacity);
                storage.free = UInt64Property(storage_item, PKEY_FreeSpace);
                storage.file_system = StringProperty(storage_item, PKEY_Volume_FileSystem);
                storage.read_only = device.kind == DeviceKind::IPhone;
                // The first storage is the phone's own; later ones are cards.
                storage.removable = !device.storages.empty();
                if (!storage.name.empty()) device.storages.push_back(std::move(storage));
                return true;
            });
            device.locked = device.storages.empty();
            parsing_by_key[device.key] = parsing;
            devices.push_back(std::move(device));
            return true;
        });
        parsing_by_key_ = std::move(parsing_by_key);
        return devices;
    }

    DeviceError Enumerate(const DevicePath& folder, const DeviceBatchSink& sink,
                          const std::atomic_bool& cancel) override {
        if (folder.IsDeviceRoot()) return DeviceError::NotFound;
        ComPtr<IShellItem> item;
        const DeviceError resolved = Resolve(folder, &item);
        if (resolved != DeviceError::None) return resolved;
        // Remembered for ShellId(): the folder just listed is the one the
        // app asks for next (paste into it, its properties).
        last_listed_ = FormatDevicePath(folder);
        last_listed_id_ = shell_id::FromItem(item.Get());
        std::vector<DirEntry> batch;
        batch.reserve(kDeviceBatchSize);
        bool stopped = false;
        const bool listed = ForEachChild(item.Get(), &cancel, [&](IShellItem* child) {
            DirEntry entry;
            entry.name = ItemName(child);
            if (entry.name.empty()) return true;
            SFGAOF attrs = 0;
            const HRESULT attrs_hr = child->GetAttributes(
                SFGAO_FOLDER | SFGAO_STREAM | SFGAO_HIDDEN | SFGAO_CANDELETE | SFGAO_CANRENAME, &attrs);
            entry.is_dir = (attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM);
            entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (attrs & SFGAO_HIDDEN) entry.attrs |= FILE_ATTRIBUTE_HIDDEN;
            // Read-only = the shell offers neither delete nor rename: the app
            // then hides both. What the shell grants (an iPhone's photos can
            // be deleted, as in Explorer) is what Pulse offers.
            if (SUCCEEDED(attrs_hr) && !(attrs & (SFGAO_CANDELETE | SFGAO_CANRENAME)))
                entry.attrs |= FILE_ATTRIBUTE_READONLY;
            entry.shell_id = shell_id::FromItem(child);
            ComPtr<IShellItem2> item2;
            if (SUCCEEDED(child->QueryInterface(IID_PPV_ARGS(&item2)))) {
                ULONGLONG size = 0;
                if (!entry.is_dir && SUCCEEDED(item2->GetUInt64(PKEY_Size, &size))) entry.size = size;
                item2->GetFileTime(PKEY_DateModified, &entry.mtime);
                item2->GetFileTime(PKEY_DateCreated, &entry.ctime);
            }
            entry.full_path = FormatDevicePath(DeviceChild(folder, entry.name));
            batch.push_back(std::move(entry));
            if (batch.size() >= kDeviceBatchSize) {
                if (!sink(std::move(batch))) { stopped = true; return false; }
                batch.clear();
                batch.reserve(kDeviceBatchSize);
            }
            return true;
        });
        if (cancel.load()) return DeviceError::Cancelled;
        if (!listed) return StillConnected(folder.device) ? DeviceError::Failed : DeviceError::Disconnected;
        if (!stopped && !batch.empty()) sink(std::move(batch));
        return DeviceError::None;
    }

    DeviceItemCaps Caps(const DevicePath& path) override {
        DeviceItemCaps caps;
        ComPtr<IShellItem> item;
        if (path.IsDeviceRoot() || Resolve(path, &item) != DeviceError::None) return caps;
        SFGAOF attrs = 0;
        item->GetAttributes(SFGAO_CANDELETE | SFGAO_CANRENAME | SFGAO_FOLDER | SFGAO_READONLY, &attrs);
        caps.can_copy = true;
        caps.can_delete = (attrs & SFGAO_CANDELETE) != 0 && !path.IsStorageRoot();
        caps.can_rename = (attrs & SFGAO_CANRENAME) != 0 && !path.IsStorageRoot();
        caps.can_create = (attrs & SFGAO_FOLDER) != 0 && !(attrs & SFGAO_READONLY);
        return caps;
    }

    std::wstring ShellId(const DevicePath& path) override {
        if (path.IsDeviceRoot()) return {};
        const std::wstring formatted = FormatDevicePath(path);
        if (!last_listed_id_.empty() && _wcsicmp(formatted.c_str(), last_listed_.c_str()) == 0)
            return last_listed_id_;
        ComPtr<IShellItem> item;
        if (Resolve(path, &item) != DeviceError::None) return {};
        return shell_id::FromItem(item.Get());
    }

    HRESULT ShellItem(const DevicePath& path, IShellItem** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        ComPtr<IShellItem> item;
        const DeviceError resolved = Resolve(path, &item);
        if (resolved == DeviceError::Disconnected) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        if (resolved != DeviceError::None) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        *out = item.Detach();
        return S_OK;
    }

private:
    std::wstring DeviceId(const std::wstring& key) {
        if (!parsing_by_key_.contains(key)) ListDevices();
        const auto it = parsing_by_key_.find(key);
        return it == parsing_by_key_.end() ? std::wstring() : it->second;
    }

    bool StillConnected(const std::wstring& key) {
        ListDevices();
        return parsing_by_key_.contains(key);
    }

    // Device item by its parsing name, then one child per segment matched by
    // name (a folder's children stay cached while it is open: D3).
    DeviceError Resolve(const DevicePath& path, ComPtr<IShellItem>* out) {
        const std::wstring id = DeviceId(path.device);
        if (id.empty()) return DeviceError::Disconnected;
        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(id.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item)
            return DeviceError::Disconnected;
        std::vector<const std::wstring*> segments;
        if (!path.storage.empty()) segments.push_back(&path.storage);
        for (const auto& part : path.parts) segments.push_back(&part);
        for (size_t i = 0; i < segments.size(); ++i) {
            ComPtr<IShellItem> found;
            const bool storage_level = i == 0;
            const bool listed = ForEachChild(item.Get(), nullptr, [&](IShellItem* child) {
                const std::wstring name = storage_level ? DisplayName(child, SIGDN_NORMALDISPLAY) : ItemName(child);
                if (_wcsicmp(name.c_str(), segments[i]->c_str()) != 0) return true;
                found = child;
                return false;
            });
            if (!listed && storage_level) return DeviceError::Locked;
            if (!found) return DeviceError::NotFound;
            item = std::move(found);
        }
        *out = std::move(item);
        return DeviceError::None;
    }

    std::unordered_map<std::wstring, std::wstring> parsing_by_key_;
    std::wstring last_listed_;     // pulse:device: path of the last Enumerate
    std::wstring last_listed_id_;  // and its shell id (ID list)
};

} // namespace

std::unique_ptr<DeviceSource> MakeShellDeviceSource() {
    return std::make_unique<ShellDeviceSource>();
}

bool AnyPortableDevicePresent() {
    ULONG length = 0;
    GUID wpd = kWpdInterfaceClass;
    if (CM_Get_Device_Interface_List_SizeW(&length, &wpd, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
        CR_SUCCESS)
        return false;
    return length > 1;  // an empty list is a single terminating NUL
}

} // namespace pulse::fs
