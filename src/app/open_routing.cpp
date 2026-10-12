// open_routing.cpp - Folders from outside Pulse, plan §6 (see open_routing.h).
#include "open_routing.h"

#include "app_internal.h"
#include "default_file_manager.h"
#include "external_open.h"
#include "tray_reveal.h"
#include "window_factory.h"
#include "window_registry.h"

#include <vector>

namespace pulse {
namespace {

// Interrupting this window would cost the user something: a rename or an
// address being typed, a dialog it owns, or mouse capture anywhere on the UI
// thread (every Pulse window shares it): a tab, splitter or file drag (OLE
// captures too), an open menu, the move loop of a dragged-out tab.
bool IsBusy(const AppState& s) {
    if (s.renameIndex >= 0 || s.addressEditing) return true;
    if (s.hwnd && !IsWindowEnabled(s.hwnd)) return true;
    return GetCapture() != nullptr;
}

int LayoutTabIndex(const AppState& s, const void* tab) {
    if (!tab) return -1;
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i)
        if (s.window_tabs.items[i].get() == tab) return static_cast<int>(i);
    return -1;
}

const void* ActiveLayoutTab(const AppState& s) {
    return s.window_tabs.active < s.window_tabs.items.size() ? s.window_tabs.items[s.window_tabs.active].get()
                                                             : nullptr;
}

app::ExternalOpenPolicy OpenPolicy(const AppState&) {
    // The setting arrives with M7b; until then the plan's default.
    return app::ExternalOpenPolicy::Smart;
}

}  // namespace

AppState& OpenExternalFolder(AppState& mru, const std::wstring& raw) {
    const bool this_pc = app::IsThisPcArgument(raw);
    const std::wstring path = this_pc ? std::wstring() : ResolveOpenFolderPath(raw);
    // Screenshot runs stay one window (plan §8.7); nothing to open stays as before.
    if (mru.shot.active || (!this_pc && path.empty())) {
        OpenFolderInNewTab(mru, raw);
        return mru;
    }
    AppCore& core = mru.core;
    const uint64_t now = GetTickCount64();
    // Read before any Pulse window comes forward: where the request came from.
    const HWND foreground = GetForegroundWindow();

    std::vector<AppState*> windows;
    std::vector<app::ExternalOpenWindow> snapshot;
    app::ExternalOpenBatch batch = core.open_batch;
    bool batch_tab_movable = false;
    app::ForEachWindowSharingCore(mru, [&](AppState& w) {
        if (!w.hwnd) return;
        app::ExternalOpenWindow e;
        e.shows_folder = !this_pc && ShowsFolder(w, path);
        e.minimized = IsIconic(w.hwnd) != FALSE;
        e.hidden = !IsWindowVisible(w.hwnd);
        e.busy = IsBusy(w);
        e.last_input_tick = w.last_input_tick;
        e.batch_window = core.open_batch_window && w.hwnd == core.open_batch_window;
        if (e.batch_window)
            batch_tab_movable = LayoutTabIndex(w, core.open_batch_tab) >= 0 && w.window_tabs.items.size() >= 2;
        windows.push_back(&w);
        snapshot.push_back(e);
    });
    if (windows.empty()) {
        OpenFolderInNewTab(mru, raw);
        return mru;
    }
    // The first tab can only move out while it is still there and not alone.
    batch.tab_movable = batch.tab_movable && batch_tab_movable;

    app::ExternalOpenRequest request;
    request.can_dedupe = !this_pc;
    request.tick = now;
    const app::ExternalOpenDecision decision = app::DecideOpenTarget(request, snapshot, batch, OpenPolicy(mru));

    AppState* target = nullptr;
    bool opened = false;
    const auto open_window = [&](AppState& from, AppState* adopt_from, size_t adopt_index) -> AppState* {
        WindowInit init;
        init.path = path;
        init.adopt_from = adopt_from;
        init.adopt_index = adopt_index;
        init.monitor = foreground ? MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) : nullptr;
        const HWND hwnd = OpenPulseWindow(from, init);
        return hwnd ? core.windows.Lookup(hwnd) : nullptr;
    };
    switch (decision.action) {
    case app::ExternalOpenAction::Activate:
    case app::ExternalOpenAction::Tab:
    case app::ExternalOpenAction::JoinBatch:
        // OpenFolderInNewTab restores a minimized or hidden window (a fresh
        // start from the tray included) and brings an open folder forward.
        target = windows[decision.window];
        OpenFolderInNewTab(*target, raw);
        opened = decision.action != app::ExternalOpenAction::Activate;
        break;
    case app::ExternalOpenAction::NewWindow:
        if ((target = open_window(mru, nullptr, 0)) != nullptr) {
            opened = true;
            if (!this_pc) SelectLaunchedFile(*target, raw);
            RevealWindow(*target);
        }
        break;
    case app::ExternalOpenAction::PromoteBatch: {
        AppState& from = *windows[decision.window];
        const int index = LayoutTabIndex(from, core.open_batch_tab);
        // The new window takes the batch's first tab alive (as Ctrl+Shift+M
        // does), then this folder opens next to it.
        if (index >= 0 && (target = open_window(from, &from, static_cast<size_t>(index))) != nullptr) {
            opened = true;
            OpenFolderInNewTab(*target, raw);
        }
        break;
    }
    }
    if (!target) {
        // A window could not be created: a tab in the window that has it.
        target = &mru;
        OpenFolderInNewTab(mru, raw);
    }

    const bool tab_movable = decision.action == app::ExternalOpenAction::Tab && target->window_tabs.items.size() >= 2;
    core.open_batch = app::NextOpenBatch(batch, request, decision, opened, tab_movable);
    if (opened) {
        core.open_batch_window = target->hwnd;
        core.open_batch_tab = decision.action == app::ExternalOpenAction::Tab ? ActiveLayoutTab(*target) : nullptr;
    } else if (core.open_batch.count == 0) {
        core.open_batch_window = nullptr;
        core.open_batch_tab = nullptr;
    }
    return *target;
}

void NoteUserInput(AppState& any, const MSG& msg) {
    switch (msg.message) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        break;
    default:
        return;
    }
    if (!msg.hwnd) return;
    if (AppState* w = any.core.windows.Lookup(GetAncestor(msg.hwnd, GA_ROOT))) w->last_input_tick = GetTickCount64();
}

}  // namespace pulse
