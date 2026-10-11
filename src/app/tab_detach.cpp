// tab_detach.cpp - Multi-window M6a: drags a tab off the strip into its own
// window (plan §8.2).
#include "tab_detach.h"

#include "app_internal.h"
#include "app_messages.h"
#include "tab_transfer.h"
#include "window_factory.h"
#include "window_registry.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace pulse {
namespace {
// Esc inside the system move loop: the loop reads the key itself, but a
// thread keyboard hook still sees it. Only one move loop runs at a time.
HHOOK g_escape_hook = nullptr;
bool g_escape_pressed = false;
// Where the cursor holds the window being dragged, relative to its top-left
// corner. The move loop starts a few mouse moves after the press or the
// detach; the window first catches up so the grab point stays put.
struct Grab {
    HWND hwnd = nullptr;
    POINT offset{};
};
Grab g_grab;
// The window in our move loop, and the strip spot its tab would drop on
// (windows are looked up by handle: one may close during the loop).
HWND g_moving = nullptr;
struct Drop {
    HWND window = nullptr;
    size_t index = 0;
};
Drop g_drop;

AppState* WindowOf(AppState& any, HWND hwnd) {
    if (!hwnd) return nullptr;
    AppState& found = app::FindWindowSharingCore(any, [hwnd](const AppState& w) { return w.hwnd == hwnd; });
    return found.hwnd == hwnd ? &found : nullptr;
}

void ShowDrop(AppState& any, const Drop& next) {
    if (g_drop.window == next.window && g_drop.index == next.index) return;
    if (AppState* old = WindowOf(any, g_drop.window)) {
        old->tabDropIndex = -1;
        InvalidateRect(old->hwnd, nullptr, FALSE);
    }
    g_drop = next;
    if (AppState* target = WindowOf(any, next.window)) {
        target->tabDropIndex = static_cast<int>(next.index);
        InvalidateRect(target->hwnd, nullptr, FALSE);
    }
}

// Over a strip the dragged window would hide the drop bar under its own
// tab: it turns see-through until it leaves the strip or the loop ends.
void SeeThrough(HWND hwnd, bool on) {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (on == ((ex & WS_EX_LAYERED) != 0)) return;
    if (on) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, 140, LWA_ALPHA);
    } else {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    }
}

bool Cloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

// The tab strip spot under `cursor` (screen px) in the topmost window below
// `moving`, when that window is another Pulse window on the same core.
Drop FindDrop(AppState& moving, POINT cursor) {
    for (HWND below = GetWindow(moving.hwnd, GW_HWNDNEXT); below; below = GetWindow(below, GW_HWNDNEXT)) {
        if (!IsWindowVisible(below) || IsIconic(below) || Cloaked(below)) continue;
        RECT bounds{};
        if (!GetWindowRect(below, &bounds) || !PtInRect(&bounds, cursor)) continue;
        AppState* target = WindowOf(moving, below);
        if (!target || target == &moving) return {};  // another program covers the spot
        const ui::WindowViewModel vm = BuildVm(*target, false);
        const float width = static_cast<float>(target->compositor.Width());
        std::vector<TabSpan> spans;
        for (int i = 0; i < static_cast<int>(vm.tabs.size()); ++i) {
            D2D1_RECT_F rc{};
            if (!target->renderer.TabItemRect(vm, width, i, &rc)) continue;
            spans.push_back(TabSpan{static_cast<size_t>(i),
                                    RECT{std::lround(rc.left), std::lround(rc.top), std::lround(rc.right),
                                         std::lround(rc.bottom)}});
        }
        POINT client = cursor;
        ScreenToClient(below, &client);
        const float scale = target->scale > 0.0f ? target->scale : 1.0f;
        const LONG caption_buttons = std::lround(3.0f * 46.0f * scale);  // minimize, maximize, close
        const int index = TabDropIndex(spans.data(), spans.size(), target->window_tabs.items.size(), client,
                                       std::lround(12.0f * scale), std::lround(width) - caption_buttons);
        if (index < 0) return {};
        return Drop{below, static_cast<size_t>(index)};
    }
    return {};
}

LRESULT CALLBACK EscapeHook(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && wparam == VK_ESCAPE && (lparam & 0x80000000) == 0)
        g_escape_pressed = true;
    return CallNextHookEx(g_escape_hook, code, wparam, lparam);
}

bool PrimaryButtonDown() {
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(button) & 0x8000) != 0;
}

// Drops the strip drag as WM_CAPTURECHANGED does; the tabs never reordered.
void EndStripDrag(AppState& s) {
    s.tabDragPending = false;
    s.tabDragging = false;
    s.tabDragIndex = -1;
    s.tabDragRunPos = 0;
    s.tabDragRunLen = 1;
    s.tabDragFromChip = false;
    s.tabDragGroupId = 0;
    s.tabDragSlots = 1.0f;
    s.tabOrder.clear();
    s.tabTracks.clear();
    s.tabOffsets.clear();
    s.chipTracks.clear();
    s.chipOffsets.clear();
    if (GetCapture() == s.hwnd) ReleaseCapture();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
}  // namespace

bool TabDetachDistanceReached(POINT press, POINT now, float scale) {
    const float limit = kTabDetachDistanceDip * (scale > 0.0f ? scale : 1.0f);
    return std::fabs(static_cast<float>(now.y - press.y)) >= limit;
}

RECT DetachedWindowRect(SIZE size, POINT cursor, POINT grab_client, POINT client_offset) {
    const LONG left = cursor.x - grab_client.x - client_offset.x;
    const LONG top = cursor.y - grab_client.y - client_offset.y;
    return RECT{left, top, left + size.cx, top + size.cy};
}

int TabDropIndex(const TabSpan* spans, size_t span_count, size_t count, POINT pt, LONG slack,
                 LONG strip_right) {
    if (!spans || span_count == 0) return -1;
    LONG top = spans[0].rect.top, bottom = spans[0].rect.bottom;
    LONG left = spans[0].rect.left;
    for (size_t i = 1; i < span_count; ++i) {
        top = (std::min)(top, spans[i].rect.top);
        bottom = (std::max)(bottom, spans[i].rect.bottom);
        left = (std::min)(left, spans[i].rect.left);
    }
    if (pt.y < top - slack || pt.y > bottom + slack || pt.x < left - slack || pt.x > strip_right) return -1;
    for (size_t i = 0; i < span_count; ++i) {
        if (pt.x < (spans[i].rect.left + spans[i].rect.right) / 2) return static_cast<int>(spans[i].index);
    }
    return static_cast<int>(count);
}

void TrackTabDrop(AppState& s) {
    if (g_moving != s.hwnd) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    ShowDrop(s, s.window_tabs.items.size() == 1 ? FindDrop(s, cursor) : Drop{});
    SeeThrough(s.hwnd, g_drop.window != nullptr);
}

bool TryDetachDraggedTab(AppState& s, int mx, int my) {
    if ((!s.tabDragPending && !s.tabDragging) || s.tabDragFromChip) return false;
    const int index = s.tabDragIndex;
    if (index < 0 || index >= static_cast<int>(s.window_tabs.items.size())) return false;
    const POINT now{mx, my};
    if (s.window_tabs.items.size() < 2) {
        // A window's only tab carries the window (plan §5 P2).
        if (std::abs(mx - s.tabDragStartPt.x) < GetSystemMetrics(SM_CXDRAG) &&
            std::abs(my - s.tabDragStartPt.y) < GetSystemMetrics(SM_CYDRAG))
            return false;
        POINT press = s.tabDragStartPt;
        ClientToScreen(s.hwnd, &press);
        RECT window{};
        GetWindowRect(s.hwnd, &window);
        g_grab = Grab{s.hwnd, POINT{press.x - window.left, press.y - window.top}};
        EndStripDrag(s);
        PostMessageW(s.hwnd, WM_BEGIN_TAB_DETACH, 0, 0);
        return true;
    }
    if (!TabDetachDistanceReached(s.tabDragStartPt, now, s.scale)) return false;

    // Where the cursor held the tab, so it holds the new window's tab there.
    const ui::WindowViewModel vm = BuildVm(s, false);
    const float width = static_cast<float>(s.compositor.Width());
    D2D1_RECT_F first{}, self{};
    bool have_first = false;
    for (int i = 0; i < static_cast<int>(vm.tabs.size()) && !have_first; ++i)
        have_first = s.renderer.TabItemRect(vm, width, i, &first);
    if (!have_first || !s.renderer.TabItemRect(vm, width, index, &self)) return false;
    const POINT grab{static_cast<LONG>(std::lround(first.left)) + (s.tabDragStartPt.x -
                                                                     static_cast<LONG>(std::lround(self.left))),
                     s.tabDragStartPt.y};
    RECT window{};
    GetWindowRect(s.hwnd, &window);
    POINT origin{0, 0};
    ClientToScreen(s.hwnd, &origin);
    POINT offset{origin.x - window.left, origin.y - window.top};
    SIZE size{window.right - window.left, window.bottom - window.top};
    if (IsZoomed(s.hwnd)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (GetWindowPlacement(s.hwnd, &placement)) {
            size = SIZE{placement.rcNormalPosition.right - placement.rcNormalPosition.left,
                        placement.rcNormalPosition.bottom - placement.rcNormalPosition.top};
        }
        offset = POINT{0, 0};
    }
    POINT cursor{};
    GetCursorPos(&cursor);

    WindowInit init;
    if (const app::Tab* view = s.window_tabs.items[static_cast<size_t>(index)]->ActiveFolder())
        init.path = view->current_path;
    init.adopt_from = &s;
    init.adopt_index = static_cast<size_t>(index);
    init.rect = DetachedWindowRect(size, cursor, grab, offset);
    EndStripDrag(s);
    if (const HWND hwnd = OpenPulseWindow(s, init)) {
        g_grab = Grab{hwnd, POINT{cursor.x - init.rect->left, cursor.y - init.rect->top}};
        PostMessageW(hwnd, WM_BEGIN_TAB_DETACH, reinterpret_cast<WPARAM>(s.hwnd),
                     static_cast<LPARAM>(index));
    }
    return true;
}

void RunDetachedTabMove(AppState& s, HWND from, size_t index) {
    const HWND hwnd = s.hwnd;
    const Grab grab = g_grab;
    g_grab = Grab{};
    // Released before the loop could start: the window stays where it opened.
    if (!PrimaryButtonDown() || g_escape_hook) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    if (grab.hwnd == hwnd && !IsZoomed(hwnd)) {
        SetWindowPos(hwnd, nullptr, cursor.x - grab.offset.x, cursor.y - grab.offset.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_escape_pressed = false;
    g_escape_hook = SetWindowsHookExW(WH_KEYBOARD, EscapeHook, nullptr, GetCurrentThreadId());
    g_moving = hwnd;
    g_drop = Drop{};
    if (from) {
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
    } else {
        // The only tab drags the window exactly as its title bar would,
        // which also brings a maximized window down to its normal size.
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
    }
    if (g_escape_hook) UnhookWindowsHookEx(g_escape_hook);
    g_escape_hook = nullptr;
    g_moving = nullptr;
    const HWND drop_window = g_drop.window;
    const size_t drop_index = g_drop.index;
    ShowDrop(s, Drop{});
    if (!IsWindow(hwnd)) return;
    SeeThrough(hwnd, false);

    // Released on another window's tab strip: the tab joins it there (M6b).
    AppState* const target = g_escape_pressed ? nullptr : WindowOf(s, drop_window);
    if (target && target != &s && s.window_tabs.items.size() == 1) {
        ShowWindow(hwnd, SW_HIDE);
        if (MoveLayoutTab(s, 0, *target, drop_index, TransferSource::Closing)) {
            DestroyWindow(hwnd);
            SetForegroundWindow(target->hwnd);
            return;
        }
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }
    if (!from || !g_escape_pressed) return;

    // Esc: the tab goes back to its place and the new window goes away.
    AppState& source = app::FindWindowSharingCore(s, [from](const AppState& w) { return w.hwnd == from; });
    if (&source == &s || s.window_tabs.items.size() != 1) return;
    ShowWindow(hwnd, SW_HIDE);
    if (!MoveLayoutTab(s, 0, source, index, TransferSource::Closing)) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        return;
    }
    DestroyWindow(hwnd);
    SetForegroundWindow(from);
}
}  // namespace pulse
