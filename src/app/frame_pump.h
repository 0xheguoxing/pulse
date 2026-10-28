// frame_pump.h - Paces UI animation frames to the display.
//
// WM_TIMER is the lowest-priority message and its period is quantised to the
// system tick (15.6 ms), so a "16 ms" timer delivers ~32 uneven frames per
// second. While something animates, the pump instead wakes on the compositor
// clock (DCompositionWaitForCompositorClock, Windows 11; DwmFlush before that)
// and posts one coalesced message per display frame; it sleeps on an event and
// costs nothing when idle. The same approach as Chromium's and WinUI's
// compositor-driven frame scheduling, reduced to a single window.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <thread>

namespace pulse::app {

class FramePump {
public:
    FramePump() = default;
    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;
    ~FramePump() { Stop(); }

    // Posts `message` (wParam/lParam 0) to hwnd once per display frame while
    // armed. False when the worker thread could not start (callers then keep
    // their timer-driven frames).
    bool Start(HWND hwnd, UINT message);
    void Stop();

    // Any thread. Frames keep coming until Disarm; at most one message is in
    // the queue at a time. Arm follows the display; ArmPaced leaves at least
    // min_frame_ms between frames (still on clock ticks) for motion that only
    // decorates, such as loading shimmer. Whichever was called last applies.
    void Arm() noexcept { ArmPaced(0.0); }
    void ArmPaced(double min_frame_ms) noexcept;
    void Disarm() noexcept { armed_.store(false, std::memory_order_relaxed); }
    // UI thread, first thing when handling the message: allows the next post.
    void FrameConsumed() noexcept { posted_.store(false, std::memory_order_release); }

    bool Running() const noexcept { return running_; }
    bool Armed() const noexcept { return armed_.load(std::memory_order_relaxed); }
    // "compositor", "dwm" or "timer" - the clock the frames follow.
    const wchar_t* ClockName() const noexcept;

private:
    enum class Clock { Compositor, Dwm, Timer };
    using WaitCompositorClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);

    void Run();
    void WaitForFrame();
    void SleepMs(double ms);

    HWND hwnd_ = nullptr;
    UINT message_ = 0;
    std::thread thread_;
    HANDLE wake_ = nullptr;   // auto-reset: armed or stopping
    HANDLE timer_ = nullptr;  // high-resolution waitable timer for fallbacks
    WaitCompositorClockFn wait_compositor_clock_ = nullptr;
    Clock clock_ = Clock::Timer;
    bool running_ = false;
    LONGLONG qpc_frequency_ = 1;
    LONGLONG last_frame_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> armed_{false};
    std::atomic<bool> posted_{false};
    std::atomic<uint32_t> min_frame_us_{0};
};

} // namespace pulse::app
