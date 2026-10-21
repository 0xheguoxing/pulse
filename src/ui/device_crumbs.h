// device_crumbs.h - Phone names for the breadcrumb (plan: 手机 D3).
//
// SplitBreadcrumb turns pulse:device:<key>\... into 此电脑 › 手机名 › ... The
// UI layer does not know the phones; the app registers a resolver at start-up
// (app_main.cpp, DeviceRegistry::Describe). Without one, or for a device never
// seen this session, the name segment reads 手机.
#pragma once
#include <string>

namespace pulse::ui {

struct DeviceCrumbInfo {
    std::wstring name;
    bool skips_storage = false;  // one storage: the address bar leaves it out
    bool known = false;
};

using DeviceCrumbResolver = DeviceCrumbInfo (*)(const std::wstring& key);
void SetDeviceCrumbResolver(DeviceCrumbResolver resolver);

}  // namespace pulse::ui
