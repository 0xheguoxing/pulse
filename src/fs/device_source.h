// device_source.h - Phones and other portable devices (MTP / PTP), browsed
// through the Windows Shell namespace like File Explorer does.
//
// A DeviceSource answers on one thread only: the device STA thread
// (device_worker.h). Nothing here touches the UI; the app side keeps the
// device list (app/device_registry.h) and maps pulse:device: paths
// (device_path.h) onto Enumerate.
//
// ShellDeviceSource is the real implementation. FakeDeviceSource stands in
// for a phone in tests: a local folder per device, only built in isolated
// test instances (PULSE_TEST_DATA_DIR + PULSE_TEST_FAKE_DEVICE).
#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "device_path.h"
#include "fs_enum.h"

namespace pulse::fs {

enum class DeviceKind : uint8_t { Android, IPhone, Other };

struct StorageInfo {
    std::wstring id;           // shell parsing name (fake: local folder)
    std::wstring name;         // what Explorer shows (内部共享存储空间, SD 卡)
    std::wstring file_system;  // "FAT32", "exFAT", ... empty = not reported
    uint64_t total = 0;        // bytes, 0 = not reported
    uint64_t free = 0;
    bool read_only = false;
    bool removable = false;    // an SD card
    bool operator==(const StorageInfo&) const = default;
};

struct DeviceInfo {
    std::wstring id;    // shell parsing name of the device (fake: "fake:<folder>")
    std::wstring key;   // DeviceKeyFor(id): the pulse:device: path segment
    std::wstring name;  // what Explorer shows (Xiaomi 14, Apple iPhone)
    DeviceKind kind = DeviceKind::Android;
    // Connected but no storage visible: the phone waits for 允许 / 信任.
    bool locked = false;
    std::vector<StorageInfo> storages;
    bool operator==(const DeviceInfo&) const = default;
};

struct DeviceItemCaps {
    bool can_copy = false;    // copy to the PC
    bool can_delete = false;
    bool can_rename = false;
    bool can_create = false;  // new folder / paste into (folders only)
    bool operator==(const DeviceItemCaps&) const = default;
};

enum class DeviceError : uint8_t {
    None,
    NotFound,      // the path names no item (bad path, item gone)
    Disconnected,  // the device went away (or never was here)
    Locked,        // the device is connected but shows no storage yet
    Cancelled,
    Failed,        // any other shell / driver failure
};

// Rows of one folder, at most kDeviceBatchSize per call; full_path is the
// pulse:device: path of the row. Return false to stop the enumeration.
constexpr size_t kDeviceBatchSize = 200;
using DeviceBatchSink = std::function<bool(std::vector<DirEntry>&& batch)>;

class DeviceSource {
public:
    virtual ~DeviceSource() = default;
    virtual std::vector<DeviceInfo> ListDevices() = 0;
    // Lists a storage or folder (not a device root: that is ListDevices).
    virtual DeviceError Enumerate(const DevicePath& folder, const DeviceBatchSink& sink,
                                  const std::atomic_bool& cancel) = 0;
    virtual DeviceItemCaps Caps(const DevicePath& item) = 0;
    // The shell item behind a path, for copies and thumbnails. *out is
    // AddRef'd on success.
    virtual HRESULT ShellItem(const DevicePath& item, IShellItem** out) = 0;
};

std::unique_ptr<DeviceSource> MakeShellDeviceSource();
// Test stand-in: every sub-folder of `root` is a device (see device_fake.cpp).
std::unique_ptr<DeviceSource> MakeFakeDeviceSource(const std::wstring& root);

// Cheap presence check (no COM, no shell): is any portable-device interface
// present? False means there is no phone, so no device thread is started.
bool AnyPortableDevicePresent();

// DirEntry::pc_device bits for This PC rows.
constexpr uint8_t kPcDevicePhone = 1;
constexpr uint8_t kPcDeviceIPhone = 2;   // read-only
constexpr uint8_t kPcDeviceLocked = 4;   // no storage until Allow / Trust
inline uint8_t PcDeviceFlags(const DeviceInfo& device) {
    return static_cast<uint8_t>(kPcDevicePhone | (device.kind == DeviceKind::IPhone ? kPcDeviceIPhone : 0) |
                                (device.locked ? kPcDeviceLocked : 0));
}

// Where opening a device row goes: the device root, or straight into its
// only storage (single-storage phones skip that level, the address bar
// still shows the device name).
std::wstring DeviceOpenTarget(const DeviceInfo& device);
inline bool SkipsStorageLevel(const DeviceInfo& device) { return device.storages.size() == 1; }

} // namespace pulse::fs
