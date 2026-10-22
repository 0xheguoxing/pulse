// app_network_page.cpp - see app_network_page.h.
#include "app_network_page.h"

#include "app_internal.h"
#include "app_network_locations.h"
#include "context_menu.h"
#include "lan_discovery.h"
#include "network_locations.h"
#include "network_recent.h"
#include "selection_caps.h"
#include "server_address.h"
#include "session.h"
#include "window_registry.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"
#include "../ui/fluent_menu.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>

namespace pulse {

namespace {
constexpr ULONGLONG kLanCacheMs = 10ull * 60 * 1000;  // one answer per network for 10 minutes
constexpr DWORD kLanBudgetMs = 2500;

struct LanScan {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    app::LanDiscoveryResult result;
    bool reused = false;  // same network as the cached answer: nothing was sent
    bool ready = false;
};
}  // namespace

struct NetworkPageLoad {
    app::NetworkRecentList recent;
    bool recent_loaded = false;
    std::shared_ptr<LanScan> scan;  // in flight (UI thread only)
    std::vector<app::LanDevice> devices;
    std::wstring network_key;
    ULONGLONG scanned_at = 0;
    bool scanned = false;
    bool public_network = false;
    bool offline = false;
};

namespace {
NetworkPageLoad& Page(AppState& s) {
    if (!s.networkPage) s.networkPage = std::make_shared<NetworkPageLoad>();
    return *s.networkPage;
}

std::wstring RecentFile() {
    const std::wstring dir = app::GetPulseDataDir();
    return dir.empty() ? std::wstring() : dir + L"\\network_recent.json";
}

app::NetworkRecentList& Recent(AppState& s) {
    NetworkPageLoad& page = Page(s);
    if (!page.recent_loaded) {
        page.recent_loaded = true;
        std::wstring text;
        // A few hundred bytes in the local data folder, read once.
        if (const std::wstring file = RecentFile(); !file.empty() && ReadUtf8File(file, text))
            page.recent.FromJson(text);
    }
    return page.recent;
}

void SaveRecent(AppState& s) {
    const std::wstring file = RecentFile();
    if (file.empty()) return;
    s.worker.EnqueueIo([file, json = Recent(s).ToJson()] { WriteUtf8FileAtomic(file, json); });
}

uint64_t NowTicks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring Leaf(const std::wstring& unc) {
    std::wstring path = unc;
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    const size_t cut = path.find_last_of(L'\\');
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

std::wstring Lower(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

bool Listed(const std::vector<fs::DirEntry>& entries, const std::wstring& path) {
    return std::any_of(entries.begin(), entries.end(), [&](const fs::DirEntry& e) {
        return app::SameNetworkLocation(e.full_path, path);
    });
}

std::wstring DeviceTypeText(const app::LanDevice& device) {
    std::wstring text = device.model.empty() ? l10n::Get(l10n::StringId::LanDeviceGeneric) : device.model;
    const std::wstring address = !device.ip.empty() ? device.ip : device.host;
    if (!address.empty()) text += L" \u00B7 " + address;
    std::wstring via;
    if (device.sources & app::kLanSourceMdns) via = L"mDNS";
    if (device.sources & app::kLanSourceWsd) via += via.empty() ? L"WSD" : L" + WSD";
    if (!via.empty()) text += L" \u00B7 " + via;
    return text;
}

void RunCommandLine(AppState& s, const wchar_t* exe, const wchar_t* args) {
    wchar_t system[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(system, ARRAYSIZE(system));
    if (!n || n >= ARRAYSIZE(system)) return;
    s.ops.OpenProgramIn(std::wstring(system) + L"\\" + exe, std::wstring(), args);
}

void ExplainAddress(AppState& s, const app::ServerAddress& address) {
    std::wstring message;
    if (address.kind == app::ServerAddressKind::Unsupported || address.kind == app::ServerAddressKind::WebDav) {
        wchar_t text[512]{};
        swprintf_s(text, l10n::Get(l10n::StringId::ServerAddressUnsupportedFormat).c_str(),
                   address.scheme.empty() ? L"?" : address.scheme.c_str());
        message = text;
    } else {
        message = l10n::Get(l10n::StringId::ServerAddressInvalid);
    }
    s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::ConnectServer), std::move(message));
}

bool AnyWindowShowsNetworkPage(AppState& s) {
    bool shown = false;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        ForEachPane(window, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab(); tab && tab->current_path == kNetworkLocationsPath) shown = true;
        });
    });
    return shown;
}
}  // namespace

bool IsConnectServerPage(const std::wstring& path) {
    return path == kNetworkLocationsPath || app::IsUncServerRoot(path);
}

void BeginConnectServer(AppState& s) {
    ShowAddressEditor(s);
    if (!s.addressEditing || !s.hwndAddressEdit) return;
    s.addressConnectMode = true;
    SetWindowTextW(s.hwndAddressEdit, L"\\\\");
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 2, 2);
    UpdateAddressSuggestions(s);
}

void ShowConnectServerMenu(AppState& s) {
    if (!EnsureMenu(s)) return;
    const auto anchor = s.renderer.NewCommandRect(static_cast<float>(s.compositor.Width()));
    POINT pt{static_cast<LONG>(anchor.left), static_cast<LONG>(anchor.bottom)};
    ClientToScreen(s.hwnd, &pt);
    const auto row = [](int command, l10n::StringId text, const wchar_t* glyph) {
        ui::FluentMenuItem item;
        item.command = command;
        item.text = l10n::Get(text);
        item.glyph = glyph;
        return item;
    };
    std::vector<ui::FluentMenuItem> items;
    items.push_back(row(app::CmdConnectServer, l10n::StringId::ConnectServerMenu, L"\xE968"));
    items.push_back(row(app::CmdMapNetworkDrive, l10n::StringId::MapNetworkDrive, L"\xE8CE"));
    items.push_back(row(app::CmdAddNetworkPlace, l10n::StringId::NetworkAddWindowsPlace, L"\xE710"));
    items.back().separator_after = true;
    items.push_back(row(app::CmdLanRescan, l10n::StringId::NetworkRescan, L"\xE72C"));
    items.push_back(row(app::CmdNetworkSharingSettings, l10n::StringId::NetworkSharingSettings, L"\xE713"));
    {
        POINT tl{static_cast<LONG>(anchor.left), static_cast<LONG>(anchor.top)};
        POINT br{static_cast<LONG>(anchor.right), static_cast<LONG>(anchor.bottom)};
        ClientToScreen(s.hwnd, &tl);
        ClientToScreen(s.hwnd, &br);
        s.menu->SetMorphFrom(RECT{tl.x, tl.y, br.x, br.y});
    }
    const int cmd = s.menu->TrackPopup(pt, std::move(items));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

bool HandleNetworkPageCommand(AppState& s, int cmd) {
    switch (cmd) {
    case app::CmdConnectServer: BeginConnectServer(s); return true;
    case app::CmdForgetNetworkRecent: ForgetSelectedNetworkRecent(s); return true;
    case app::CmdLanRescan: RequestLanDiscovery(s, true); return true;
    // The Windows wizard creates a NetHood shortcut; the page re-reads them.
    case app::CmdAddNetworkPlace: RunCommandLine(s, L"rundll32.exe", L"shwebsvc.dll,AddNetPlaceRunDll"); return true;
    case app::CmdNetworkSharingSettings:
        RunCommandLine(s, L"control.exe", L"/name Microsoft.NetworkAndSharingCenter /page Advanced");
        return true;
    default: return false;
    }
}

bool OpenServerAddressText(AppState& s, const std::wstring& text) {
    const bool connect = s.addressConnectMode;
    const bool url = app::IsServerUrlText(text) || text.find(L"://") != std::wstring::npos;
    if (!connect && !app::IsServerUrlText(text)) return false;
    const app::ServerAddress address = app::ParseServerAddress(text, connect);
    switch (address.kind) {
    case app::ServerAddressKind::Server:
    case app::ServerAddressKind::Share:
        NavigateTo(s, address.unc);
        return true;
    case app::ServerAddressKind::WebDav:
    case app::ServerAddressKind::Unsupported:
        ExplainAddress(s, address);
        return true;
    default:
        if (!url) return false;  // connect mode, but a local path: just go there
        ExplainAddress(s, address);
        return true;
    }
}

bool FillServerSuggestions(AppState& s, const std::wstring& raw) {
    std::wstring text = raw;
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    const bool unc = text.size() >= 2 && text[0] == L'\\' && text[1] == L'\\';
    if (!unc && !s.addressConnectMode) return false;
    if (!unc && (text.find_first_of(L":%") != std::wstring::npos)) return false;
    std::wstring needle = text;
    while (!needle.empty() && (needle.front() == L'\\' || needle.front() == L'/')) needle.erase(needle.begin());
    for (auto& c : needle) if (c == L'/') c = L'\\';
    needle = Lower(needle);

    struct Candidate { std::wstring label, path, hint; };
    std::vector<Candidate> candidates;
    const auto add = [&](const std::wstring& path, const std::wstring& name, const std::wstring& hint) {
        if (path.empty()) return;
        for (const auto& c : candidates) if (app::SameNetworkLocation(c.path, path)) return;
        const std::wstring bare = Lower(path.size() > 2 ? path.substr(2) : path);
        if (!needle.empty() && bare.rfind(needle, 0) != 0 && Lower(name).find(needle) == std::wstring::npos) return;
        candidates.push_back({path, path, name.empty() || name == Leaf(path) ? hint : name + L" \u00B7 " + hint});
    };
    for (const auto& pin : s.places.networks)
        add(app::NetworkLocationKey(pin.unc), pin.name, l10n::Get(l10n::StringId::GroupNetworkPinned));
    NetworkPageLoad& page = Page(s);
    for (const auto& device : page.devices)
        add(app::LanDeviceUnc(device), device.name, l10n::Get(l10n::StringId::GroupNetworkDevices));
    for (const auto& item : Recent(s).Items())
        add(item.path, std::wstring(), l10n::Get(l10n::StringId::GroupNetworkRecent));
    if (candidates.empty()) return unc;  // a typed UNC path: no alias rows either
    s.addressSuggestHeader = l10n::Get(l10n::StringId::ConnectServerHint);
    for (auto& c : candidates) {
        if (s.addressSuggest.size() >= 8) break;
        s.addressSuggest.push_back({std::move(c.label), std::move(c.path), std::move(c.hint), false});
    }
    s.addressSuggestSel = -1;  // Enter opens what was typed; arrows pick a row
    return true;
}

void RecordNetworkVisit(AppState& s, const std::wstring& path) {
    if (!Recent(s).Touch(path, NowTicks())) return;
    SaveRecent(s);
}

bool SelectionHasNetworkRecent(const app::Tab& tab) {
    if (tab.current_path != kNetworkLocationsPath || !tab.snapshot) return false;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
        if (tab.EntryAt(static_cast<size_t>(index)).net_group == 4) return true;
    }
    return false;
}

bool ForgetSelectedNetworkRecent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !SelectionHasNetworkRecent(*tab)) return false;
    bool changed = false;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab->EntryCount()) continue;
        const fs::DirEntry e = tab->EntryAt(static_cast<size_t>(index));
        if (e.net_group == 4) changed = Recent(s).Remove(e.full_path) || changed;
    }
    if (!changed) return false;
    SaveRecent(s);
    RefreshNetworkPages(s);
    return true;
}

void AppendNetworkPageEntries(AppState& s, std::vector<fs::DirEntry>& entries) {
    NetworkPageLoad& page = Page(s);
    for (const auto& device : page.devices) {
        const std::wstring unc = app::LanDeviceUnc(device);
        if (unc.empty() || Listed(entries, unc)) continue;
        fs::DirEntry entry;
        entry.name = device.name.empty() ? unc.substr(2) : device.name;
        entry.full_path = unc;
        entry.is_dir = true;
        entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
        entry.net_group = 3;
        entry.change_type_text = DeviceTypeText(device);
        entries.push_back(std::move(entry));
    }
    for (const auto& item : Recent(s).Items()) {
        if (Listed(entries, item.path)) continue;
        fs::DirEntry entry;
        entry.name = Leaf(item.path);
        entry.full_path = item.path;
        entry.is_dir = true;
        entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
        entry.mtime.dwLowDateTime = static_cast<DWORD>(item.last_visit);
        entry.mtime.dwHighDateTime = static_cast<DWORD>(item.last_visit >> 32);
        entry.net_group = 4;
        entry.change_type_text = item.path;
        entries.push_back(std::move(entry));
    }
}

std::wstring NetworkPageStatus(AppState& s, const std::vector<fs::DirEntry>& entries) {
    int counts[5]{};
    for (const auto& e : entries) if (e.net_group < 5) ++counts[e.net_group];
    std::wstring text;
    const auto part = [&](l10n::StringId format, int count) {
        if (count <= 0) return;
        wchar_t buffer[128]{};
        swprintf_s(buffer, l10n::Get(format).c_str(), count);
        if (!text.empty()) text += L" \u00B7 ";
        text += buffer;
    };
    part(l10n::StringId::NetworkStatusPinnedFormat, counts[1]);
    part(l10n::StringId::NetworkStatusDrivesFormat, counts[2]);
    part(l10n::StringId::NetworkStatusRecentFormat, counts[4]);
    const NetworkPageLoad& page = Page(s);
    std::wstring lan;
    if (page.scan) lan = l10n::Get(l10n::StringId::LanDiscoveryScanning);
    else if (page.offline) lan = l10n::Get(l10n::StringId::LanDiscoveryOffline);
    else if (page.public_network) lan = l10n::Get(l10n::StringId::LanDiscoveryPublic);
    else if (page.scanned && page.devices.empty()) lan = l10n::Get(l10n::StringId::LanDiscoveryNone);
    else if (page.scanned) {
        wchar_t buffer[128]{};
        swprintf_s(buffer, l10n::Get(l10n::StringId::LanDiscoveryFoundFormat).c_str(), static_cast<int>(page.devices.size()));
        lan = buffer;
    }
    if (!lan.empty()) text += (text.empty() ? L"" : L" \u00B7 ") + lan;
    return text;
}

void RequestLanDiscovery(AppState& s, bool force) {
    NetworkPageLoad& page = Page(s);
    if (page.scan) return;  // one look at a time
    const bool fresh = !force && page.scanned && GetTickCount64() - page.scanned_at < kLanCacheMs;
    auto scan = std::make_shared<LanScan>();
    page.scan = scan;
    const HWND hwnd = ServiceWindow(s);
    const std::wstring cached_key = fresh ? page.network_key : std::wstring();
    const bool isolated = s.isolatedTest;  // tests never send multicast
    s.worker.EnqueueIo([scan, hwnd, cached_key, fresh, isolated] {
        app::LanDiscoveryResult result;
        bool reused = false;
        if (isolated) {
            result.offline = true;
        } else if (fresh) {
            app::ReadLanNetworkIdentity(result);
            reused = !result.network_key.empty() && result.network_key == cached_key;
        }
        if (!isolated && !reused && !scan->cancelled)
            result = app::RunLanDiscovery([scan] { return scan->cancelled.load(); }, kLanBudgetMs);
        if (scan->cancelled) return;
        {
            std::lock_guard lock(scan->mutex);
            scan->result = std::move(result);
            scan->reused = reused;
            scan->ready = true;
        }
        if (hwnd) PostMessageW(hwnd, WM_LAN_DISCOVERY, 0, 0);
    });
    RefreshNetworkPages(s);  // 正在发现局域网设备…
}

void ApplyLanDiscovery(AppState& s) {
    if (!s.networkPage || !s.networkPage->scan) return;
    NetworkPageLoad& page = *s.networkPage;
    const auto scan = page.scan;
    {
        std::lock_guard lock(scan->mutex);
        if (!scan->ready) return;
        page.scan.reset();
        if (scan->result.cancelled) return;
        if (!scan->reused) {
            page.devices = std::move(scan->result.devices);
            page.network_key = scan->result.network_key;
            page.public_network = scan->result.public_network;
            page.offline = scan->result.offline;
            page.scanned_at = GetTickCount64();
            page.scanned = true;
        }
    }
    RefreshNetworkPages(s);
}

void CancelLanDiscovery(AppState& s) {
    if (s.networkPage && s.networkPage->scan) s.networkPage->scan->cancelled = true;
}

void RefreshNetworkPages(AppState& s) {
    if (!AnyWindowShowsNetworkPage(s)) return;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        bool any = false;
        ForEachPane(window, [&](app::Pane& pane) {
            if (auto* tab = pane.ActiveTab(); tab && tab->current_path == kNetworkLocationsPath) {
                FillNetworkLocationsView(window, *tab);
                any = true;
            }
        });
        if (any) InvalidateRect(window.hwnd, nullptr, FALSE);
    });
}

} // namespace pulse
