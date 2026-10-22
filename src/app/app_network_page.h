// app_network_page.h - The network page beyond the pins (pulse_network_design.html):
// 连接服务器 in the New button's slot and the address bar, 最近访问, and the
// 局域网设备 found by lan_discovery. Window-side; the data is shared through
// AppCore::networkPage so every window sees the same lists.
#pragma once
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace app { struct Tab; }
namespace fs { struct DirEntry; }
struct NetworkPageLoad;

// The network page and a server's share list put 连接服务器 where 新建 is.
bool IsConnectServerPage(const std::wstring& path);
// 连接服务器: the address bar, prefilled with "\\" and offering known servers.
void BeginConnectServer(AppState& s);
// The button's ▾: 连接服务器… / 映射网络驱动器… / 添加 Windows 网络位置… /
// 重新发现局域网设备 / 网络和共享设置.
void ShowConnectServerMenu(AppState& s);
// Commands from the network page's menus (CmdConnectServer ...). False when
// `cmd` is not one of them.
bool HandleNetworkPageCommand(AppState& s, int cmd);

// Address bar text that names a server: smb:// (and other schemes, which get
// an explanation), a bare IP, or anything typed after 连接服务器. True when
// handled; false leaves the text to ordinary navigation.
bool OpenServerAddressText(AppState& s, const std::wstring& text);
// Server suggestions under the address bar for "\\..." (and in connect mode):
// pins, recent servers, LAN devices from the cache, mapped drives. Never
// touches the network. True when it owns the suggestion list.
bool FillServerSuggestions(AppState& s, const std::wstring& text);

// A UNC listing opened in a window: remember its share root (最近访问).
void RecordNetworkVisit(AppState& s, const std::wstring& path);
bool SelectionHasNetworkRecent(const app::Tab& tab);
bool ForgetSelectedNetworkRecent(AppState& s);

// The network page's rows after the pins and drives: LAN devices, then recent
// visits that are not already listed above.
void AppendNetworkPageEntries(AppState& s, std::vector<fs::DirEntry>& entries);
// Status bar summary for the network page (counts, discovery state).
std::wstring NetworkPageStatus(AppState& s, const std::vector<fs::DirEntry>& entries);

// Looks for LAN devices unless a recent answer for this network exists
// (10 min); `force` (F5, 重新发现) always looks. Nothing runs on Public networks.
void RequestLanDiscovery(AppState& s, bool force);
void ApplyLanDiscovery(AppState& s);
void CancelLanDiscovery(AppState& s);
// Rebuilds every window's network page from the current data.
void RefreshNetworkPages(AppState& s);
} // namespace pulse
