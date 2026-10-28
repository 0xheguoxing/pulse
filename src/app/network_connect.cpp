// network_connect.cpp — see network_connect.h.
#include "network_connect.h"

#include "../common/localization.h"

#include <winnetwk.h>
#include <cwchar>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace pulse::app {
namespace {

constexpr ULONGLONG kQuietAfterSuccessMs = 15000;

std::mutex g_mutex;
std::set<std::wstring> g_pending;                 // lower-case share roots
std::map<std::wstring, ULONGLONG> g_succeeded;     // lower-case share root -> tick

std::wstring Key(std::wstring share) {
    for (auto& c : share) c = static_cast<wchar_t>(towlower(c));
    return share;
}

void Connect(HWND owner, HWND notify, UINT msg, std::wstring share) {
    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = share.data();
    // No local name: a deviceless connection, like typing the path in Explorer.
    const DWORD error = WNetAddConnection3W(IsWindow(owner) ? owner : nullptr, &resource,
                                            nullptr, nullptr, CONNECT_INTERACTIVE);
    {
        std::lock_guard lock(g_mutex);
        const std::wstring key = Key(share);
        g_pending.erase(key);
        if (error == NO_ERROR) g_succeeded[key] = GetTickCount64();
    }
    auto* result = new NetworkConnectResult{std::move(share), error};
    if (!IsWindow(notify) || !PostMessageW(notify, msg, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

} // namespace

std::wstring UncShareRoot(const std::wstring& path) {
    std::wstring rest;
    if (path.size() > 8 && _wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0) rest = path.substr(8);
    else if (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'?' && path[2] != L'.')
        rest = path.substr(2);
    else return {};
    const size_t server_end = rest.find(L'\\');
    if (server_end == std::wstring::npos || server_end == 0) return {};
    size_t share_end = rest.find(L'\\', server_end + 1);
    if (share_end == std::wstring::npos) share_end = rest.size();
    if (share_end == server_end + 1) return {};
    return L"\\\\" + rest.substr(0, share_end);
}

bool StartNetworkConnect(HWND owner, HWND notify, UINT msg, const std::wstring& path) {
    const std::wstring share = UncShareRoot(path);
    if (share.empty()) return false;
    const std::wstring key = Key(share);
    {
        std::lock_guard lock(g_mutex);
        if (const auto done = g_succeeded.find(key);
            done != g_succeeded.end() && GetTickCount64() - done->second < kQuietAfterSuccessMs)
            return false;
        if (!g_pending.insert(key).second) return false;
    }
    try {
        std::thread(Connect, owner, notify, msg, share).detach();
    } catch (...) {
        std::lock_guard lock(g_mutex);
        g_pending.erase(key);
        return false;
    }
    return true;
}

bool NetworkConnectPending(const std::wstring& path) {
    const std::wstring share = UncShareRoot(path);
    if (share.empty()) return false;
    std::lock_guard lock(g_mutex);
    return g_pending.count(Key(share)) != 0;
}

bool NetworkConnectUnreachable(DWORD error) {
    switch (error) {
    case ERROR_BAD_NETPATH:
    case ERROR_REM_NOT_LIST:
    case ERROR_NETNAME_DELETED:
    case ERROR_SEM_TIMEOUT:
    case ERROR_NO_NET_OR_BAD_PATH:
    case ERROR_NO_NETWORK:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE:
    case ERROR_CONNECTION_REFUSED:
        return true;
    default:
        return false;
    }
}

std::wstring NetworkConnectErrorText(DWORD error) {
    if (error == ERROR_CANCELLED) return {};
    if (NetworkConnectUnreachable(error))
        return l10n::Get(l10n::StringId::CanTReachTheServerCheckTheAddressOverTheInternetISPsOftenBlo).c_str();
    switch (error) {
    case ERROR_BAD_NET_NAME:
        return l10n::Get(l10n::StringId::TheServerHasNoShareWithThisName).c_str();
    case ERROR_ACCESS_DENIED:
    case ERROR_LOGON_FAILURE:
    case ERROR_INVALID_PASSWORD:
    case ERROR_ACCOUNT_RESTRICTION:
        return l10n::Get(l10n::StringId::WrongUserNameOrPasswordOrThisAccountHasNoAccessPressF5ToTryA).c_str();
    case ERROR_SESSION_CREDENTIAL_CONFLICT:
        return l10n::Get(l10n::StringId::ThisServerIsAlreadyConnectedWithAnotherAccountWindowsAllowsO).c_str();
    case 1272:  // ERROR_ACCESS_DENIED_GUEST-style: insecure guest logons are blocked
        return l10n::Get(l10n::StringId::TheServerOnlyAllowsGuestAccessWhichWindowsBlocksByDefaultSet).c_str();
    default: {
        wchar_t code[64]{};
        swprintf_s(code, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), error);
        return code;
    }
    }
}

} // namespace pulse::app
