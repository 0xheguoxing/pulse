// closed_windows.h - Windows closed while others stayed open, kept so they
// can be reopened (multi-window M4c, plan §8.1): closing a window is undone
// from the toast in the window used next, or with Ctrl+Shift+T. Session v8
// keeps the newest few (M7f), and closed tabs (closed_tabs.h) share the
// numbering so Ctrl+Shift+T reopens whichever went last.
#pragma once
#include "session.h"

#include <windows.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pulse::app {

struct ClosedWindow {
    std::wstring name;   // active tab's title, shown in the toast
    RECT rect{};         // normal-state screen rectangle
    bool maximized = false;
    std::vector<LayoutTabSnapshot> layout_tabs;
    std::vector<GroupSessionSnapshot> tab_groups;
    int active_layout_tab = 0;
    // Its own sidebar and details pane (M4d); without one the window takes
    // the layout of the window that opens it.
    std::optional<WindowLayoutSnapshot> layout;
    WindowIdentity identity;  // M7c: comes back with its name and color
    uint64_t closed_seq = 0;  // M7f: when, among closed tabs and windows (AppCore::close_seq)
};

// Most recently closed last. Holds at most kCapacity windows; the oldest go.
class ClosedWindowStack {
public:
    static constexpr size_t kCapacity = 10;

    // A window without tabs has nothing to bring back and is not kept.
    void Push(ClosedWindow window) {
        if (window.layout_tabs.empty()) return;
        items_.push_back(std::move(window));
        while (items_.size() > kCapacity) items_.pop_front();
    }
    std::optional<ClosedWindow> TakeLatest() {
        if (items_.empty()) return std::nullopt;
        std::optional<ClosedWindow> latest(std::move(items_.back()));
        items_.pop_back();
        return latest;
    }
    const ClosedWindow* Latest() const noexcept { return items_.empty() ? nullptr : &items_.back(); }
    uint64_t LatestSeq() const noexcept { return items_.empty() ? 0 : items_.back().closed_seq; }
    // Oldest first (the session keeps the newest, M7f).
    const std::deque<ClosedWindow>& Items() const noexcept { return items_; }
    size_t Size() const noexcept { return items_.size(); }
    bool Empty() const noexcept { return items_.empty(); }

private:
    std::deque<ClosedWindow> items_;
};

// Session v8 form of a closed window (M7f); the rect stays a screen rect.
inline ClosedWindowSessionSnapshot ClosedWindowToSession(const ClosedWindow& closed) {
    ClosedWindowSessionSnapshot out;
    out.name = closed.name;
    out.window.window_rect = closed.rect;
    out.window.maximized = closed.maximized;
    if (closed.layout) out.window.layout = *closed.layout;
    out.window.layout_tabs = closed.layout_tabs;
    out.window.tab_groups = closed.tab_groups;
    out.window.active_layout_tab = closed.active_layout_tab;
    out.window.identity = closed.identity;
    return out;
}

// Back from the session; `seq` orders it among this run's closes.
inline ClosedWindow ClosedWindowFromSession(const ClosedWindowSessionSnapshot& saved, uint64_t seq) {
    ClosedWindow out;
    out.name = saved.name;
    out.rect = saved.window.window_rect;
    out.maximized = saved.window.maximized;
    out.layout = saved.window.layout;
    out.layout_tabs = saved.window.layout_tabs;
    out.tab_groups = saved.window.tab_groups;
    out.active_layout_tab = saved.window.active_layout_tab;
    out.identity = saved.window.identity;
    out.closed_seq = seq;
    return out;
}

}  // namespace pulse::app
