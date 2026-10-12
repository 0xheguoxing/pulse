// shell_window_registry.h — Pulse panes as shell windows (B站 #1 phase 2a).
//
// Each pane showing a folder is registered in IShellWindows as an SWC_BROWSER
// window whose document serves an IShellView. SHOpenFolderAndSelectItems
// ("open file location" in WeChat, WPS, browsers…) then finds the pane by
// folder and calls IShellView::SelectItem, which comes back to the UI thread
// as a ShellSelectRequest. When no pane shows the folder, the shell runs the
// folder's open verb (Pulse, with the takeover on) and waits for a window
// registered as pending for that folder — so a new pane registers at once.
// The shell matches the folder's ID list byte for byte, so a folder below a
// known folder (Desktop, Downloads…) is registered under both of its forms.
//
// Every call into Explorer runs on the registry's own STA thread; the UI
// thread only publishes the wanted set and never blocks on the shell.
#pragma once
#include "shell_window_plan.h"

#include <windows.h>
#include <memory>
#include <vector>

struct IShellWindows;

namespace pulse::app {

// Selftest builds: appends a line to PULSE_TEST_SHELL_WINDOWS_LOG, if set.
void TraceShellWindows(const wchar_t* format, ...);

class ShellWindowRegistry {
public:
    // Selection requests are posted to `window` as `select_message` with a
    // heap-allocated ShellSelectRequest* in lParam (the receiver deletes it).
    using CreateWindows = HRESULT (*)(IShellWindows**);
    ShellWindowRegistry(HWND window, UINT select_message, CreateWindows create = nullptr);
    bool Running() const;
    ~ShellWindowRegistry();
    ShellWindowRegistry(const ShellWindowRegistry&) = delete;
    ShellWindowRegistry& operator=(const ShellWindowRegistry&) = delete;

    // Replaces the wanted set; the thread applies the latest one it sees.
    void Publish(std::vector<ShellWindowEntry> wanted);
    // UI-side liveness check only; failed thread creation is retried with backoff.
    void EnsureRunning();
    // Revokes every registration and ends the thread, waiting at most
    // `timeout_ms` (a hung Explorer must not hold up closing Pulse).
    void Stop(DWORD timeout_ms = 2000);

    struct Shared;

private:
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
    ULONGLONG next_start_ = 0;
    DWORD start_retry_ms_ = 250;
};

} // namespace pulse::app
