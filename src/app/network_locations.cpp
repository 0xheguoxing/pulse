#include "network_locations.h"
#include <shlobj.h>
#include <netfw.h>
#include <oleauto.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>

namespace pulse::app {
namespace {
using Microsoft::WRL::ComPtr;

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
struct FindScope {
    HANDLE handle;
    ~FindScope() { if (handle != INVALID_HANDLE_VALUE) FindClose(handle); }
};
struct BstrScope {
    explicit BstrScope(BSTR value) : value(value) {}
    ~BstrScope() { if (value) SysFreeString(value); }
    BSTR value;
};

NetworkDiscoveryState QueryNetworkDiscoveryState() {
    ComPtr<INetFwPolicy2> firewall;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&firewall)))) return NetworkDiscoveryState::Unknown;

    long active_profiles = 0;
    if (FAILED(firewall->get_CurrentProfileTypes(&active_profiles))) return NetworkDiscoveryState::Unknown;

    BstrScope group(SysAllocString(L"@FirewallAPI.dll,-32752")); // Windows' localized Network Discovery group.
    if (!group.value) return NetworkDiscoveryState::Unknown;

    const NET_FW_PROFILE_TYPE2 profiles[] = {
        NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC};
    std::vector<NetworkDiscoveryState> states;
    for (const NET_FW_PROFILE_TYPE2 profile : profiles) {
        const long profile_mask = static_cast<long>(profile);
        if ((active_profiles & profile_mask) == 0) continue;
        VARIANT_BOOL firewall_enabled = VARIANT_FALSE;
        if (firewall->get_FirewallEnabled(profile, &firewall_enabled) != S_OK || firewall_enabled != VARIANT_TRUE) {
            states.push_back(NetworkDiscoveryState::Unknown);
            continue;
        }
        VARIANT_BOOL discovery_enabled = VARIANT_FALSE;
        const HRESULT hr = firewall->IsRuleGroupEnabled(profile_mask, group.value, &discovery_enabled);
        states.push_back(hr == S_OK
            ? (discovery_enabled == VARIANT_TRUE ? NetworkDiscoveryState::Enabled : NetworkDiscoveryState::Disabled)
            : NetworkDiscoveryState::Unknown);
    }
    return SummarizeNetworkDiscoveryProfiles(states);
}

bool Equal(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring Key(std::wstring path) {
    return NetworkLocationKey(std::move(path));
}

bool DirectUnc(const std::wstring& path, DWORD attributes) {
    const auto normalized = Key(path);
    if (!normalized.starts_with(L"\\\\") || normalized.starts_with(L"\\\\?\\") ||
        normalized.starts_with(L"\\\\.\\")) return false;
    const auto separator = normalized.find(L'\\', 2);
    if (separator == std::wstring::npos || separator <= 2 ||
        separator + 1 >= normalized.size() || normalized[separator + 1] == L'\\') return false;
    return normalized.find(L'\\', separator + 1) == std::wstring::npos ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadLink(const std::wstring& local_path, const std::wstring& name, NetworkLocation& item) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) ||
        FAILED(file->Load(local_path.c_str(), STGM_READ))) return false;
    item = {name, local_path, true};
    std::array<wchar_t, 32768> target{}, arguments{};
    WIN32_FIND_DATAW stored_data{};
    if (SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))) &&
        arguments[0] == L'\0' &&
        SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), &stored_data, SLGP_RAWPATH)) &&
        DirectUnc(target.data(), stored_data.dwFileAttributes)) item = {name, target.data(), false};
    return true;
}
} // namespace

NetworkDiscoveryState SummarizeNetworkDiscoveryProfiles(
    const std::vector<NetworkDiscoveryState>& active_profiles) {
    if (active_profiles.empty()) return NetworkDiscoveryState::Unknown;
    bool all_enabled = true;
    bool all_disabled = true;
    for (const auto state : active_profiles) {
        all_enabled &= state == NetworkDiscoveryState::Enabled;
        all_disabled &= state == NetworkDiscoveryState::Disabled;
    }
    if (all_enabled) return NetworkDiscoveryState::Enabled;
    if (all_disabled) return NetworkDiscoveryState::Disabled;
    return NetworkDiscoveryState::Unknown;
}

NetworkLocationScan ReadNetworkLocations(const std::wstring& root, const std::function<bool()>& cancel) {
    NetworkLocationScan result;
    auto stopped = [&] {
        if (cancel && cancel()) {
            result.items.clear();
            result.cancelled = true;
        }
        return result.cancelled;
    };
    auto fail = [&](DWORD error) {
        result.items.clear();
        result.error = error;
        return result;
    };
    if (stopped()) return result;
    ComScope com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) return fail(static_cast<DWORD>(com.hr));
    // Fixtures stay deterministic and never read the host's firewall state.
    if (root.empty()) result.discovery = QueryNetworkDiscoveryState();
    std::wstring directory = root;
    if (directory.empty()) {
        PWSTR folder = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_NetHood, KF_FLAG_DONT_VERIFY, nullptr, &folder);
        if (SUCCEEDED(hr) && folder) directory = folder;
        CoTaskMemFree(folder);
        if (FAILED(hr)) return fail(static_cast<DWORD>(hr));
    }
    // Even an explicit fixture root must be local; no attributes query on UNC roots.
    auto normalized = Key(directory);
    if (normalized.starts_with(L"\\\\?\\") && normalized.size() >= 7 && normalized[5] == L':')
        normalized.erase(0, 4);
    if (normalized.starts_with(L"\\\\") || normalized.size() < 3 || normalized[1] != L':' ||
        normalized[2] != L'\\') return fail(ERROR_BAD_PATHNAME);
    if (GetDriveTypeW(normalized.substr(0, 3).c_str()) == DRIVE_REMOTE) return fail(ERROR_BAD_PATHNAME);
    if (directory.size() >= MAX_PATH - 12 && !directory.starts_with(L"\\\\?\\"))
        directory = L"\\\\?\\" + normalized;
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return result;
        return fail(error);
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return fail(ERROR_BAD_PATHNAME);
    if (directory.back() != L'\\' && directory.back() != L'/') directory += L'\\';
    WIN32_FIND_DATAW data{};
    FindScope find{FindFirstFileW((directory + L"*").c_str(), &data)};
    if (find.handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? result : fail(error);
    }
    size_t count = 0;
    do {
        if (stopped()) return result;
        if (++count > 4096) return fail(ERROR_BUFFER_OVERFLOW);
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L".." || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        NetworkLocation item;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const auto link_path = directory + name + L"\\target.lnk";
            const DWORD link_attributes = GetFileAttributesW(link_path.c_str());
            if (link_attributes != INVALID_FILE_ATTRIBUTES &&
                !(link_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                ReadLink(link_path, name, item)) result.items.push_back(std::move(item));
        } else if (name.size() > 4 && Equal(name.substr(name.size() - 4), L".lnk")) {
            if (ReadLink(directory + name, name.substr(0, name.size() - 4), item))
                result.items.push_back(std::move(item));
        } else if (name.size() > 4 && Equal(name.substr(name.size() - 4), L".url")) {
            result.items.push_back({name.substr(0, name.size() - 4), directory + name, true});
        }
    } while (FindNextFileW(find.handle, &data));
    const DWORD error = GetLastError();
    if (stopped()) return result;
    if (error != ERROR_NO_MORE_FILES) return fail(error);
    std::stable_sort(result.items.begin(), result.items.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return result;
}

std::vector<NetworkLocation> MergeNetworkLocations(const std::vector<NetworkLocation>& pinned,
    const std::vector<NetworkLocation>& system) {
    std::vector<NetworkLocation> result;
    std::vector<std::wstring> keys;
    auto append = [&](const auto& items) {
        for (const auto& item : items) {
            if (item.path.empty()) continue;
            const auto key = Key(item.path);
            if (std::any_of(keys.begin(), keys.end(), [&](const auto& existing) { return Equal(key, existing); })) continue;
            keys.push_back(key);
            result.push_back(item);
        }
    };
    append(pinned);
    append(system);
    return result;
}
} // namespace pulse::app
