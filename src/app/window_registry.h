// window_registry.h - The Pulse windows of this process (multi-window M3).
//
// AppCore keeps one registry; every window registers on WM_CREATE and leaves
// on WM_DESTROY. Order is most recently activated first, which is where the
// host window routes process-level notifications and second launches until
// the open policy (M4) picks a window per request. Single UI thread: no locks.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pulse::app {

template <class Window>
class BasicWindowRegistry {
public:
    struct Entry {
        HWND hwnd = nullptr;
        Window* window = nullptr;
    };

    // Registers (or re-registers) a window as the most recent one.
    void Add(HWND hwnd, Window* window) {
        if (!hwnd || !window) return;
        Remove(hwnd);
        entries_.insert(entries_.begin(), Entry{hwnd, window});
    }
    bool Remove(HWND hwnd) {
        const auto it = Find(hwnd);
        if (it == entries_.end()) return false;
        entries_.erase(it);
        return true;
    }
    // Activation: moves a registered window to the front; unknown windows are ignored.
    void Touch(HWND hwnd) {
        const auto it = Find(hwnd);
        if (it == entries_.end() || it == entries_.begin()) return;
        std::rotate(entries_.begin(), it, it + 1);
    }
    Window* Lookup(HWND hwnd) const {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [hwnd](const Entry& e) { return e.hwnd == hwnd; });
        return it == entries_.end() ? nullptr : it->window;
    }
    Window* MostRecent() const { return entries_.empty() ? nullptr : entries_.front().window; }
    HWND MostRecentHwnd() const { return entries_.empty() ? nullptr : entries_.front().hwnd; }
    std::size_t Count() const noexcept { return entries_.size(); }
    bool Empty() const noexcept { return entries_.empty(); }
    // Most recent first. The callback must not add or remove windows.
    template <class F>
    void ForEach(F&& f) const {
        for (const Entry& e : entries_) f(e.hwnd, *e.window);
    }

private:
    typename std::vector<Entry>::iterator Find(HWND hwnd) {
        return std::find_if(entries_.begin(), entries_.end(),
                            [hwnd](const Entry& e) { return e.hwnd == hwnd; });
    }

    std::vector<Entry> entries_;
};

}  // namespace pulse::app
