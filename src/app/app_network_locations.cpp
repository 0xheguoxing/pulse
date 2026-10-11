#include "app_network_locations.h"
#include "app_internal.h"
#include "network_locations.h"
#include "../common/localization.h"
#include <algorithm>

namespace pulse {
struct NetworkLocationLoad {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    app::NetworkLocationScan result;
    bool ready = false;
    bool pending = false; // UI thread only
    DWORD error = 0; // UI thread only
    std::vector<app::NetworkLocation> items; // UI thread only
};

namespace {
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
    state.worker.EnqueueIo([load, hwnd, fixture = std::move(fixture)] {
        auto result = app::ReadNetworkLocations(fixture, [load] { return load->cancelled.load(); });
        if (load->cancelled) return;
        {
            std::lock_guard lock(load->mutex);
            load->result = std::move(result);
            load->ready = true;
        }
        // The shared result owns its lifetime, including window shutdown.
        if (hwnd) PostMessageW(hwnd, WM_NETWORK_LOCATIONS, 0, 0);
    });
}

void FillNetworkLocationsView(AppState& state, app::Tab& tab) {
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto& location : Locations(state)) {
        fs::DirEntry entry;
        entry.name = location.name;
        entry.full_path = location.path;
        entry.is_dir = !location.shell_link;
        entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entries->push_back(std::move(entry));
    }
    std::sort(entries->begin(), entries->end(), [&](const auto& a, const auto& b) {
        return app::EntryLess(a, b, tab.sort_column, tab.sort_direction);
    });
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
        tab.banner_title = l10n::Pick(L"系统网络位置读取失败", L"Could not read system network locations");
        wchar_t code[64]{};
        swprintf_s(code, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(), state.networkLocations->error);
        tab.banner_message = code;
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
        if (!load->error) load->items = app::MergeNetworkLocations({}, load->result.items);
    }
    state.sidebar.system_networks.clear();
    for (const auto& item : load->items) {
        app::SidebarEntry entry;
        entry.label = item.name;
        entry.path = item.path;
        entry.detail = item.shell_link
            ? l10n::Pick(L"由系统打开", L"Opens in Windows") : item.path;
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
