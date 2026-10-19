// ui_timer_pacer.h - Chooses the main window's kTimerUi period.
//
// kTimerUi polls background results and steps timer-driven animations. At a
// fixed 16 ms it woke the UI thread ~40-60 times a second even when nothing on
// screen could change, which keeps the CPU out of its deep idle states. The
// pacer keeps the fast period while the user interacts or a tick produced
// visible change, and relaxes to a slow poll once the window has been quiet
// for a moment. Any input, paint or dirty tick restores the fast period at
// once, so animations still start on the next display-rate tick; only work
// that begins with no input and no repaint waits for the slow poll.
#pragma once
#include <windows.h>

namespace pulse::app {

class UiTimerPacer {
public:
    static constexpr UINT kActiveMs = 16;
    static constexpr UINT kIdleMs = 100;
    // Hidden to the tray or minimized: background results only need to be
    // picked up eventually, and showing the window is activity (fast again).
    static constexpr UINT kHiddenMs = 1000;
    // How long the fast period lasts after the last activity.
    static constexpr ULONGLONG kLingerMs = 1000;

    void NoteActivity(ULONGLONG now) noexcept { active_until_ = now + kLingerMs; }

    UINT Want(bool visible, ULONGLONG now) const noexcept {
        if (!visible) return kHiddenMs;
        return now < active_until_ ? kActiveMs : kIdleMs;
    }

    // True when the period changes; `period` then holds the new value.
    bool Update(bool visible, ULONGLONG now, UINT& period) noexcept {
        const UINT want = Want(visible, now);
        if (want == current_) return false;
        current_ = want;
        period = want;
        return true;
    }

    UINT Current() const noexcept { return current_; }

private:
    UINT current_ = kActiveMs;
    ULONGLONG active_until_ = 0;
};

} // namespace pulse::app
