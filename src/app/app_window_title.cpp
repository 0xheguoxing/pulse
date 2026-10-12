#include "app_window_title.h"
#include "app_runtime.h"
#include "window_identity_ui.h"

namespace pulse {
std::wstring TaskbarWindowTitle(const app::Tab* tab) {
    if (!tab) return L"Pulse";
    const auto name = tab->virtual_title.empty()
        ? app::TabTitle(tab->current_path) : tab->virtual_title;
    return name.empty() ? L"Pulse" : name + L" — Pulse";
}

bool SyncTaskbarWindowTitle(AppState& s) {
    if (!s.hwnd) return false;
    // M7c: with two or more windows each is called by its name, so Alt+Tab
    // and the taskbar tell them apart.
    const auto title = WindowIdentityShown(s) ? WindowDisplayName(s) + L" \u2014 Pulse" : TaskbarWindowTitle(ActiveTab(s));
    const int length = GetWindowTextLengthW(s.hwnd);
    if (length == static_cast<int>(title.size())) {
        std::wstring current(static_cast<size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(s.hwnd, current.data(), length + 1);
        current.resize(static_cast<size_t>(copied));
        if (current == title) return false;
    }
    return SetWindowTextW(s.hwnd, title.c_str()) != FALSE;
}
} // namespace pulse
