// closed_windows.h - Windows closed while others stayed open, kept so they
// can be reopened (multi-window M4c, plan §8.1): closing a window is undone
// from the toast in the window used next, or with Ctrl+Shift+T.
#pragma once
#include "session.h"

#include <windows.h>

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
    size_t Size() const noexcept { return items_.size(); }
    bool Empty() const noexcept { return items_.empty(); }

private:
    std::deque<ClosedWindow> items_;
};

}  // namespace pulse::app
