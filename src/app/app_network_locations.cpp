#include "app_network_locations.h"
#include "app_internal.h"
#include "app_network_page.h"
#include "entry_group.h"
#include "network_drives.h"
#include "network_locations.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse {
struct NetworkLocationLoad {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    app::NetworkLocationScan result;
    std::vector<app::MappedNetworkDrive> drives_result;
    bool ready = false;
    bool pending = false; // UI thread only
    DWORD error = 0; // UI thread only
    app::NetworkDiscoveryState discovery = app::NetworkDiscoveryState::Unknown; // UI thread only
    std::vector<app::NetworkLocation> items; // UI thread only
    std::vector<app::MappedNetworkDrive> drives; // UI thread only
};

namespace {
std::wstring LeafName(const std::wstring& unc) {
    std::wstring path = unc;
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    const size_t cut = path.find_last_of(L'\\');
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

// "\\nas\share · 在线 · 3 ms" for a Pulse pin whose probe answered.
std::wstring PinTypeText(const AppState& state, const app::NetworkLocation& location) {
    if (location.shell_link) return l10n::Get(l10n::StringId::OpensInWindows);
    std::wstring text = app::NetworkLocationKey(location.path);
    const int index = state.places.FindNetwork(location.path);
    if (index < 0) return text;
    const auto& pin = state.places.networks[static_cast<size_t>(index)];
    wchar_t status[96]{};
    if (pin.status == fs::NetStatus::Online)
        swprintf_s(status, l10n::Get(l10n::StringId::NetworkOnlineMs).c_str(), static_cast<unsigned>(pin.rtt_ms));
    else if (pin.status == fs::NetStatus::Slow)
        swprintf_s(status, l10n::Get(l10n::StringId::NetworkSlowMs).c_str(), static_cast<unsigned>(pin.rtt_ms));
    else if (pin.status == fs::NetStatus::Offline)
        wcscpy_s(status, l10n::Get(l10n::StringId::Offline).c_str());
    if (status[0]) text += std::wstring(L" \u00B7 ") + status;
    return text;
}

std::vector<app::NetworkLocation> Locations(const AppState& state) {
    std::vector<app::NetworkLocation> pins;
    for (const auto& pin : state.places.networks)
        pins.push_back({pin.name.empty() ? pin.unc : pin.name, pin.unc, false});
    return app::MergeNetworkLocations(pins, state.networkLocations
        ? state.networkLocations->items : std::vector<app::NetworkLocation>{});
}
}

void RequestNetworkLocations(AppState& state) {
    if (!state.networkLocations) state.networkLocations = std::make_shared<NetworkLocationLoad>();
    const auto load = state.networkLocations;
    if (load->pending || load->cancelled) return;
    load->pending = true;
    const HWND hwnd = ServiceWindow(state);
    std::wstring fixture;
    if (state.isolatedTest) {
        wchar_t path[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_NETWORK_LOCATIONS", path, ARRAYSIZE(path));
        if (length && length < ARRAYSIZE(path)) fixture.assign(path, length);
    }
    const bool isolated = state.isolatedTest;
    state.worker.EnqueueIo([load, hwnd, fixture = std::move(fixture), isolated] {
        auto result = app::ReadNetworkLocations(fixture, [load] { return load->cancelled.load(); });
        if (load->cancelled) return;
        // Mapped drives (网络驱动器) only from the real system, never in tests.
        auto drives = isolated ? std::vector<app::MappedNetworkDrive>{} : app::ReadMappedNetworkDrives();
        if (load->cancelled) return;
        {
            std::lock_guard lock(load->mutex);
            load->result = std::move(result);
            load->drives_result = std::move(drives);
            load->ready = true;
        }
        // The shared result owns its lifetime, including window shutdown.
        if (hwnd) PostMessageW(hwnd, WM_NETWORK_LOCATIONS, 0, 0);
    });
}

void FillNetworkLocationsView(AppState& state, app::Tab& tab) {
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    // 已固定: Pulse pins and Windows network locations (NetHood).
    for (const auto& location : Locations(state)) {
        fs::DirEntry entry;
        entry.name = location.name;
        entry.full_path = location.path;
        entry.is_dir = !location.shell_link;
        entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entry.net_group = 1;
        entry.change_type_text = PinTypeText(state, location);
        entries->push_back(std::move(entry));
    }
    // 网络驱动器: mapped drives, also the remembered ones that are not connected.
    if (state.networkLocations) {
        for (const auto& drive : state.networkLocations->drives) {
            fs::DirEntry entry;
            const wchar_t letter[3] = {drive.letter, L':', L'\0'};
            entry.name = LeafName(drive.remote) + L" (" + letter + L")";
            entry.full_path = std::wstring(letter) + L"\\";
            entry.is_dir = true;
            entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
            entry.drive_type = DRIVE_REMOTE;
            entry.drive_remote_like = true;
            entry.net_group = 2;
            entry.change_type_text = drive.remote;
            if (!drive.connected)
                entry.change_type_text += L" \u00B7 " + l10n::Get(l10n::StringId::NetworkDriveDisconnected);
            entries->push_back(std::move(entry));
        }
    }
    // 局域网设备 and 最近访问 (app_network_page.cpp).
    AppendNetworkPageEntries(state, *entries);
    {
        const app::ScopedEntryGrouping grouping(tab.EffectiveGroup(), tab.current_path);
        std::stable_sort(entries->begin(), entries->end(), [&](const auto& a, const auto& b) {
            return app::EntryLess(a, b, tab.sort_column, tab.sort_direction);
        });
    }
    tab.virtual_status = NetworkPageStatus(state, *entries);
    std::vector<std::wstring> selected;
    std::wstring focus;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
        selected.push_back(tab.EntryAt(static_cast<size_t>(index)).full_path);
        if (index == tab.selected_index) focus = selected.back();
    }
    tab.virtual_title = l10n::Get(l10n::StringId::SidebarNetworkLocations);
    tab.SetSnapshot(std::move(entries));
    tab.ClearSelection();
    int focus_index = -1;
    for (size_t i = 0; i < tab.EntryCount(); ++i) {
        const auto& path = tab.EntryAt(i).full_path;
        if (std::find(selected.begin(), selected.end(), path) == selected.end()) continue;
        tab.ToggleSelect(static_cast<int>(i));
        if (path == focus) focus_index = static_cast<int>(i);
    }
    if (focus_index >= 0) tab.selected_index = tab.selection_anchor = focus_index;
    tab.loading = state.networkLocations && state.networkLocations->pending;
    tab.banner_title.clear();
    tab.banner_message.clear();
    if (state.networkLocations && state.networkLocations->error) {
        tab.banner_title = l10n::Get(l10n::StringId::CouldNotReadSystemNetworkLocations).c_str();
        wchar_t code[64]{};
        swprintf_s(code, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), state.networkLocations->error);
        tab.banner_message = code;
    } else if (state.networkLocations &&
        state.networkLocations->discovery == app::NetworkDiscoveryState::Disabled) {
        tab.banner_title = l10n::Get(l10n::StringId::NetworkDiscoveryOffTitle);
        tab.banner_message = l10n::Get(l10n::StringId::NetworkDiscoveryOffDesc);
    }
}

void ApplyNetworkLocations(AppState& state) {
    const auto load = state.networkLocations;
    if (!load || load->cancelled) return;
    {
        std::lock_guard lock(load->mutex);
        if (!load->ready) return;
        load->ready = false;
        load->pending = false;
        if (load->result.cancelled) return;
        load->error = load->result.error;
        load->discovery = load->result.discovery;
        if (!load->error) load->items = app::MergeNetworkLocations({}, load->result.items);
        load->drives = std::move(load->drives_result);
    }
    state.sidebar.system_networks.clear();
    for (const auto& item : load->items) {
        app::SidebarEntry entry;
        entry.label = item.name;
        entry.path = item.path;
        entry.detail = item.shell_link
            ? l10n::Get(l10n::StringId::OpensInWindows).c_str() : item.path;
        state.sidebar.system_networks.push_back(std::move(entry));
    }
    ForEachPane(state, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab(); tab && tab->current_path == kNetworkLocationsPath)
            FillNetworkLocationsView(state, *tab);
    });
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

void CancelNetworkLocations(AppState& state) {
    if (state.networkLocations) state.networkLocations->cancelled = true;
}

bool OpenSystemNetworkShortcut(AppState& state, const std::wstring& path) {
    if (!state.networkLocations) return false;
    for (const auto& location : state.networkLocations->items) {
        if (location.shell_link && _wcsicmp(location.path.c_str(), path.c_str()) == 0) {
            state.ops.OpenWith(location.path);
            return true;
        }
    }
    return false;
}
} // namespace pulse
