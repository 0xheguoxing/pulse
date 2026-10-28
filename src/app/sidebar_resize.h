#pragma once
#include "app_state.h"
#include "../ui/ui_renderer.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace pulse {
inline bool SidebarResizeHit(const AppState& s, int x, int y) {
    const auto bounds = s.renderer.SidebarRect(static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height()));
    return bounds.right > 60 * s.scale && std::abs(x - bounds.right) <= 4 * s.scale &&
        y >= bounds.top && y < bounds.bottom;
}

inline bool HandleSidebarResize(AppState* s, HWND hwnd, UINT message, LPARAM lp) {
    if (!s) return false;
    if (message == WM_LBUTTONDOWN && SidebarResizeHit(*s, GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
        s->sidebarResizing = true;
        SetCapture(hwnd);
        return true;
    }
    if (!s->sidebarResizing) return false;
    if (message == WM_MOUSEMOVE) {
        // Explorer-style limit: the window decides how far the splitter goes.
        const int max_width = static_cast<int>(std::lround(
            s->renderer.SidebarMaxWidthDip(static_cast<float>(s->compositor.Width()))));
        const int width = std::clamp(static_cast<int>(std::lround(GET_X_LPARAM(lp) / s->scale)),
            static_cast<int>(ui::kSidebarMinWidthDip), max_width);
        s->appPrefs.sidebar_width = width;
        s->renderer.SetSidebarWidthDip(static_cast<float>(width));
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (message == WM_LBUTTONUP || message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        s->sidebarResizing = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (!s->isolatedTest) s->appPrefs.Save();
        return message == WM_LBUTTONUP;
    }
    return false;
}
}
