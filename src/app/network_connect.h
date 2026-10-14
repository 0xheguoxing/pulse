// network_connect.h — log on to an SMB share the way File Explorer does.
#pragma once

#include <windows.h>
#include <string>

namespace pulse::app {

struct NetworkConnectResult {
    std::wstring share;  // "\\server\share"
    DWORD error = 0;     // WNetAddConnection3 result; ERROR_CANCELLED: the prompt was closed
};

// "\\server\share" of a UNC path (plain or \\?\UNC\), empty when it has no share.
std::wstring UncShareRoot(const std::wstring& path);

// A share that refused the listing is connected with WNetAddConnection3
// (CONNECT_INTERACTIVE) on a background thread: Windows tries the saved
// credentials first and shows its own credential prompt (with 记住我的凭据)
// only when the server asks for a logon - a share on the internet usually
// does, since only the LAN ones have credentials saved already. The prompt
// is owned by `owner`; msg is posted to `notify` with
// LPARAM = NetworkConnectResult* (the receiver deletes it). Returns false
// when path has no share, a connection to it is running, or one just
// succeeded (the listing that failed after it must not prompt again).
bool StartNetworkConnect(HWND owner, HWND notify, UINT msg, const std::wstring& path);

// Banner text for a failed connection; empty for ERROR_CANCELLED.
std::wstring NetworkConnectErrorText(DWORD error);
// Whether the error means the server could not be reached at all.
bool NetworkConnectUnreachable(DWORD error);

} // namespace pulse::app
