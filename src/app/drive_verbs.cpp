// drive_verbs.cpp — see drive_verbs.h.
#include "drive_verbs.h"
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devguid.h>
#include <ntddstor.h>
#include <winnetwk.h>
#include <thread>
#include <vector>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "mpr.lib")

namespace pulse::app {
namespace {

std::wstring VolumeDevice(const std::wstring& root) {
    if (root.size() < 2 || root[1] != L':') return {};
    return std::wstring(L"\\\\.\\") + root[0] + L":";
}

bool DeviceNumber(HANDLE h, STORAGE_DEVICE_NUMBER& n) {
    DWORD bytes = 0;
    return DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &n, sizeof(n), &bytes,
                           nullptr) != FALSE;
}

// The disk device node that holds the volume.
DEVINST DiskDevInst(const std::wstring& root) {
    const std::wstring device = VolumeDevice(root);
    if (device.empty()) return 0;
    HANDLE volume = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE) return 0;
    STORAGE_DEVICE_NUMBER want{};
    const bool got = DeviceNumber(volume, want);
    CloseHandle(volume);
    if (!got) return 0;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    DEVINST disk = 0;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD i = 0; !disk && SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_DISK, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buffer(need + sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA info{sizeof(info)};
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, &info)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER n{};
        if (DeviceNumber(h, n) && n.DeviceNumber == want.DeviceNumber && n.DeviceType == want.DeviceType)
            disk = info.DevInst;
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return disk;
}

bool Removable(DEVINST node) {
    DWORD caps = 0, size = sizeof(caps), type = 0;
    return CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CAPABILITIES, &type, &caps, &size, 0) ==
               CR_SUCCESS && (caps & CM_DEVCAP_REMOVABLE);
}

// Safely Remove Hardware: the nearest removable node from the disk up (the
// USB device for a USB stick), else the disk's parent, as Explorer does.
DWORD EjectDevice(const std::wstring& root, std::wstring& veto_name) {
    const DEVINST disk = DiskDevInst(root);
    if (!disk) return ERROR_FILE_NOT_FOUND;
    DEVINST target = 0;
    for (DEVINST node = disk; node && !target;) {
        if (Removable(node)) target = node;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    if (!target && CM_Get_Parent(&target, disk, 0) != CR_SUCCESS) return ERROR_NOT_SUPPORTED;
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t name[MAX_PATH]{};
    const CONFIGRET cr = CM_Request_Device_EjectW(target, &veto, name, MAX_PATH, 0);
    if (cr == CR_SUCCESS && veto == PNP_VetoTypeUnknown && !name[0]) return NO_ERROR;
    // A program or service is worth naming; for an open handle the name is
    // only the volume's device instance path (STORAGE\Volume\{...}).
    if (veto == PNP_VetoWindowsApp || veto == PNP_VetoWindowsService) veto_name = name;
    return cr == CR_SUCCESS || cr == CR_REMOVE_VETOED ? ERROR_DEVICE_IN_USE
                                                     : CM_MapCrToWin32Err(cr, ERROR_NOT_READY);
}

// Optical drives: open the tray (Explorer's 弹出).
DWORD EjectMedia(const std::wstring& root) {
    const std::wstring device = VolumeDevice(root);
    HANDLE h = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytes, nullptr);
    const DWORD error = ok ? NO_ERROR : GetLastError();
    CloseHandle(h);
    return error;
}

void Post(HWND notify, UINT msg, DriveVerbResult* result) {
    if (!notify || !PostMessageW(notify, msg, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

int CALLBACK FontFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

} // namespace

bool DriveEjectable(const std::wstring& root, uint8_t drive_type) {
    if (drive_type == DRIVE_REMOVABLE || drive_type == DRIVE_CDROM) return true;
    if (drive_type != DRIVE_FIXED) return false;
    const std::wstring device = VolumeDevice(root);
    if (device.empty()) return false;
    HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    BYTE buffer[1024]{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer,
                                    sizeof(buffer), &bytes, nullptr);
    CloseHandle(h);
    if (!ok || bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return false;
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer);
    switch (descriptor->BusType) {
    case BusTypeUsb:
    case BusType1394:
    case BusTypeSd:
    case BusTypeMmc:
        return true;
    default:
        return descriptor->RemovableMedia != FALSE;
    }
}

const wchar_t* EjectGlyph() {
    static const bool fluent = [] {
        bool found = false;
        LOGFONTW font{};
        font.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(font.lfFaceName, L"Segoe Fluent Icons");
        HDC dc = GetDC(nullptr);
        if (dc) {
            EnumFontFamiliesExW(dc, &font, FontFound, reinterpret_cast<LPARAM>(&found), 0);
            ReleaseDC(nullptr, dc);
        }
        return found;
    }();
    return fluent ? L"\xF847" : L"\xE88E";  // Eject : USB
}

bool StartDriveEject(HWND notify, UINT msg, const std::wstring& root, uint8_t drive_type) {
    try {
        std::thread([notify, msg, root, drive_type] {
            auto* result = new DriveVerbResult;
            result->verb = DriveVerb::Eject;
            result->root = root;
            result->error = drive_type == DRIVE_CDROM ? EjectMedia(root) : EjectDevice(root, result->veto_name);
            Post(notify, msg, result);
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}

bool StartDriveDisconnect(HWND notify, UINT msg, const std::wstring& root, bool force) {
    if (root.size() < 2 || root[1] != L':') return false;
    try {
        std::thread([notify, msg, root, force] {
            auto* result = new DriveVerbResult;
            result->verb = DriveVerb::Disconnect;
            result->root = root;
            const std::wstring local = root.substr(0, 2);
            // Like Explorer: the mapping is gone for good, not only this session.
            result->error = WNetCancelConnection2W(local.c_str(), CONNECT_UPDATE_PROFILE, force);
            result->open_files = !force &&
                (result->error == ERROR_OPEN_FILES || result->error == ERROR_DEVICE_IN_USE);
            Post(notify, msg, result);
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}

DWORD MapNetworkDrive(HWND owner, const std::wstring& share) {
    if (share.empty()) {
        const DWORD error = WNetConnectionDialog(owner, RESOURCETYPE_DISK);
        return error == static_cast<DWORD>(-1) ? ERROR_CANCELLED : error;
    }
    std::wstring remote = share;
    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = remote.data();
    CONNECTDLGSTRUCTW dialog{};
    dialog.cbStructure = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpConnRes = &resource;
    dialog.dwFlags = CONNDLG_RO_PATH;  // prefill the selected share; this flag excludes USE_MRU
    const DWORD error = WNetConnectionDialog1W(&dialog);
    return error == static_cast<DWORD>(-1) ? ERROR_CANCELLED : error;
}

} // namespace pulse::app
