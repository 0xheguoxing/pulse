#pragma once
#include <windows.h>

namespace pulse {
struct AppState;
struct SidebarRefreshLoad;
void RequestSidebarRefresh(AppState& state, bool rebuild = false);
bool TickSidebarRefresh(AppState& state, ULONGLONG now);
void CancelSidebarRefresh(AppState& state);
// Phone rows (DeviceRegistry) into state.sidebar.devices; true when changed.
bool SyncDeviceSidebar(AppState& state);
} // namespace pulse
