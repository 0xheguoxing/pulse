// tab_detach.cpp - Multi-window M6a: drags a tab off the strip into its own
// window (plan §8.2).
#include "tab_detach.h"

#include "app_internal.h"
#include "app_messages.h"
#include "tab_transfer.h"
#include "window_factory.h"
#include "window_registry.h"

#include <cmath>
#include <cstdlib>

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
    if (from) {
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
    } else {
        // The only tab drags the window exactly as its title bar would,
        // which also brings a maximized window down to its normal size.
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
    }
    if (g_escape_hook) UnhookWindowsHookEx(g_escape_hook);
    g_escape_hook = nullptr;
    if (!from || !g_escape_pressed || !IsWindow(hwnd)) return;

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
