#include "window_identity_ui.h"

#include "app_internal.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/FluentTokens.h"
#include "../ui/fluent_menu.h"
#include "app_window_title.h"
#include "window_identity.h"
#include "window_overview_ui.h"
#include "window_registry.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse {
namespace {

// Commands of the editor menu (its own popup; no other verbs share it).
constexpr int kCmdAutoName = 1;
constexpr int kCmdOverview = 2;
constexpr int kCmdColorBase = 10;

// A round dot in `rgb` with a light rim, so it reads on any taskbar.
HICON MakeDotIcon(uint32_t rgb, int size) {
    if (size <= 0) return nullptr;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    const float center = size * 0.5f;
    const float outer = center - 0.5f;
    const float rim = (std::max)(1.0f, size / 10.0f);
    const float fr = static_cast<float>((rgb >> 16) & 0xFF);
    const float fg = static_cast<float>((rgb >> 8) & 0xFF);
    const float fb = static_cast<float>(rgb & 0xFF);
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = x + 0.5f - center;
            const float dy = y + 0.5f - center;
            const float d = std::sqrt(dx * dx + dy * dy);
            const float alpha = std::clamp(outer - d + 0.5f, 0.0f, 1.0f);
            const float fill = std::clamp(outer - rim - d + 0.5f, 0.0f, 1.0f);
            const auto mix = [fill](float c) { return static_cast<uint32_t>(std::lround(c * fill + 255.0f * (1.0f - fill))); };
            const uint32_t a = static_cast<uint32_t>(std::lround(alpha * 255.0f));
            px[y * size + x] = (a << 24) | (mix(fr) << 16) | (mix(fg) << 8) | mix(fb);
        }
    }
    std::vector<BYTE> zeros(static_cast<size_t>(((size + 15) / 16) * 2 * size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

}  // namespace

std::wstring WindowDisplayName(const AppState& s) {
    if (!s.identity.name.empty()) return s.identity.name;
    std::vector<std::wstring> paths;
    for (const auto& layout : s.window_tabs.items) {
        if (!layout) continue;
        for (const app::Pane* pane : layout->VisiblePanes())
            if (pane) paths.push_back(pane->ActiveTab()->current_path);
    }
    const std::wstring folder = app::AutoNameFolder(paths);
    if (!folder.empty()) return app::TabTitle(folder);
    if (const app::LayoutTab* active = s.window_tabs.Active()) return app::LayoutTabTitle(*active);
    return L"Pulse";
}

bool WindowIdentityShown(const AppState& s) {
    return app::ShowsWindowIdentity(s.core.windows.Count());
}

UINT TaskbarButtonCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarButtonCreated");
    return message;
}

void SyncWindowOverlay(AppState& s) {
    if (!s.hwnd) return;
    const bool show = WindowIdentityShown(s) && app::ValidWindowColor(s.identity.color);
    HICON next = nullptr;
    std::wstring label;
    if (show) {
        const int size = pulse::compat::SystemMetricsForDpi(SM_CXSMICON, pulse::compat::WindowDpi(s.hwnd));
        next = MakeDotIcon(app::kWindowColors[s.identity.color], size);
        label = WindowDisplayName(s);
    }
    if (next || s.identity_overlay) {
        Microsoft::WRL::ComPtr<ITaskbarList3> taskbar;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) &&
            SUCCEEDED(taskbar->HrInit()))
            taskbar->SetOverlayIcon(s.hwnd, next, next ? label.c_str() : nullptr);
    }
    ReleaseWindowOverlay(s);
    s.identity_overlay = next;
}

void ReleaseWindowOverlay(AppState& s) {
    if (s.identity_overlay) DestroyIcon(s.identity_overlay);
    s.identity_overlay = nullptr;
}

void SyncWindowIdentities(AppState& any) {
    std::vector<int> taken;
    app::ForEachWindowSharingCore(any, [&](AppState& w) {
        if (app::ValidWindowColor(w.identity.color)) taken.push_back(w.identity.color);
    });
    app::ForEachWindowSharingCore(any, [&](AppState& w) {
        if (!app::ValidWindowColor(w.identity.color)) {
            w.identity.color = app::PickWindowColor(taken);
            taken.push_back(w.identity.color);
        }
        if (!w.hwnd) return;
        SyncTaskbarWindowTitle(w);
        SyncWindowOverlay(w);
        InvalidateRect(w.hwnd, nullptr, FALSE);
    });
}

bool HandleWindowIdentityPress(AppState& s, const ui::HitTestResult& hit, POINT client) {
    if (hit.region != ui::HitTestResult::WindowIdentity || !s.hwnd) return false;
    s.dragPending = false;
    POINT screen = client;
    ClientToScreen(s.hwnd, &screen);
    // Still part of the title bar: a drag moves the window (and can merge it).
    if (DragDetect(s.hwnd, screen)) {
        ReleaseCapture();
        SendMessageW(s.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screen.x, screen.y));
    }
    return true;
}

bool HandleWindowIdentityDoubleClick(AppState& s, const ui::HitTestResult& hit, POINT client) {
    if (hit.region != ui::HitTestResult::WindowIdentity || !s.hwnd) return false;
    s.dragPending = false;
    POINT screen = client;
    ClientToScreen(s.hwnd, &screen);
    ShowWindowIdentityMenu(s, screen);
    return true;
}

void ShowWindowIdentityMenu(AppState& s, POINT screen) {
    if (!s.hwnd || !EnsureMenu(s)) return;
    ui::FluentMenu& menu = *s.menu;
    const std::wstring before = WindowDisplayName(s);
    const int color = s.identity.color;
    const bool typed_name = !s.identity.name.empty();
    menu.SetFilterPlaceholder(l10n::Get(l10n::StringId::WindowNamePlaceholder));
    menu.SetInitialFilterText(before);
    menu.SetSelectAllOnOpen(true);
    menu.SetFilterMinWidth(260.0f);
    const auto build = [&](const std::wstring&) {
        std::vector<ui::FluentMenuItem> items;
        ui::FluentMenuItem colors;
        for (int i = 0; i < app::kWindowColorCount; ++i) {
            ui::FluentMenuSwatch swatch;
            swatch.command = kCmdColorBase + i;
            swatch.color = ui::HexColor(app::kWindowColors[i]);
            swatch.checked = i == color;
            colors.quick_swatches.push_back(swatch);
        }
        colors.separator_after = true;
        items.push_back(std::move(colors));
        ui::FluentMenuItem automatic;
        automatic.command = kCmdAutoName;
        automatic.text = l10n::Get(l10n::StringId::WindowAutoName);
        automatic.glyph = L"\uE72C";  // Refresh
        automatic.enabled = typed_name;
        items.push_back(std::move(automatic));
        ui::FluentMenuItem overview;  // M7e: every window at once
        overview.command = kCmdOverview;
        overview.text = l10n::Get(l10n::StringId::WindowOverviewTitle);
        overview.glyph = L"\uE8A9";  // ViewAll
        overview.shortcut = L"Ctrl+Shift+O";
        items.push_back(std::move(overview));
        return items;
    };
    const int command = menu.TrackPopup(screen, build(before), build);
    menu.SetFilterPlaceholder(l10n::Get(l10n::StringId::TabMenuSearch));
    const std::wstring typed = menu.LastFilterQuery();

    if (command == kCmdOverview) {
        ToggleWindowOverview(s);
        return;
    }
    app::WindowIdentity next = s.identity;
    if (command == kCmdAutoName) {
        next.name.clear();
    } else {
        if (command >= kCmdColorBase && command < kCmdColorBase + app::kWindowColorCount)
            next.color = command - kCmdColorBase;
        // Like a tab group's name: what is in the field when the editor
        // closes is the name, unless it was left as it was.
        if (typed != before) next.name = app::NormalizeWindowName(typed);
    }
    if (next == s.identity) return;
    s.identity = std::move(next);
    SyncWindowIdentities(s);
}

}  // namespace pulse
