#include "../common/windows_compat.h"
#include "window_helpers.h"

#include <algorithm>
#include <vector>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace pulse::ui {

float ScaleDip(float scale, float value) { return value * scale; }

D2D1_RECT_F DipRect(float scale, float x, float y, float width, float height) {
    return D2D1::RectF(ScaleDip(scale, x), ScaleDip(scale, y),
                       ScaleDip(scale, x + width), ScaleDip(scale, y + height));
}

bool ContainsRect(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lparam, float title_height,
                          const D2D1_RECT_F& client_buttons) {
    POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    RECT window{};
    GetWindowRect(hwnd, &window);
    const UINT dpi = pulse::compat::WindowDpi(hwnd);
    const int frame_x = pulse::compat::SystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                      + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int frame_y = pulse::compat::SystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                      + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const bool left = point.x < window.left + frame_x;
    const bool right = point.x >= window.right - frame_x;
    const bool top = point.y < window.top + frame_y;
    const bool bottom = point.y >= window.bottom - frame_y;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    ScreenToClient(hwnd, &point);
    if (point.y >= 0 && point.y < title_height &&
        !ContainsRect(client_buttons, static_cast<float>(point.x), static_cast<float>(point.y)))
        return HTCAPTION;
    return HTCLIENT;
}

namespace {

constexpr wchar_t kAcrylicProp[] = L"Pulse.DialogAcrylic";
constexpr UINT_PTR kKeepFrostedId = 0x50414352;

// Settings > Personalization > Colors > Transparency effects.
bool SystemTransparencyEnabled() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return true;
    }
    return value != 0;
}

// DWM flattens acrylic to a solid colour once the window is inactive; telling
// it the frame is still active keeps the dialog frosted behind other windows.
LRESULT CALLBACK KeepFrostedProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCACTIVATE) return DefSubclassProc(hwnd, message, TRUE, lparam);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, KeepFrostedProc, id);
        RemovePropW(hwnd, kAcrylicProp);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

} // namespace

bool ApplyBackdrop(HWND hwnd, bool dark) {
    UpdateWindowTheme(hwnd, dark);
    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const bool solid = IsHighContrast() || !SystemTransparencyEnabled();
    HRESULT result = E_FAIL;
    DWORD backdrop = DWMSBT_NONE;
    if (solid) {
        result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    } else {
        // Acrylic like the context menus; Mica Alt / Mica where it is missing.
        for (const DWORD type : {DWORD{DWMSBT_TRANSIENTWINDOW}, DWORD{DWMSBT_TABBEDWINDOW},
                                 DWORD{DWMSBT_MAINWINDOW}}) {
            backdrop = type;
            result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
            if (SUCCEEDED(result)) break;
        }
    }
    const bool acrylic = !solid && SUCCEEDED(result) && backdrop == DWMSBT_TRANSIENTWINDOW;
    if (acrylic) {
        SetPropW(hwnd, kAcrylicProp, reinterpret_cast<HANDLE>(1));
        SetWindowSubclass(hwnd, KeepFrostedProc, kKeepFrostedId, 0);
        if (GetForegroundWindow() != hwnd) SendMessageW(hwnd, WM_NCACTIVATE, TRUE, 0);
    } else {
        RemovePropW(hwnd, kAcrylicProp);
        RemoveWindowSubclass(hwnd, KeepFrostedProc, kKeepFrostedId);
    }
    MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    return !solid && SUCCEEDED(result);
}

bool IsAcrylicBackdrop(HWND hwnd) {
    return hwnd && GetPropW(hwnd, kAcrylicProp) != nullptr;
}

D2D1_COLOR_F GlassCard(bool dark) {
    return dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f);
}

D2D1_COLOR_F GlassSheet(bool dark) {
    return dark ? D2D1::ColorF(24.0f / 255.0f, 26.0f / 255.0f, 25.0f / 255.0f, 0.55f)
                : D2D1::ColorF(236.0f / 255.0f, 238.0f / 255.0f, 233.0f / 255.0f, 0.38f);
}

void ApplyGlass(Theme& theme, bool dark) {
    theme.surface_card = GlassCard(dark);
    theme.surface_sheet = GlassSheet(dark);
}

std::wstring FitTextEnd(const std::wstring& text, float width,
                       const std::function<float(std::wstring_view)>& measure) {
    if (measure(text) <= width) return text;
    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(text.substr(0, mid) + L"\u2026") <= width) lo = mid; else hi = mid - 1;
    }
    if (lo > 0 && IS_HIGH_SURROGATE(text[lo - 1])) --lo;
    return text.substr(0, lo) + L"\u2026";
}

std::wstring FitPathMiddle(const std::wstring& text, float width,
                           const std::function<float(std::wstring_view)>& measure) {
    if (text.empty() || measure(text) <= width) return text;
    std::vector<std::wstring> parts;
    for (size_t start = 0;;) {
        const size_t next = text.find(L'\\', start);
        parts.push_back(text.substr(start, next == std::wstring::npos ? std::wstring::npos
                                                                       : next - start));
        if (next == std::wstring::npos) break;
        start = next + 1;
    }
    if (parts.size() < 3 || parts.back().empty()) return FitTextEnd(text, width, measure);
    // A UNC path keeps its server as the root.
    size_t first = 1;
    std::wstring root = parts[0] + L"\\";
    if (parts[0].empty() && parts.size() > 3 && parts[1].empty()) {
        root = L"\\\\" + parts[2] + L"\\";
        first = 3;
    }
    std::wstring tail = parts.back();
    std::wstring best;
    for (size_t i = parts.size() - 1; i-- > first;) {
        const std::wstring candidate = root + L"\u2026\\" + parts[i] + L"\\" + tail;
        if (measure(candidate) > width) break;
        tail = parts[i] + L"\\" + tail;
        best = candidate;
    }
    if (!best.empty()) return best;
    const std::wstring last = root + L"\u2026\\" + parts.back();
    if (measure(last) <= width) return last;
    return FitTextEnd(parts.back(), width, measure);
}

void RedirectStrayModalKey(MSG& message, HWND dialog) {
    if (message.message < WM_KEYFIRST || message.message > WM_KEYLAST || !dialog) return;
    if (message.hwnd == dialog || IsChild(dialog, message.hwnd)) return;
    // Other enabled windows of this thread keep their typing.
    const HWND root = message.hwnd ? GetAncestor(message.hwnd, GA_ROOT) : nullptr;
    if (!root || IsWindowEnabled(root)) return;
    if (GetForegroundWindow() != dialog) SetForegroundWindow(dialog);
    const HWND focus = GetFocus();
    message.hwnd = (focus == dialog || IsChild(dialog, focus)) ? focus : dialog;
    constexpr LPARAM kAltDown = 1 << 29;
    if (!(message.lParam & kAltDown)) {
        if (message.message == WM_SYSKEYDOWN) message.message = WM_KEYDOWN;
        else if (message.message == WM_SYSKEYUP) message.message = WM_KEYUP;
        else if (message.message == WM_SYSCHAR) message.message = WM_CHAR;
    }
}

void CenterOwnedWindow(HWND hwnd, HWND owner, int width, int height,
                       bool clamp_to_work_area) {
    RECT anchor{};
    if (!owner || !GetWindowRect(owner, &anchor)) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        anchor = monitor.rcWork;
    }
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    if (clamp_to_work_area) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        x = std::clamp(x, static_cast<int>(monitor.rcWork.left),
                       (std::max)(static_cast<int>(monitor.rcWork.left),
                                  static_cast<int>(monitor.rcWork.right) - width));
        y = std::clamp(y, static_cast<int>(monitor.rcWork.top),
                       (std::max)(static_cast<int>(monitor.rcWork.top),
                                  static_cast<int>(monitor.rcWork.bottom) - height));
    }
    SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void BeginSurface(Compositor& compositor, fluent::Painter& painter,
                  const Theme& theme, bool dark, bool high_contrast,
                  bool backdrop_enabled, float scale) {
    auto* dc = compositor.Dc();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    painter.BeginFrame(theme, high_contrast);
    D2D1_COLOR_F tint = theme.bg;
    // Acrylic carries its own frost, so the tint stays thin (效果图 ④); Mica
    // needs more to read as a dialog rather than the wallpaper.
    tint.a = high_contrast || !backdrop_enabled ? 1.0f
           : IsAcrylicBackdrop(compositor.Hwnd()) ? 0.42f
           : (dark ? 0.76f : 0.82f);
    const float width = static_cast<float>(compositor.Width());
    const float height = static_cast<float>(compositor.Height());
    painter.FillRoundedRect(D2D1::RectF(0, 0, width, height), 0, tint);
    const float inset = 0.5f;
    painter.StrokeRoundedRect(D2D1::RectF(inset, inset, width - inset, height - inset),
                              12.0f * scale, theme.stroke_card);
}

void EndSurface(Compositor& compositor) {
    const HRESULT hr = compositor.Dc()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        compositor.NotifyDeviceLost(hr);
        return;
    }
    compositor.Present();
}

} // namespace pulse::ui
