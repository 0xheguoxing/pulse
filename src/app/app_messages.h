// app_messages.h - Pulse's private window messages (WM_APP range).
// Light on purpose, so routing tables and tests can use the ids without the
// whole AppState.
#pragma once
#include <windows.h>

namespace pulse {

constexpr UINT WM_WORKER_RESULT = WM_APP + 1;
constexpr UINT WM_OPS_NOTIFY = WM_APP + 2;
constexpr UINT WM_INDEX_NOTIFY = WM_APP + 40;
constexpr UINT WM_INDEX_SEARCH = WM_APP + 42;
constexpr UINT WM_NET_PROBE = WM_APP + 41;
constexpr UINT WM_TAG_ADS_WARNING = WM_APP + 43;
constexpr UINT WM_TAG_ADS_DISCOVERED = WM_APP + 44;
// Explorer context-menu integration (优化.md §7). lParam owns a heap payload.
constexpr UINT WM_SHELLCTX_ITEMS = WM_APP + 45;  // ShellCtxItemsPayload*
constexpr UINT WM_SHELL_VERBS = WM_APP + 46;     // ShellVerbsResult*
constexpr UINT WM_DETAILS_META = WM_APP + 47;    // DetailsMetaResult*
constexpr UINT WM_SETTINGS_TASK_RESULT = WM_APP + 48;
constexpr UINT WM_SHELL_CACHE_INVALIDATE = WM_APP + 49;
constexpr UINT WM_NETWORK_INDEX_NOTIFY = WM_APP + 51;
constexpr UINT WM_NETWORK_INDEX_SEARCH = WM_APP + 52;
constexpr UINT WM_CONTENT_SEARCH = WM_APP + 53;
constexpr UINT WM_CONTENT_SELECTION = WM_APP + 64;
constexpr UINT WM_DUPLICATE_SCAN = WM_APP + 58;
constexpr UINT WM_QUICK_PREVIEW_NAVIGATE = WM_APP + 54;
constexpr UINT WM_QUICK_PREVIEW_OPEN = WM_APP + 55;
constexpr UINT WM_QUICK_PREVIEW_COMMAND = WM_APP + 65;  // lParam: immutable command token
constexpr UINT WM_SHOW_RELEASE_NOTES = WM_APP + 66;  // "updated" toast clicked
constexpr UINT WM_UPDATE_RESULT = WM_APP + 56;
constexpr UINT WM_RECYCLE_INFO = WM_APP + 57;
constexpr UINT WM_DUP_VOLUMES = WM_APP + 59;
constexpr UINT WM_UPDATE_DOWNLOADED = WM_APP + 60;
constexpr UINT WM_UPDATE_INSTALL = WM_APP + 61;
constexpr UINT WM_SEARCH_HISTORY = WM_APP + 62;
constexpr UINT WM_CHANGE_TRACKING = WM_APP + 63;
constexpr UINT WM_FRAME_PUMP = WM_APP + 67;  // app::FramePump: one display frame of motion
constexpr UINT WM_SHELL_SELECT = WM_APP + 68;  // app::ShellSelectRequest* (shell_window_sync.h)
constexpr UINT WM_EXPLORER_TAKEOVER = WM_APP + 69;  // app::ExplorerTakeoverRequest* (shell_window_sync.h)
constexpr UINT WM_NETWORK_LIVE_SEARCH = WM_APP + 71;  // shared_ptr<LiveNetworkSearch>* (#74 live walk progressed)
constexpr UINT WM_SHELL_VERB_SEED = WM_APP + 70;
constexpr UINT WM_EXIT_PULSE = WM_APP + 72;  // palette "Exit Pulse" (#57)  // ShellVerbSeed* (machine verb cache read off the UI thread)
constexpr UINT WM_NETWORK_LOCATIONS = WM_APP + 73;
constexpr UINT WM_ASSOC_CHANGED = WM_APP + 74;  // SHCNE_ASSOCCHANGED: default programs changed
constexpr UINT WM_BITLOCKER_UNLOCK = WM_APP + 75;  // app::BitLockerUnlockResult* (bitlocker_unlock.h)
constexpr UINT WM_RELEASE_WINDOW_STATES = WM_APP + 76;  // host: free retired window states (window_factory.h)
constexpr UINT WM_NEW_WINDOW = WM_APP + 77;  // open another window at this window's folder (M4a)
constexpr UINT WM_REOPEN_CLOSED_WINDOW = WM_APP + 78;  // reopen the last closed window (M4c toast, Ctrl+Shift+T)
constexpr UINT WM_OPEN_SESSION_WINDOWS = WM_APP + 79;  // first window shown: open the session's other windows (M4d)
constexpr UINT WM_MOVE_TAB_TO_NEW_WINDOW = WM_APP + 80;  // Ctrl+Shift+M: the active tab moves to a new window (M4e)
constexpr UINT WM_BEGIN_TAB_DETACH = WM_APP + 81;  // a tab dragged off the strip: start the system move loop (M6a)
constexpr UINT WM_UNDO_WINDOW_MERGE = WM_APP + 82;  // the merge toast was clicked: open the merged window again (M6c)
constexpr UINT WM_MERGE_WINDOW = WM_APP + 83;  // released on a tab strip after a title-bar drag; wParam target hwnd, lParam index (M6c)
constexpr UINT WM_CONVERT_EXTERNAL_OPEN = WM_APP + 84;  // the open toast was clicked: tab <-> new window (M7b)
constexpr UINT WM_ADOPT_OPEN_POLICY = WM_APP + 85;  // the "make it the default" toast was clicked (M7b)
constexpr UINT WM_REOPEN_CLOSED_TABS = WM_APP + 86;  // the "N tabs closed" toast was clicked (M7f)

}  // namespace pulse
