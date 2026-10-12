#include "window_switcher_ui.h"
#include "app_internal.h"
#include "tray_reveal.h"
#include "window_identity_ui.h"
#include "window_switcher.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include "../common/string_ids.h"
#include "../fs/fs_enum.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

namespace pulse {
namespace {

constexpr size_t kCardTabs = 3;
constexpr LPARAM kHookedEsc = 0x5357'0001;  // marks the Esc posted by the hook below

// Esc while Ctrl is held is Ctrl+Esc, which Windows takes for Start before
// the window sees it. While the switcher is open (it closes as soon as the
// window loses the foreground) a low-level hook takes Esc first and hands it
// to the window as the cancel key; it is removed when the switcher closes.
HHOOK g_esc_hook = nullptr;
HWND g_esc_window = nullptr;

LRESULT CALLBACK SwitcherEscHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g_esc_window && GetForegroundWindow() == g_esc_window) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (key->vkCode == VK_ESCAPE) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
                PostMessageW(g_esc_window, WM_KEYDOWN, VK_ESCAPE, kHookedEsc);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void HookEsc(HWND hwnd) {
    g_esc_window = hwnd;
    if (!g_esc_hook) g_esc_hook = SetWindowsHookExW(WH_KEYBOARD_LL, SwitcherEscHook, GetModuleHandleW(nullptr), 0);
}

void UnhookEsc() {
    if (g_esc_hook) UnhookWindowsHookEx(g_esc_hook);
    g_esc_hook = nullptr;
    g_esc_window = nullptr;
}

std::vector<HWND> OpenWindows(const AppState& s) {
    std::vector<HWND> out;
    s.core.windows.ForEach([&](HWND hwnd, AppState&) { out.push_back(hwnd); });
    return out;
}

// What the key below Esc types on this keyboard layout, for the hint.
std::wstring SwitcherKeyLabel() {
    const UINT vk = MapVirtualKeyW(0x29, MAPVK_VSC_TO_VK);
    const UINT ch = vk ? (MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu) : 0;  // high bit: dead key
    if (ch >= 0x20) return std::wstring(1, static_cast<wchar_t>(ch));
    wchar_t name[32]{};
    if (GetKeyNameTextW(0x29 << 16, name, 32) > 0) return name;
    return L"`";
}

std::wstring FormatString(l10n::StringId id, const wchar_t* arg) {
    wchar_t buf[256]{};
    swprintf_s(buf, l10n::Get(id).c_str(), arg);
    return buf;
}

ui::WindowSwitcherCard CardFor(AppState& w) {
    ui::WindowSwitcherCard card;
    card.name = WindowDisplayName(w);
    card.rgb = app::ValidWindowColor(w.identity.color) ? app::kWindowColors[w.identity.color] : 0x94A3B8;
    if (const app::LayoutTab* active = w.window_tabs.Active()) {
        if (const app::Tab* view = active->ActiveFolder()) {
            const std::wstring& path = view->current_path;
            card.folder = fs::IsVirtualPath(path) ? app::TabTitle(path) : path::StripExtendedPathPrefix(path);
        }
    }
    const size_t count = w.window_tabs.items.size();
    if (count == 1) {
        card.tab_count = l10n::Get(l10n::StringId::WindowSwitcherTabOne);
    } else {
        wchar_t buf[64]{};
        swprintf_s(buf, l10n::Get(l10n::StringId::WindowSwitcherTabs).c_str(), static_cast<int>(count));
        card.tab_count = buf;
    }
    // A few tabs around the active one, so it is always among them.
    const size_t active = std::min(w.window_tabs.active, count ? count - 1 : 0);
    const size_t first = active >= kCardTabs ? active - (kCardTabs - 1) : 0;
    for (size_t i = first; i < count && card.tabs.size() < kCardTabs; ++i) {
        const auto& tab = w.window_tabs.items[i];
        if (!tab) continue;
        if (i == active) card.active_tab = static_cast<int>(card.tabs.size());
        card.tabs.push_back(app::LayoutTabTitle(*tab));
    }
    return card;
}

void Kick(AppState& s) {
    if (s.framePump.Running()) s.framePump.Arm();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Lays the cards out again from the live windows (names, folders and the
// set of windows can change while it is open).
void Refresh(AppState& s) {
    app::PruneWindowSwitcher(s.switcher, OpenWindows(s));
    auto& view = s.renderer.WindowSwitcher();
    if (!s.switcher.IsOpen()) {
        UnhookEsc();
        view.Hide();
        Kick(s);
        return;
    }
    HookEsc(s.hwnd);
    ui::WindowSwitcherData data;
    for (HWND hwnd : s.switcher.order) {
        AppState* w = s.core.windows.Lookup(hwnd);
        data.cards.push_back(w ? CardFor(*w) : ui::WindowSwitcherCard{});
    }
    data.selected = s.switcher.selected;
    data.hint = FormatString(l10n::StringId::WindowSwitcherHint, SwitcherKeyLabel().c_str());
    view.Show(std::move(data),
              D2D1::RectF(0.0f, 0.0f, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height())),
              s.scale);
    Kick(s);
}

void Step(AppState& s, int dir) {
    app::StepWindowSwitcher(s.switcher, {}, dir);
    Refresh(s);
}

void Close(AppState& s) {
    UnhookEsc();
    app::CancelWindowSwitcher(s.switcher);
    s.renderer.WindowSwitcher().Hide();
    Kick(s);
}

void Commit(AppState& s) {
    app::PruneWindowSwitcher(s.switcher, OpenWindows(s));
    const HWND target = app::CommitWindowSwitcher(s.switcher);
    UnhookEsc();
    s.renderer.WindowSwitcher().Hide();
    Kick(s);
    if (!target) return;
    if (AppState* w = s.core.windows.Lookup(target)) RevealWindow(*w);  // restores a minimized window
}

bool Down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

}  // namespace

bool HandleWindowSwitcherKey(AppState& s, LPARAM key_lparam) {
    if (!app::IsWindowSwitcherScanCode(key_lparam) || !Down(VK_CONTROL) || Down(VK_MENU)) return false;
    if (s.switcher.IsOpen()) {
        Step(s, Down(VK_SHIFT) ? -1 : 1);
        return true;
    }
    if (!app::StepWindowSwitcher(s.switcher, OpenWindows(s), Down(VK_SHIFT) ? -1 : 1)) return false;
    Refresh(s);
    return true;
}

bool WindowSwitcherMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // The hook's Esc can arrive after the switcher closed: drop it.
    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && lParam == kHookedEsc) {
        if (s.switcher.IsOpen()) Close(s);
        return true;
    }
    if (!s.switcher.IsOpen() || hwnd != s.hwnd) return false;
    auto& view = s.renderer.WindowSwitcher();
    const float x = static_cast<float>(GET_X_LPARAM(lParam));
    const float y = static_cast<float>(GET_Y_LPARAM(lParam));
    switch (msg) {
    case WM_KEYDOWN:
        if (app::IsWindowSwitcherScanCode(lParam)) {
            Step(s, Down(VK_SHIFT) ? -1 : 1);
            return true;
        }
        switch (wParam) {
        case VK_ESCAPE: Close(s); break;
        case VK_RETURN:
        case VK_SPACE: Commit(s); break;
        case VK_RIGHT:
        case VK_DOWN: Step(s, 1); break;
        case VK_LEFT:
        case VK_UP: Step(s, -1); break;
        case VK_TAB: Step(s, Down(VK_SHIFT) ? -1 : 1); break;
        default: break;  // swallowed: Ctrl+W and friends must not act behind the cards
        }
        return true;
    case WM_KEYUP:
        if (wParam == VK_CONTROL) Commit(s);
        return true;
    case WM_CHAR:
    case WM_DEADCHAR:
        return true;
    case WM_SYSKEYDOWN:
        Close(s);
        return false;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const int card = view.HitCard(x, y);
        if (card >= 0) {
            app::SelectWindowSwitcher(s.switcher, card);
            Commit(s);
        } else if (!view.Contains(x, y)) {
            Close(s);
        }
        return true;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        return true;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        Close(s);
        return true;
    case WM_MOUSEWHEEL:
        Step(s, GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
        return true;
    case WM_KILLFOCUS:
    case WM_SIZE:
    case WM_CANCELMODE:
        Close(s);
        return false;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) Close(s);
        return false;
    default:
        return false;
    }
}

}  // namespace pulse
