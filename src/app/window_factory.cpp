#include "window_factory.h"

#include "app_messages.h"
#include "app_state.h"
#include "single_instance_coordinator.h"
#include "window_placement.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace pulse {
namespace {

// UI thread only.
std::vector<std::unique_ptr<AppState>> g_windows;
std::vector<std::unique_ptr<AppState>> g_retired;

// The layout a new window starts with: the opener's sidebar and details pane.
// Process-wide preferences live in the shared core already.
void InheritWindowLayout(const AppState& from, AppState& to) {
    to.sidebarCollapsedMask = from.sidebarCollapsedMask;
    to.sidebarHiddenMask = from.sidebarHiddenMask;
    to.sidebarOrder = from.sidebarOrder;
    to.sidebarQuickAccessHiddenMask = from.sidebarQuickAccessHiddenMask;
    to.starredExpanded = from.starredExpanded;
    to.showDetailsPanel = from.showDetailsPanel;
    to.detailsPanelWidth = from.detailsPanelWidth;
    to.detailsPreviewOnly = from.detailsPreviewOnly;
    to.detailsPreviewEnabled = from.detailsPreviewEnabled;
    to.detailsPreviewExpansion = from.detailsPreviewExpansion;
    to.showFps = from.showFps;
    to.forceStatusPerformance = from.forceStatusPerformance;
}

// Normal-state screen rectangle of `from`, cascaded by 32 DIP within its
// monitor's work area (CascadeWindowRect). A maximized opener does not pass
// its maximized state on.
RECT CascadedRect(const AppState& from) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(from.hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return RECT{};
    RECT normal{};
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (IsIconic(from.hwnd) || IsZoomed(from.hwnd)) {
        if (!GetWindowPlacement(from.hwnd, &placement)) return RECT{};
        normal = app::WorkspaceToScreenRect(placement.rcNormalPosition, monitor.rcMonitor, monitor.rcWork);
    } else if (!GetWindowRect(from.hwnd, &normal)) {
        return RECT{};
    }
    if (normal.right <= normal.left || normal.bottom <= normal.top) return RECT{};
    const int offset = static_cast<int>(std::lround(32.0f * (from.scale > 0.0f ? from.scale : 1.0f)));
    return app::CascadeWindowRect(normal, monitor.rcWork, offset);
}

}  // namespace

HWND OpenPulseWindow(AppState& from, const WindowInit& init) {
    ReleaseRetiredWindowStates();
    auto owned = std::make_unique<AppState>(from.core);
    AppState* const state = owned.get();
    InheritWindowLayout(from, *state);
    state->has_window_start = true;
    state->window_start_path = init.path;
    g_windows.push_back(std::move(owned));

    const RECT rect = CascadedRect(from);
    const bool placed = rect.right > rect.left;
    const HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP, app::SingleInstanceCoordinator::WindowClassName(), L"Pulse", kMainWindowStyle,
        placed ? rect.left : CW_USEDEFAULT, placed ? rect.top : CW_USEDEFAULT,
        placed ? rect.right - rect.left : CW_USEDEFAULT, placed ? rect.bottom - rect.top : CW_USEDEFAULT,
        nullptr, nullptr, GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        // A failed WM_CREATE still destroys the window, which retires the
        // state; otherwise it was never handed to a window at all.
        const auto it = std::find_if(g_windows.begin(), g_windows.end(),
                                     [state](const std::unique_ptr<AppState>& w) { return w.get() == state; });
        if (it != g_windows.end()) g_windows.erase(it);
        return nullptr;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    return hwnd;
}

bool RetireWindowState(AppState& s) {
    const auto it = std::find_if(g_windows.begin(), g_windows.end(),
                                 [&s](const std::unique_ptr<AppState>& w) { return w.get() == &s; });
    if (it == g_windows.end()) return false;
    g_retired.push_back(std::move(*it));
    g_windows.erase(it);
    if (const HWND host = s.core.host.Hwnd()) PostMessageW(host, WM_RELEASE_WINDOW_STATES, 0, 0);
    return true;
}

void ReleaseRetiredWindowStates() {
    // Moved out first: an AppState destructor must not see the list mid-change.
    std::vector<std::unique_ptr<AppState>> retired;
    retired.swap(g_retired);
}

void ReleaseWindowStatesAtExit() {
    std::vector<HWND> open;
    for (const auto& w : g_windows)
        if (w->hwnd && IsWindow(w->hwnd)) open.push_back(w->hwnd);
    for (HWND hwnd : open) DestroyWindow(hwnd);
    ReleaseRetiredWindowStates();
    g_windows.clear();
}

}  // namespace pulse
