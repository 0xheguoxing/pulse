#pragma once
// window_identity_ui.h - Multi-window M7c (plan section 8.3): a window's name
// and color on screen. With two or more windows the title bar shows a color
// dot and the name (double-click or right-click edits both), the window title
// reads "name - Pulse" and the taskbar button carries a dot in the window's
// color. With one window none of it shows. Rules: window_identity.h.
#include <windows.h>

#include <string>

namespace pulse {
struct AppState;
namespace ui {
struct HitTestResult;
}

// The typed name, else the folder its panes share (app::AutoNameFolder),
// else its active tab's title.
std::wstring WindowDisplayName(const AppState& s);
// True while two or more windows are open.
bool WindowIdentityShown(const AppState& s);

// Gives every window that has none a color (the least used), then brings
// each window's title, taskbar dot and title bar up to date. Call when a
// window opens or closes and after a rename or recolor.
void SyncWindowIdentities(AppState& any);
// The taskbar dot of one window; also on TaskbarButtonCreatedMessage().
void SyncWindowOverlay(AppState& s);
// WM_DESTROY: frees the dot's icon.
void ReleaseWindowOverlay(AppState& s);
UINT TaskbarButtonCreatedMessage();

// The name in the title bar. A press that drags moves the window like the
// rest of the title bar; a double-click opens the editor. False when `hit`
// is not the name.
bool HandleWindowIdentityPress(AppState& s, const ui::HitTestResult& hit, POINT client);
bool HandleWindowIdentityDoubleClick(AppState& s, const ui::HitTestResult& hit, POINT client);

// The editor: a name field (empty: automatic again), the six colors and
// "use the automatic name". Opens at `screen`.
void ShowWindowIdentityMenu(AppState& s, POINT screen);
}  // namespace pulse
