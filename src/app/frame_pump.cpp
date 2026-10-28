// frame_pump.cpp - see frame_pump.h.
#include "frame_pump.h"
#include <dwmapi.h>

namespace pulse::app {

namespace {
// Never faster than this, whatever the clock claims (a clock that returns at
// once - display off, DWM idle - must not become a busy loop).
constexpr double kMinFrameMs = 4.0;
constexpr double kFallbackFrameMs = 1000.0 / 60.0;
}

bool FramePump::Start(HWND hwnd, UINT message) {
    if (running_) return true;
    hwnd_ = hwnd;
    message_ = message;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    qpc_frequency_ = frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) return false;
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    // The export exists from Windows 11 (build 22000); the module stays loaded
    // for the process (the compositor links it anyway).
    if (HMODULE dcomp = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        wait_compositor_clock_ = reinterpret_cast<WaitCompositorClockFn>(
            reinterpret_cast<void*>(GetProcAddress(dcomp, "DCompositionWaitForCompositorClock")));
    }
    BOOL composition = FALSE;
    clock_ = wait_compositor_clock_ ? Clock::Compositor
           : (SUCCEEDED(DwmIsCompositionEnabled(&composition)) && composition) ? Clock::Dwm
           : Clock::Timer;
    stop_.store(false);
    try {
        thread_ = std::thread([this] { Run(); });
    } catch (...) {
        CloseHandle(wake_);
        wake_ = nullptr;
        if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
        return false;
    }
    running_ = true;
    return true;
}

void FramePump::Stop() {
    if (!running_) return;
    stop_.store(true);
    armed_.store(false);
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
    if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
    running_ = false;
}

void FramePump::ArmPaced(double min_frame_ms) noexcept {
    min_frame_us_.store(min_frame_ms > 0.0 ? static_cast<uint32_t>(min_frame_ms * 1000.0) : 0u,
                        std::memory_order_relaxed);
    if (!running_ || armed_.exchange(true, std::memory_order_relaxed)) return;
    SetEvent(wake_);
}

const wchar_t* FramePump::ClockName() const noexcept {
    switch (clock_) {
    case Clock::Compositor: return L"compositor";
    case Clock::Dwm: return L"dwm";
    default: return L"timer";
    }
}

void FramePump::Run() {
    // Thread names are optional; Windows 8.1 does not export this API.
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        auto set_description = reinterpret_cast<SetThreadDescriptionFn>(
            reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")));
        if (set_description) set_description(GetCurrentThread(), L"Pulse frame pump");
    }
    // Frame timing matters more than throughput here; the work per wake-up
    // is one PostMessage.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!stop_.load()) {
        if (!armed_.load(std::memory_order_relaxed)) {
            WaitForSingleObject(wake_, INFINITE);
            continue;
        }
        WaitForFrame();
        if (stop_.load() || !armed_.load(std::memory_order_relaxed)) continue;
        if (!posted_.exchange(true, std::memory_order_acq_rel) &&
            !PostMessageW(hwnd_, message_, 0, 0)) {
            posted_.store(false);
        }
    }
}

void FramePump::WaitForFrame() {
    LARGE_INTEGER now{};
    for (;;) {
        bool paced = false;
        if (clock_ == Clock::Compositor) {
            // STATUS_SUCCESS (0) on a clock tick; anything else (display off,
            // timeout) falls back to plain pacing below.
            paced = wait_compositor_clock_(0, nullptr, 100) == 0;
        } else if (clock_ == Clock::Dwm) {
            paced = SUCCEEDED(DwmFlush());
        }
        QueryPerformanceCounter(&now);
        double elapsed = static_cast<double>(now.QuadPart - last_frame_) * 1000.0 /
                         static_cast<double>(qpc_frequency_);
        const double floor_ms = paced ? kMinFrameMs : kFallbackFrameMs;
        if (last_frame_ != 0 && elapsed < floor_ms) {
            SleepMs(floor_ms - elapsed);
            QueryPerformanceCounter(&now);
            elapsed = static_cast<double>(now.QuadPart - last_frame_) * 1000.0 /
                      static_cast<double>(qpc_frequency_);
        }
        // ArmPaced: skip ticks until the interval is (nearly) up, re-read each
        // time so an Arm in between takes effect on the next tick. Half a
        // millisecond of slack keeps a 30 fps request on every other 60 Hz tick.
        const double min_ms = min_frame_us_.load(std::memory_order_relaxed) / 1000.0;
        if (last_frame_ == 0 || min_ms <= 0.0 || elapsed + 0.5 >= min_ms ||
            stop_.load() || !armed_.load(std::memory_order_relaxed)) {
            break;
        }
        if (!paced) SleepMs(min_ms - elapsed);
    }
    last_frame_ = now.QuadPart;
}

void FramePump::SleepMs(double ms) {
    if (ms <= 0.0) return;
    if (timer_) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);  // relative, 100 ns units
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            HANDLE handles[] = {timer_, wake_};
            // wake_ only interrupts for Stop; an Arm while armed never sets it.
            WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>(ms + 0.5));
}

} // namespace pulse::app
