#include "double_ctrl.h"
#include <shellapi.h>

namespace pulse {
bool DoubleCtrlDetector::Key(bool ctrl, bool down, uint64_t now) {
    if (!ctrl) {
        // Ctrl+anything is a shortcut, not a tap; a key typed between the taps
        // starts over. Releases of keys pressed earlier do not matter.
        if (down) phase_ = held_ ? Phase::Spoiled : Phase::Idle;
        return false;
    }
    if (down) {
        if (held_) {  // auto-repeat while held
            if (now - down_at_ > kHoldMs) phase_ = Phase::Spoiled;
            return false;
        }
        held_ = true;
        if (phase_ == Phase::FirstUp && now - up_at_ <= gap_) phase_ = Phase::SecondDown;
        else phase_ = Phase::FirstDown;
        down_at_ = now;
        return false;
    }
    if (!held_) return false;
    held_ = false;
    const bool tap = now - down_at_ <= kHoldMs;
    if (phase_ == Phase::FirstDown && tap) {
        phase_ = Phase::FirstUp;
        up_at_ = now;
        return false;
    }
    const bool fired = phase_ == Phase::SecondDown && tap;
    phase_ = Phase::Idle;
    return fired;
}

void DoubleCtrlDetector::Interrupt() {
    phase_ = held_ ? Phase::Spoiled : Phase::Idle;
}

bool DoubleCtrlWatcher::QuietMoment() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
}

void DoubleCtrlWatcher::Update(HWND target, UINT message, bool enabled, uint32_t gap_ms) {
    target_ = target;
    message_ = message;
    gap_ = gap_ms;
    if (!enabled || !target) { Stop(); return; }
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
}

void DoubleCtrlWatcher::Stop() {
    if (!thread_.joinable()) return;
    // The thread publishes its id before it can receive messages; spin until
    // then so the quit message is never lost.
    DWORD id = 0;
    while ((id = thread_id_.load()) == 0) Sleep(1);
    while (!PostThreadMessageW(id, WM_QUIT, 0, 0)) {
        if (GetLastError() != ERROR_INVALID_THREAD_ID) break;
        Sleep(1);
    }
    thread_.join();
    thread_id_ = 0;
}

LRESULT CALLBACK DoubleCtrlWatcher::WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    } else if (msg == WM_INPUT) {
        if (auto* self = reinterpret_cast<DoubleCtrlWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
            self->OnInput(reinterpret_cast<HRAWINPUT>(lparam));
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void DoubleCtrlWatcher::WatchMouse(bool on) {
    if (on == mouse_) return;
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;  // mouse
    device.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    device.hwndTarget = on ? window_ : nullptr;
    if (RegisterRawInputDevices(&device, 1, sizeof(device)) || !on) mouse_ = on;
}

void DoubleCtrlWatcher::OnInput(HRAWINPUT input) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    detector_.SetGap(gap_.load());
    const uint64_t now = GetTickCount64();
    if (raw.header.dwType == RIM_TYPEMOUSE) {
        if (raw.data.mouse.usButtonFlags) detector_.Interrupt();
    } else if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const auto& key = raw.data.keyboard;
        if (key.VKey == 0xFF || key.VKey == 0) return;  // fake keys from escaped scan sequences
        const bool ctrl = key.VKey == VK_CONTROL || key.VKey == VK_LCONTROL || key.VKey == VK_RCONTROL;
        const bool down = (key.Flags & RI_KEY_BREAK) == 0;
        if (detector_.Key(ctrl, down, now)) {
            const HWND target = target_.load();
            if (target && !QuietMoment()) PostMessageW(target, message_.load(), 0, 0);
        }
    }
    WatchMouse(detector_.Pending(now));
}

void DoubleCtrlWatcher::Run() {
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue
    thread_id_ = GetCurrentThreadId();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseDoubleCtrl";
    RegisterClassExW(&wc);  // fails harmlessly when already registered
    window_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
    if (window_) {
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;
        device.usUsage = 0x06;  // keyboard
        device.dwFlags = RIDEV_INPUTSINK;
        device.hwndTarget = window_;
        RegisterRawInputDevices(&device, 1, sizeof(device));
    }
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (window_) {
        WatchMouse(false);
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;
        device.usUsage = 0x06;
        device.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&device, 1, sizeof(device));
        DestroyWindow(window_);
        window_ = nullptr;
    }
    detector_ = DoubleCtrlDetector{};
}
}
