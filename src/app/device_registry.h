// device_registry.h - The phones connected right now (plan: 手机 D1).
//
// Owns the device thread (fs::DeviceWorker) and the DeviceSource that runs
// on it. Portable-device arrivals / removals (WM_DEVICECHANGE for
// GUID_DEVINTERFACE_WPD, see host_routing.h) refresh the list after 500 ms
// of quiet; while a phone waits for 允许 / 信任 the list is checked again
// every 2 s, for up to 5 minutes. A changed list bumps Revision() and posts
// WM_DEVICES_CHANGED to the notify window (the host routes it to a window).
//
// Without a phone nothing runs: the start-up check is a cfgmgr32 query and
// the device thread only exists while there is device work.
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "../fs/device_source.h"
#include "../fs/device_worker.h"

namespace pulse::app {

class DeviceRegistry {
public:
    static DeviceRegistry& Instance();

    // UI thread, once per process (later calls are ignored). Registers the
    // notification on `notify` and lists devices when one is present.
    void Start(HWND notify);
    void Stop();

    // A portable-device interface came or went (SystemBroadcast::Devices).
    void OnDevicesBroadcast();

    std::vector<fs::DeviceInfo> Devices() const;
    uint64_t Revision() const;

    // The device thread and the source behind it (D3: enumeration). The
    // source must only be used from tasks posted to Worker().
    fs::DeviceWorker& Worker() { return worker_; }
    fs::DeviceSource* SourceOnWorker();

private:
    DeviceRegistry();
    static void CALLBACK OnTimer(PTP_CALLBACK_INSTANCE, void*, PTP_TIMER);
    void ScheduleRefresh(uint32_t delay_ms);
    void RefreshOnWorker();
    void Publish(std::vector<fs::DeviceInfo> devices);

    mutable std::mutex mutex_;
    HWND notify_ = nullptr;
    HDEVNOTIFY registration_ = nullptr;
    bool started_ = false;
    bool stopped_ = false;
    bool test_source_ = false;  // FakeDeviceSource (isolated test instance)
    std::wstring fake_root_;
    std::vector<fs::DeviceInfo> devices_;
    uint64_t revision_ = 0;
    uint32_t locked_checks_ = 0;
    std::unique_ptr<fs::DeviceSource> source_;  // device thread only
    PTP_TIMER timer_ = nullptr;
    fs::DeviceWorker worker_;
};

} // namespace pulse::app
