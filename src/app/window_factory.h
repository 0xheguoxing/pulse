// window_factory.h - Opens further Pulse windows on the shared core
// (multi-window M4a; plan §2, §8).
//
// The first window is created by WinMain and owns the AppCore (its AppState
// lives until the process exits). Every later window gets an AppState bound to
// that core, owned here: WM_CREATE skips the process-level setup for it, and
// after WM_NCDESTROY the state is retired and freed on the host's next message,
// never inside the window's own message handling.
#pragma once
#include <windows.h>

#include <string>

namespace pulse {

struct AppState;

// Style of every Pulse main window (see WinMain for why WS_CAPTION without
// WS_SYSMENU).
constexpr DWORD kMainWindowStyle = WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

struct WindowInit {
    std::wstring path;  // folder of the first tab ("" = This PC)
};

// Creates and shows a window on `from`'s core at `init.path`, next to `from`
// and with its per-window layout (sidebar, details pane). Returns the new
// window, or nullptr when it could not be created.
HWND OpenPulseWindow(AppState& from, const WindowInit& init);

// WM_NCDESTROY: hands the state of a window opened by OpenPulseWindow over for
// release. False for the first window, whose state WinMain owns.
bool RetireWindowState(AppState& s);
// Frees retired states (the host's WM_RELEASE_WINDOW_STATES, and WinMain
// before the core goes away).
void ReleaseRetiredWindowStates();
// WinMain after the message loop: destroys windows still open (an abnormal
// quit) and frees every state before the core goes away.
void ReleaseWindowStatesAtExit();

}  // namespace pulse
