// host_routing.h - Which messages the hidden host window hands to a Pulse
// window (multi-window M2; plan §2.3).
//
// Services and background threads post process-level notifications to the
// host (ServiceWindow). Until the data/window split of M3, the host forwards
// each one unchanged to the main window with SendMessage, so the existing
// handlers run as before. Window-level messages (frame pump, quick preview,
// details probe, menus, toasts) still go to their window directly. System
// broadcasts are not listed: every top-level window already receives them,
// and forwarding would handle them twice; the host handles TaskbarCreated
// itself because it owns the notification icon.
#pragma once
#include "app_messages.h"
#include "tray_controller.h"

namespace pulse::app {

inline constexpr UINT kHostRoutedMessages[] = {
    WM_WORKER_RESULT,          // PostWorkerResult
    WM_OPS_NOTIFY,             // OpsManager completion
    WM_INDEX_NOTIFY,           // IndexClient status
    WM_INDEX_SEARCH,           // IndexClient search results
    WM_NETWORK_INDEX_NOTIFY,   // NetworkAgentClient status
    WM_NETWORK_INDEX_SEARCH,   // NetworkAgentClient search results
    WM_CONTENT_SEARCH,         // content search agent
    WM_DUPLICATE_SCAN,         // duplicate scan agent
    WM_ASSOC_CHANGED,          // SHCNE_ASSOCCHANGED registration
    WM_SHELL_CACHE_INVALIDATE, // shell registry watch
    WM_SHELL_VERB_SEED,        // machine verb cache seed
    WM_SHELL_VERBS,            // static verb reads (process-wide cache)
    TrayController::kCallbackMessage,  // notification icon (owned by the host)
    WM_HOTKEY,                 // global search hotkey (registered on the host)
};

constexpr bool IsHostRoutedMessage(UINT message) noexcept {
    for (const UINT routed : kHostRoutedMessages)
        if (routed == message) return true;
    return false;
}

}  // namespace pulse::app
