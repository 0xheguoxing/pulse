#include "edit_host.h"
#include "typography.h"
#include <commctrl.h>
#include <algorithm>
namespace pulse::ui {
constexpr UINT_PTR kEditCaretTimer = 71;
namespace {
// Re-arm the blink timer after a caret move so the next toggle comes one full
// interval later; the painter restarts the blink phase at the same moment.
void RestartCaretBlink(HWND hwnd) {
    if (GetFocus() == hwnd) SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
}
constexpr wchar_t kNativeEdit[] = L"Pulse.NativeEditFallback";
constexpr wchar_t kPolicyNative[] = L"Pulse.NativeEditPolicy";
constexpr wchar_t kRestoreUploaded[] = L"Pulse.EditRestoreUploaded";
constexpr wchar_t kMigrating[] = L"Pulse.EditMigrating";
void RestoreNativeCaret(HWND hwnd) {
    if (GetFocus() != hwnd) return;
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndCaret == hwnd) {
        // Custom mouse/paint paths can hide the system caret more than once.
        // Recreate it at the EDIT's current position to reset that hide count.
        const int width = (std::max)(1L, info.rcCaret.right - info.rcCaret.left);
        const int height = (std::max)(1L, info.rcCaret.bottom - info.rcCaret.top);
        if (CreateCaret(hwnd, nullptr, width, height)) SetCaretPos(info.rcCaret.left, info.rcCaret.top);
    }
    ShowCaret(hwnd);
}
// The EDIT shows and moves the system caret only while redraw is on, and the
// text input framework learns about typed text from those caret events. Under
// the redraw guard it never heard of a commit, so after the next one it put
// the selection back where focus had left it: in the search box the second
// word went in front of the first. Report the caret the EDIT skipped; the
// caret itself stays hidden behind the custom painter.
struct EditCaretState {
    DWORD from = 0;
    DWORD to = 0;
    int length = 0;
    bool operator==(const EditCaretState&) const = default;
};
EditCaretState ReadCaretState(HWND hwnd) {
    EditCaretState state;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&state.from), reinterpret_cast<LPARAM>(&state.to));
    state.length = GetWindowTextLengthW(hwnd);
    return state;
}
void ReportNativeCaret(HWND hwnd, const EditCaretState& state) {
    if (GetFocus() != hwnd) return;
    const LRESULT pos = SendMessageW(hwnd, EM_POSFROMCHAR, state.to, 0);
    if (pos != -1) SetCaretPos(static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos)));
    NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, hwnd, OBJID_CARET, CHILDID_SELF);
    NotifyWinEvent(EVENT_OBJECT_SHOW, hwnd, OBJID_CARET, CHILDID_SELF);
}
bool CustomEdit(Compositor& compositor, HWND hwnd) {
    const bool custom = compositor.CustomEditEnabled() && !GetPropW(hwnd, kNativeEdit);
    if (GetPropW(hwnd, kMigrating)) return custom;
    const bool policy_native = GetPropW(hwnd, kPolicyNative) != nullptr;
    if (!custom && !policy_native) {
        SetPropW(hwnd, kMigrating, reinterpret_cast<HANDLE>(1));
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(hwnd, nullptr, nullptr, &flags) || !(flags & LWA_ALPHA))
            SetPropW(hwnd, kRestoreUploaded, reinterpret_cast<HANDLE>(1));
        SetPropW(hwnd, kPolicyNative, reinterpret_cast<HANDLE>(1));
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        KillTimer(hwnd, kEditCaretTimer);
        RestoreNativeCaret(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        RemovePropW(hwnd, kMigrating);
    } else if (custom && policy_native) {
        SetPropW(hwnd, kMigrating, reinterpret_cast<HANDLE>(1));
        if (RemovePropW(hwnd, kRestoreUploaded)) {
            const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED);
        }
        RemovePropW(hwnd, kPolicyNative);
        if (GetFocus() == hwnd) {
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        RemovePropW(hwnd, kMigrating);
    }
    return custom;
}
bool PresentEdit(Compositor& compositor, HWND hwnd, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background) {
    bool force_failure = false;
#ifdef PULSE_WITH_SELFTEST
    force_failure = GetPropW(hwnd, L"Pulse.Test.LumaPresentFailure") != nullptr;
#endif
    if (!force_failure && compositor.PresentLumaEdit(hwnd, format, foreground, background)) return true;
    // Keep native EDIT input, selection and IME together if presentation fails.
    SetPropW(hwnd, kNativeEdit, reinterpret_cast<HANDLE>(1));
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    KillTimer(hwnd, kEditCaretTimer);
    RestoreNativeCaret(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    return false;
}
}
bool SynchronizeChildEditBackend(Compositor& compositor, HWND hwnd) {
    return hwnd && CustomEdit(compositor, hwnd);
}
bool PresentChildEdit(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd) {
    if (!hwnd || !CustomEdit(compositor, hwnd)) return false;
    HideCaret(hwnd);
    return PresentEdit(compositor, hwnd, format, foreground, background);
}
bool HandleChildEditMessage(Compositor& compositor, IDWriteTextFormat* format,
    D2D1_COLOR_F foreground, D2D1_COLOR_F background, HBRUSH background_brush,
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    (void)background_brush;
    if (msg == WM_NCDESTROY) {
        compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
        RemovePropW(hwnd, kNativeEdit);
        RemovePropW(hwnd, kPolicyNative);
        RemovePropW(hwnd, kRestoreUploaded);
        RemovePropW(hwnd, kMigrating);
        return false;
    }
    if (msg == typography::TextBackendChangedMessage()) {
        if (CustomEdit(compositor, hwnd) && IsWindowVisible(hwnd))
            PresentEdit(compositor, hwnd, format, foreground, background);
        result = 0;
        return true;
    }
    if (CustomEdit(compositor, hwnd) &&
        (msg == WM_PRINT || msg == WM_PRINTCLIENT || msg == WM_NCPAINT)) {
        result = 0;
        return true;
    }
    if (CustomEdit(compositor, hwnd) && msg == WM_ERASEBKGND) {
        result = 1;
        return true;
    }
    switch (msg) {
    case WM_LBUTTONDOWN: {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (!CustomEdit(compositor, hwnd)) return false;
        result = compositor.CallLumaEditMouse(
            hwnd, msg, wParam, lParam, format);
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_LBUTTONUP)
            RestartCaretBlink(hwnd);
        if (msg != WM_MOUSEMOVE || GetCapture() == hwnd) {
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        }
        return true;
    }
    case WM_PAINT: {
        if (!CustomEdit(compositor, hwnd)) return false;
        HideCaret(hwnd);
        PresentEdit(compositor, hwnd, format, foreground, background);
        result = 0;
        return true;
    }
    case WM_SETFOCUS: {
        result = DefSubclassProc(hwnd, msg, wParam, lParam);
        if (CustomEdit(compositor, hwnd)) {
            HideCaret(hwnd);
            compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam); // caret starts solid
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
            PresentEdit(compositor, hwnd, format,
                                         foreground, background);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return true;
    }
    case WM_KILLFOCUS:
        compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
        KillTimer(hwnd, kEditCaretTimer);
        return false;
    case WM_TIMER:
        if (wParam == kEditCaretTimer) {
            if (GetCapture() != hwnd && CustomEdit(compositor, hwnd)) {
                PresentEdit(compositor, hwnd, format,
                                             foreground, background);
            } else if (GetCapture() != hwnd) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            result = 0;
            return true;
        }
        return false;
    default:
        return false;
    }
}

LRESULT DefPresentedChildEditProc(Compositor& compositor, IDWriteTextFormat* format, D2D1_COLOR_F foreground, D2D1_COLOR_F background, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_IME_STARTCOMPOSITION) {
        // Start every composition from a caret, not a selection. When a word
        // replaced the selected stem (F2 selects it), Microsoft Pinyin then
        // moved the caret back to the start of the field, so the next word
        // went in front of the first (#116, #162, #211). Deleting the
        // selection first is what the result would do anyway.
        DWORD from = 0, to = 0;
        SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
        if (from != to) SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    }
    const bool changes_visual = msg == WM_SETTEXT || msg == EM_SETSEL || msg == EM_REPLACESEL ||
        msg == WM_KEYDOWN || msg == WM_CHAR || msg == WM_CUT || msg == WM_PASTE ||
        msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_SETFONT || msg == WM_SIZE || msg == EM_SETMARGINS ||
        msg == EM_SETCUEBANNER;
    const bool custom_paint = changes_visual && CustomEdit(compositor, hwnd) && IsWindowVisible(hwnd);
    const EditCaretState before = custom_paint ? ReadCaretState(hwnd) : EditCaretState{};
    if (custom_paint) SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (changes_visual) compositor.SynchronizeEditSelection(hwnd, msg, wParam, lParam);
    if (custom_paint) {
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        const EditCaretState after = ReadCaretState(hwnd);
        if (!(after == before)) ReportNativeCaret(hwnd, after);
        HideCaret(hwnd);
        RestartCaretBlink(hwnd);
        PresentEdit(compositor, hwnd, format,
            foreground, background);
    }
    return result;
}

}
