// host_window.cpp - see host_window.h.
#include "host_window.h"

#include <utility>

namespace pulse::app {
namespace {
constexpr wchar_t kHostClass[] = L"PulseHostWindow";
}  // namespace

const wchar_t* HostWindow::ClassName() noexcept { return kHostClass; }

bool HostWindow::Create(HINSTANCE instance, Handler handler) {
    if (hwnd_) return false;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    handler_ = std::move(handler);
    // Never shown (no WS_VISIBLE); a tool window stays out of Alt+Tab and the taskbar.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Pulse", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        handler_ = nullptr;
        return false;
    }
    return true;
}

void HostWindow::Destroy() {
    if (!hwnd_) return;
    const HWND hwnd = std::exchange(hwnd_, nullptr);
    // Detach first: the handler may capture state that is already being torn down.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    handler_ = nullptr;
}

LRESULT CALLBACK HostWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<HostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        self && self->handler_) {
        if (const auto result = self->handler_(hwnd, message, wparam, lparam)) return *result;
    }
    // The host lives as long as the process. WM_CLOSE (taskkill without /F,
    // shutdown tools) closes each UI window through its own WM_CLOSE; the
    // default handling here would destroy the endpoint silently.
    if (message == WM_CLOSE) return 0;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace pulse::app
