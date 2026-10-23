#include "dialog_jump.h"
#include "jump_match.h"
#include "../common/localization.h"
#include "../index/index_client.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace pulse::app {
namespace {
using Microsoft::WRL::ComPtr;
using l10n::StringId;

constexpr UINT kMsgContext = WM_APP + 1;      // SetContext stored a new context
constexpr UINT kMsgFocus = WM_APP + 2;        // FocusSearch
constexpr UINT kMsgIndexStatus = WM_APP + 3;
constexpr UINT kMsgIndexResult = WM_APP + 4;
constexpr UINT kMsgStop = WM_APP + 5;
constexpr UINT_PTR kTimerRestore = 1, kTimerAddress = 2, kTimerSearch = 3, kTimerProbe = 4;
constexpr int kHotkeyId = 0x5054;
constexpr int kEditId = 100;
constexpr UINT kMenuBase = 1000;
constexpr uint64_t kSession = 0x50554c53444a4d50ull;
constexpr float kBarHeight = 40, kRowHeight = 44, kGap = 6;
constexpr size_t kMaxRows = 8, kMaxChips = 4, kMaxMenu = 14;
constexpr wchar_t kBarClass[] = L"Pulse.DialogJump";
constexpr wchar_t kListClass[] = L"Pulse.DialogJumpList";

std::wstring ClassOf(HWND hwnd) {
    wchar_t buffer[64]{};
    GetClassNameW(hwnd, buffer, 64);
    return buffer;
}
std::wstring TextOf(HWND hwnd) {
    DWORD_PTR length = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 300, &length)) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(hwnd, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()), SMTO_ABORTIFHUNG,
                             300, &copied))
        return {};
    text.resize(std::min(static_cast<size_t>(copied), text.size()));
    while (!text.empty() && text.back() == L'\0') text.pop_back();
    return text;
}
bool SetText(HWND hwnd, const std::wstring& text) {
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()), SMTO_ABORTIFHUNG, 500,
                               &result) != 0;
}
std::wstring Leaf(const std::wstring& path) {
    std::wstring trimmed = path;
    while (trimmed.size() > 3 && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const auto slash = trimmed.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= trimmed.size() ? trimmed : trimmed.substr(slash + 1);
}
std::wstring Parent(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash == 2 ? 3 : slash);
}
bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
// Dialogs reject the long-path form Pulse keeps: \\?\C:\x is C:\x and
// \\?\UNC\s\x is \\s\x.
std::wstring PlainPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + path.substr(8);
    if (path.rfind(L"\\\\?\\", 0) == 0) return path.substr(4);
    return path;
}
std::wstring FolderInput(std::wstring path) {
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path;
}

DWORD IntegrityOf(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return 0xFFFFFFFF;
    DWORD level = 0xFFFFFFFF;
    alignas(8) BYTE buffer[96]{};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
        const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
        level = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1u);
    }
    CloseHandle(token);
    return level;
}
// Messages to a more privileged process are dropped (UIPI): leave its dialogs alone.
bool ReachableProcess(DWORD pid) {
    static const DWORD ours = IntegrityOf(GetCurrentProcess());
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const DWORD theirs = IntegrityOf(process);
    CloseHandle(process);
    return theirs != 0xFFFFFFFF && theirs <= ours;
}

struct DialogInfo {
    HWND dialog = nullptr, name_edit = nullptr, ok = nullptr;
    bool save = false, folder_picker = false, modern = false;
    DWORD pid = 0, tid = 0;
};

HWND FirstEdit(HWND parent) {
    HWND found = nullptr;
    EnumChildWindows(parent, [](HWND child, LPARAM param) -> BOOL {
        if (ClassOf(child) != L"Edit") return TRUE;
        *reinterpret_cast<HWND*>(param) = child;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// A common file dialog: #32770 with a shell view inside. The file name box is
// control 0x47C (cmb13, an Edit inside) or 0x480 (edt1); the type filter is
// 0x470 (cmb1), which folder pickers do not have. Windows 11 hosts both in a
// FloatNotifySink with id 0: the name box is the combo with an Edit inside.
bool ProbeDialog(HWND hwnd, DialogInfo& out) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || ClassOf(hwnd) != L"#32770") return false;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid || pid == GetCurrentProcessId()) return false;
    struct Scan {
        bool view = false, modern = false, types = false;
        HWND name_box = nullptr, edt1 = nullptr;
    } scan;
    EnumChildWindows(hwnd, [](HWND child, LPARAM param) -> BOOL {
        auto& s = *reinterpret_cast<Scan*>(param);
        const auto cls = ClassOf(child);
        const int id = GetDlgCtrlID(child);
        if (cls == L"DUIViewWndClassName") s.view = s.modern = true;
        else if (cls == L"SHELLDLL_DefView") s.view = true;
        if (id == 0x470) s.types = true;
        if (id == 0x47C && !s.name_box && (cls == L"ComboBoxEx32" || cls == L"ComboBox")) s.name_box = child;
        if (id == 0 && cls == L"ComboBox" && ClassOf(GetParent(child)) == L"FloatNotifySink") {
            if (FindWindowExW(child, nullptr, L"Edit", nullptr)) {
                if (!s.name_box) s.name_box = child;
            } else {
                s.types = true;
            }
        }
        if (id == 0x480 && cls == L"Edit") s.edt1 = child;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scan));
    if (!scan.view || !ReachableProcess(pid)) return false;
    DialogInfo info;
    info.dialog = hwnd;
    info.pid = pid;
    info.tid = tid;
    info.modern = scan.modern;
    info.name_edit = scan.name_box ? FirstEdit(scan.name_box) : scan.edt1;
    info.ok = GetDlgItem(hwnd, IDOK);
    const auto ok_text = info.ok ? TextOf(info.ok) : std::wstring();
    for (const wchar_t* word : {L"\u4FDD\u5B58", L"\u5132\u5B58", L"\u5B58\u50A8", L"Save", L"save"})
        if (ok_text.find(word) != std::wstring::npos) info.save = true;
    info.folder_picker = scan.modern && !scan.types && !info.save;
    if (!info.name_edit && !info.modern) return false;
    out = info;
    return true;
}

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f) {
    return D2D1::ColorF(rgb, alpha);
}
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
    color.a = alpha;
    return color;
}
COLORREF ToColorRef(D2D1_COLOR_F color) {
    return RGB(static_cast<int>(color.r * 255.0f + 0.5f), static_cast<int>(color.g * 255.0f + 0.5f),
               static_cast<int>(color.b * 255.0f + 0.5f));
}
bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

thread_local DialogJump::Impl* t_impl = nullptr;
} // namespace

struct DialogJump::Impl {
    // Shared with the UI thread.
    std::mutex mu;
    DialogJumpContext shared_context;
    std::atomic<HWND> service{nullptr};
    std::atomic<UINT> context_message{0};
    std::atomic<HWND> bar_window{nullptr};
    std::atomic<bool> showing{false};
    std::atomic<bool> ready{false};

    // The bar thread only.
    struct Palette {
        D2D1_COLOR_F back, border, field, field_border, text, muted, hover, accent, folder;
    } pal{};
    enum class Hit { None, Field, Chip, Current, More };
    struct Chip {
        D2D1_RECT_F rect{};
        size_t folder = 0;
        bool current = false;
    };
    struct Result {
        std::wstring path, name;
        bool dir = true, open = false;
    };

    HWND bar = nullptr, list = nullptr, edit = nullptr, tooltip = nullptr;
    WNDPROC edit_proc = nullptr;
    HFONT edit_font = nullptr;
    HBRUSH field_brush = nullptr;
    HWINEVENTHOOK foreground_hook = nullptr, dialog_hook = nullptr;
    bool hotkey = false;
    DialogInfo dlg;
    HWND probe_target = nullptr;
    int probe_tries = 0;
    DialogJumpContext ctx;
    float scale = 1.0f;
    float width_dip = 0;
    bool list_below = true;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1HwndRenderTarget> bar_rt, list_rt;
    ComPtr<IDWriteTextFormat> text_format, small_format, glyph_format, logo_format, key_format;
    D2D1_RECT_F logo_rect{}, field_rect{}, current_rect{}, more_rect{}, none_rect{};
    std::vector<Chip> chips;
    Hit hover = Hit::None;
    size_t hover_chip = 0;
    bool tracking = false;

    std::vector<Result> results;
    size_t known_count = 0;
    int selected = -1;
    int list_hover = -1;
    std::wstring query;
    bool searching = false;
    index::IndexClient index_client;
    bool index_started = false;
    uint32_t search_id = 0;

    std::wstring restore_text, set_text;
    HWND restore_edit = nullptr;
    int restore_ticks = 0;
    std::wstring address_path;
    int address_ticks = 0;
    bool address_keys_sent = false;

    void Run();
    void Evaluate(HWND foreground);
    void Attach(const DialogInfo& info);
    void Detach();
    void Place();
    void Layout();
    void PaintBar();
    void PaintList();
    void UpdatePalette();
    void TakeContext();
    void UpdateTooltips();
    void OnSearchChanged();
    void StartIndexSearch();
    void OnIndexResult(uint32_t id);
    void ShowList();
    void HideList();
    void Pick(size_t row);
    void Navigate(const std::wstring& path, bool dir);
    void NavigateAddressBar(const std::wstring& path);
    void SendAddressKeys();
    void ReturnToDialog();
    void ShowMenu();
    void FocusEdit();
    float TextWidth(const std::wstring& text, IDWriteTextFormat* format, float max_width);
    void DrawText(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const std::wstring& text,
                  IDWriteTextFormat* format, D2D1_RECT_F rect, D2D1_COLOR_F color);
    std::vector<size_t> OpenFolders() const;

    static LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG id_object, LONG id_child, DWORD, DWORD);
    static void CALLBACK OnDialogEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child, DWORD,
                                       DWORD);
    LRESULT HandleBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleList(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

// --- thread -----------------------------------------------------------------

void DialogJump::Impl::Run() {
    t_impl = this;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = &Impl::BarProc;
    wc.lpszClassName = kBarClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = &Impl::ListProc;
    wc.lpszClassName = kListClass;
    RegisterClassExW(&wc);
    bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kBarClass, L"Pulse", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 10,
                          10, nullptr, nullptr, instance, nullptr);
    list = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kListClass, L"", WS_POPUP, 0, 0, 10,
                           10, bar, nullptr, instance, nullptr);
    if (!bar || !list || !d2d || !dwrite) {
        ready = true;
        return;
    }
    for (HWND hwnd : {bar, list}) {
        const DWORD corner = 2;  // DWMWCP_ROUND (Windows 11; ignored before)
        DwmSetWindowAttribute(hwnd, 33, &corner, sizeof(corner));
    }
    edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, bar,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance, nullptr);
    edit_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Impl::EditProc)));
    SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(l10n::Get(StringId::DialogJumpSearchCue).c_str()));
    tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0,
                              0, bar, nullptr, instance, nullptr);
    if (tooltip) SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 600);

    // Text formats are in DIPs; the render targets carry the dialog's DPI.
    auto make = [&](const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& out) {
        dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                 L"", out.GetAddressOf());
        if (!out) return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        dwrite->CreateEllipsisTrimmingSign(out.Get(), ellipsis.GetAddressOf());
        out->SetTrimming(&trimming, ellipsis.Get());
    };
    const wchar_t* glyph_family = L"Segoe Fluent Icons";
    {
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (SUCCEEDED(dwrite->GetSystemFontCollection(fonts.GetAddressOf())))
            fonts->FindFamilyName(glyph_family, &index, &exists);
        if (!exists) glyph_family = L"Segoe MDL2 Assets";
    }
    make(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, text_format);
    make(L"Segoe UI", 11.0f, DWRITE_FONT_WEIGHT_NORMAL, small_format);
    make(glyph_family, 15.0f, DWRITE_FONT_WEIGHT_NORMAL, glyph_format);
    make(L"Segoe UI", 13.0f, DWRITE_FONT_WEIGHT_BOLD, logo_format);
    make(L"Segoe UI", 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, key_format);
    if (glyph_format) glyph_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (logo_format) logo_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (key_format) key_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    foreground_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &Impl::OnForeground, 0,
                                      0, WINEVENT_OUTOFCONTEXT);
    bar_window = bar;
    ready = true;
    Evaluate(GetForegroundWindow());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMsgStop) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Detach();
    if (foreground_hook) UnhookWinEvent(foreground_hook);
    index_client.Stop();
    bar_window = nullptr;
    bar_rt.Reset();
    list_rt.Reset();
    if (tooltip) DestroyWindow(tooltip);
    DestroyWindow(list);
    DestroyWindow(bar);
    if (edit_font) DeleteObject(edit_font);
    if (field_brush) DeleteObject(field_brush);
    UnregisterClassW(kBarClass, instance);
    UnregisterClassW(kListClass, instance);
    t_impl = nullptr;
}

void CALLBACK DialogJump::Impl::OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG id_object, LONG id_child, DWORD,
                                             DWORD) {
    if (t_impl && id_object == OBJID_WINDOW && id_child == CHILDID_SELF) t_impl->Evaluate(hwnd);
}

void CALLBACK DialogJump::Impl::OnDialogEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG id_object, LONG id_child,
                                              DWORD, DWORD) {
    Impl* self = t_impl;
    if (!self || hwnd != self->dlg.dialog || id_object != OBJID_WINDOW || id_child != CHILDID_SELF) return;
    if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) self->Detach();
    else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (IsIconic(hwnd)) self->Detach();
        else self->Place();
    }
}

void DialogJump::Impl::Evaluate(HWND foreground) {
    if (!foreground) return;
    if (foreground == bar || foreground == list || foreground == tooltip) return;
    if (dlg.dialog && foreground == dlg.dialog) {
        if (!IsWindowVisible(bar)) Attach(dlg);
        return;
    }
    DialogInfo info;
    if (ProbeDialog(foreground, info)) {
        Attach(info);
        return;
    }
    Detach();
    // A dialog's controls can still be on the way when it takes the
    // foreground: look again a few times.
    if (ClassOf(foreground) == L"#32770") {
        probe_target = foreground;
        probe_tries = 0;
        SetTimer(bar, kTimerProbe, 120, nullptr);
    }
}

void DialogJump::Impl::Attach(const DialogInfo& info) {
    const bool same = dlg.dialog == info.dialog;
    if (!same) {
        Detach();
        dlg = info;
        dialog_hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE, nullptr, &Impl::OnDialogEvent,
                                      info.pid, info.tid, WINEVENT_OUTOFCONTEXT);
        SetWindowTextW(edit, L"");
        results.clear();
    }
    KillTimer(bar, kTimerProbe);
    if (const HWND service_window = service.load()) PostMessageW(service_window, context_message.load(), 0, 0);
    TakeContext();
    if (!hotkey) hotkey = RegisterHotKey(bar, kHotkeyId, MOD_CONTROL | MOD_NOREPEAT, 'G') != FALSE;
    UpdatePalette();
    Place();
    showing = true;
}

void DialogJump::Impl::Detach() {
    KillTimer(bar, kTimerAddress);
    if (dialog_hook) {
        UnhookWinEvent(dialog_hook);
        dialog_hook = nullptr;
    }
    if (hotkey) {
        UnregisterHotKey(bar, kHotkeyId);
        hotkey = false;
    }
    HideList();
    if (bar && IsWindowVisible(bar)) ShowWindow(bar, SW_HIDE);
    showing = false;
    if (dlg.dialog) {
        dlg = {};
        if (index_started) index_client.CancelSession(kSession);
    }
}

// --- layout and paint ---------------------------------------------------------

void DialogJump::Impl::TakeContext() {
    {
        std::lock_guard lock(mu);
        ctx = shared_context;
    }
    ctx.current = PlainPath(ctx.current);
    for (auto& folder : ctx.folders) folder.path = PlainPath(folder.path);
}

void DialogJump::Impl::UpdatePalette() {
    if (ctx.dark) {
        pal = {Rgb(0x2B2B2F), Rgb(0xFFFFFF, 0.10f), Rgb(0x1E1E22), Rgb(0xFFFFFF, 0.12f), Rgb(0xF2F2F4),
               Rgb(0xA0A0A8), Rgb(0xFFFFFF, 0.08f), ctx.accent, Rgb(0xE8B44A)};
    } else {
        pal = {Rgb(0xFBFBFC), Rgb(0x000000, 0.10f), Rgb(0xFFFFFF), Rgb(0x000000, 0.14f), Rgb(0x1B1B1F),
               Rgb(0x6B6B74), Rgb(0x000000, 0.05f), ctx.accent, Rgb(0xDDA22F)};
    }
    if (field_brush) DeleteObject(field_brush);
    field_brush = CreateSolidBrush(ToColorRef(pal.field));
    const DWORD border = ToColorRef(pal.border);
    DwmSetWindowAttribute(bar, 34, &border, sizeof(border));  // DWMWA_BORDER_COLOR
    DwmSetWindowAttribute(list, 34, &border, sizeof(border));
}

void DialogJump::Impl::Place() {
    if (!dlg.dialog || !IsWindow(dlg.dialog)) return;
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(dlg.dialog, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        GetWindowRect(dlg.dialog, &frame);
    const HMONITOR monitor = MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST);
    UINT dpi_x = 96, dpi_y = 96;
    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const float new_scale = static_cast<float>(dpi_x) / 96.0f;
    const bool rescaled = std::fabs(new_scale - scale) > 0.001f || !edit_font;
    scale = new_scale;
    const int height = static_cast<int>(std::lround(kBarHeight * scale));
    const int gap = static_cast<int>(std::lround(kGap * scale));
    const int width = frame.right - frame.left;
    int y = frame.bottom + gap;
    list_below = true;
    if (y + height > mi.rcWork.bottom) {
        if (frame.top - gap - height >= mi.rcWork.top) {
            y = frame.top - gap - height;
            list_below = false;
        } else {
            y = mi.rcWork.bottom - height;
            list_below = false;
        }
    }
    if (rescaled) {
        if (edit_font) DeleteObject(edit_font);
        edit_font = CreateFontW(-static_cast<int>(std::lround(13.0f * scale)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), TRUE);
    }
    SetWindowPos(bar, HWND_TOPMOST, frame.left, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    width_dip = static_cast<float>(width) / scale;
    if (!bar_rt) {
        d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                    D2D1::HwndRenderTargetProperties(bar, D2D1::SizeU(static_cast<UINT32>(width),
                                                                                       static_cast<UINT32>(height))),
                                    bar_rt.GetAddressOf());
    } else {
        bar_rt->Resize(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)));
    }
    if (bar_rt) bar_rt->SetDpi(96.0f * scale, 96.0f * scale);
    Layout();
    InvalidateRect(bar, nullptr, FALSE);
    if (IsWindowVisible(list)) ShowList();
}

float DialogJump::Impl::TextWidth(const std::wstring& text, IDWriteTextFormat* format, float max_width) {
    ComPtr<IDWriteTextLayout> layout;
    if (!format || FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0f,
                                                   40.0f, layout.GetAddressOf())))
        return max_width;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::min(std::ceil(metrics.widthIncludingTrailingWhitespace), max_width);
}

std::vector<size_t> DialogJump::Impl::OpenFolders() const {
    std::vector<size_t> open;
    for (size_t i = 0; i < ctx.folders.size(); ++i)
        if (ctx.folders[i].open) open.push_back(i);
    // The current folder first.
    std::stable_partition(open.begin(), open.end(),
                          [&](size_t i) { return SamePath(ctx.folders[i].path, ctx.current); });
    return open;
}

void DialogJump::Impl::Layout() {
    const float w = width_dip;
    logo_rect = D2D1::RectF(10, 9, 32, 31);
    const float field_w = std::clamp(w * 0.30f, 170.0f, 250.0f);
    field_rect = D2D1::RectF(40, 6, 40 + field_w, 34);
    more_rect = D2D1::RectF(w - 36, 6, w - 8, 34);
    float right = more_rect.left - 4;
    current_rect = {};
    if (!ctx.current.empty()) {
        const float label_w = TextWidth(l10n::Get(StringId::DialogJumpCurrent), text_format.Get(), 120);
        const float cw = 10 + 18 + 6 + label_w + 8 + 46 + 6;
        if (right - cw > field_rect.right + 60) {
            current_rect = D2D1::RectF(right - cw, 6, right, 34);
            right = current_rect.left - 4;
        }
    }
    chips.clear();
    float x = field_rect.right + 8;
    for (size_t index : OpenFolders()) {
        if (chips.size() >= kMaxChips) break;
        const auto& folder = ctx.folders[index];
        const bool current = SamePath(folder.path, ctx.current);
        const std::wstring name = folder.label.empty() ? Leaf(folder.path) : folder.label;
        const float text_w = TextWidth(name, text_format.Get(), 132);
        const float chip_w = 8 + (current ? 10.0f : 0.0f) + 18 + 6 + text_w + 10;
        if (x + chip_w > right) {
            if (chips.empty() && right - x > 80) chips.push_back({D2D1::RectF(x, 6, right, 34), index, current});
            break;
        }
        chips.push_back({D2D1::RectF(x, 6, x + chip_w, 34), index, current});
        x += chip_w + 4;
    }
    none_rect = chips.empty() && OpenFolders().empty() ? D2D1::RectF(field_rect.right + 12, 6, right, 34) : D2D1::RectF();
    const int pad = static_cast<int>(std::lround(28 * scale));
    const int edit_h = static_cast<int>(std::lround(18 * scale));
    const int edit_y = static_cast<int>(std::lround(20 * scale)) - edit_h / 2;
    MoveWindow(edit, static_cast<int>(std::lround(field_rect.left * scale)) + pad, edit_y,
               static_cast<int>(std::lround((field_rect.right - field_rect.left - 34) * scale)), edit_h, TRUE);
    UpdateTooltips();
}

void DialogJump::Impl::UpdateTooltips() {
    if (!tooltip) return;
    while (SendMessageW(tooltip, TTM_GETTOOLCOUNT, 0, 0) > 0) {
        TTTOOLINFOW info{sizeof(info)};
        if (!SendMessageW(tooltip, TTM_ENUMTOOLSW, 0, reinterpret_cast<LPARAM>(&info))) break;
        SendMessageW(tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
    auto add = [&](UINT_PTR id, const D2D1_RECT_F& r, const std::wstring& text) {
        TTTOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_SUBCLASS;
        info.hwnd = bar;
        info.uId = id;
        info.rect = {static_cast<LONG>(r.left * scale), static_cast<LONG>(r.top * scale),
                     static_cast<LONG>(r.right * scale), static_cast<LONG>(r.bottom * scale)};
        info.lpszText = const_cast<wchar_t*>(text.c_str());
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    };
    for (size_t i = 0; i < chips.size(); ++i) add(1 + i, chips[i].rect, ctx.folders[chips[i].folder].path);
    if (current_rect.right > current_rect.left) add(20, current_rect, ctx.current);
    add(21, more_rect, l10n::Get(StringId::DialogJumpMore));
}

void DialogJump::Impl::DrawText(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const std::wstring& text,
                                IDWriteTextFormat* format, D2D1_RECT_F rect, D2D1_COLOR_F color) {
    if (!format || text.empty()) return;
    brush->SetColor(color);
    rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DialogJump::Impl::PaintBar() {
    PAINTSTRUCT ps;
    BeginPaint(bar, &ps);
    EndPaint(bar, &ps);
    if (!bar_rt) return;
    ID2D1HwndRenderTarget* rt = bar_rt.Get();
    rt->BeginDraw();
    rt->Clear(pal.back);
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(pal.text, brush.GetAddressOf());
    if (!brush) {
        rt->EndDraw();
        return;
    }
    // Logo.
    brush->SetColor(pal.accent);
    rt->FillRoundedRectangle(D2D1::RoundedRect(logo_rect, 6, 6), brush.Get());
    DrawText(rt, brush.Get(), L"P", logo_format.Get(), logo_rect, Rgb(0xFFFFFF));
    // Search field.
    const bool focused = GetFocus() == edit;
    brush->SetColor(pal.field);
    rt->FillRoundedRectangle(D2D1::RoundedRect(field_rect, 6, 6), brush.Get());
    brush->SetColor(focused ? pal.accent : pal.field_border);
    const D2D1_RECT_F field_line{field_rect.left + 0.5f, field_rect.top + 0.5f, field_rect.right - 0.5f,
                                 field_rect.bottom - 0.5f};
    rt->DrawRoundedRectangle(D2D1::RoundedRect(field_line, 6, 6), brush.Get(), focused ? 1.5f : 1.0f);
    DrawText(rt, brush.Get(), L"\xE721", glyph_format.Get(),
             D2D1::RectF(field_rect.left + 4, field_rect.top, field_rect.left + 26, field_rect.bottom), pal.muted);
    // Chips.
    for (size_t i = 0; i < chips.size(); ++i) {
        const auto& chip = chips[i];
        const auto& folder = ctx.folders[chip.folder];
        if (hover == Hit::Chip && hover_chip == i) {
            brush->SetColor(pal.hover);
            rt->FillRoundedRectangle(D2D1::RoundedRect(chip.rect, 6, 6), brush.Get());
        }
        float x = chip.rect.left + 8;
        if (chip.current) {
            brush->SetColor(pal.accent);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 3, 20), 3, 3), brush.Get());
            x += 10;
        }
        DrawText(rt, brush.Get(), L"\xE8B7", glyph_format.Get(), D2D1::RectF(x, 6, x + 18, 34), pal.folder);
        x += 24;
        DrawText(rt, brush.Get(), folder.label.empty() ? Leaf(folder.path) : folder.label, text_format.Get(),
                 D2D1::RectF(x, 6, chip.rect.right - 8, 34), pal.text);
    }
    if (none_rect.right > none_rect.left)
        DrawText(rt, brush.Get(), l10n::Get(StringId::DialogJumpNone), text_format.Get(), none_rect, pal.muted);
    // Current folder (Ctrl+G).
    if (current_rect.right > current_rect.left) {
        if (hover == Hit::Current) {
            brush->SetColor(pal.hover);
            rt->FillRoundedRectangle(D2D1::RoundedRect(current_rect, 6, 6), brush.Get());
        }
        float x = current_rect.left + 10;
        DrawText(rt, brush.Get(), L"\xE81D", glyph_format.Get(), D2D1::RectF(x, 6, x + 18, 34), pal.accent);
        x += 24;
        const float key_left = current_rect.right - 6 - 46;
        DrawText(rt, brush.Get(), l10n::Get(StringId::DialogJumpCurrent), text_format.Get(),
                 D2D1::RectF(x, 6, key_left - 4, 34), pal.text);
        const D2D1_RECT_F key{key_left, 11, current_rect.right - 6, 29};
        brush->SetColor(pal.field_border);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(key.left + 0.5f, key.top + 0.5f, key.right - 0.5f,
                                                               key.bottom - 0.5f), 4, 4),
                                 brush.Get(), 1.0f);
        DrawText(rt, brush.Get(), L"Ctrl+G", key_format.Get(), key, pal.muted);
    }
    // More.
    if (hover == Hit::More) {
        brush->SetColor(pal.hover);
        rt->FillRoundedRectangle(D2D1::RoundedRect(more_rect, 6, 6), brush.Get());
    }
    DrawText(rt, brush.Get(), L"\xE712", glyph_format.Get(), more_rect, pal.muted);
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) bar_rt.Reset();
}

void DialogJump::Impl::ShowList() {
    if (query.empty() || !dlg.dialog) {
        HideList();
        return;
    }
    RECT bar_rect{};
    GetWindowRect(bar, &bar_rect);
    const size_t rows = std::max<size_t>(1, std::min(results.size(), kMaxRows));
    const int height = static_cast<int>(std::lround((static_cast<float>(rows) * kRowHeight + 8) * scale));
    const int left = bar_rect.left + static_cast<int>(std::lround(field_rect.left * scale));
    const int width = std::min(static_cast<int>(std::lround(520 * scale)), static_cast<int>(bar_rect.right - left));
    const int gap = static_cast<int>(std::lround(4 * scale));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST), &mi);
    int y = bar_rect.bottom + gap;
    if (!list_below || y + height > mi.rcWork.bottom) y = bar_rect.top - gap - height;
    SetWindowPos(list, HWND_TOPMOST, left, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!list_rt) {
        d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                    D2D1::HwndRenderTargetProperties(list, D2D1::SizeU(static_cast<UINT32>(width),
                                                                                        static_cast<UINT32>(height))),
                                    list_rt.GetAddressOf());
    } else {
        list_rt->Resize(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)));
    }
    if (list_rt) list_rt->SetDpi(96.0f * scale, 96.0f * scale);
    InvalidateRect(list, nullptr, FALSE);
}

void DialogJump::Impl::HideList() {
    if (list && IsWindowVisible(list)) ShowWindow(list, SW_HIDE);
    list_hover = -1;
}

void DialogJump::Impl::PaintList() {
    PAINTSTRUCT ps;
    BeginPaint(list, &ps);
    EndPaint(list, &ps);
    if (!list_rt) return;
    ID2D1HwndRenderTarget* rt = list_rt.Get();
    const D2D1_SIZE_F size = rt->GetSize();
    rt->BeginDraw();
    rt->Clear(pal.back);
    ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(pal.text, brush.GetAddressOf());
    if (!brush) {
        rt->EndDraw();
        return;
    }
    if (results.empty()) {
        const auto& text = l10n::Get(searching ? StringId::DialogJumpSearching : StringId::DialogJumpNoMatch);
        DrawText(rt, brush.Get(), text, text_format.Get(), D2D1::RectF(16, 4, size.width - 12, 4 + kRowHeight),
                 pal.muted);
    }
    for (size_t i = 0; i < results.size() && i < kMaxRows; ++i) {
        const auto& r = results[i];
        const float top = 4 + static_cast<float>(i) * kRowHeight;
        const D2D1_RECT_F row{4, top, size.width - 4, top + kRowHeight};
        if (static_cast<int>(i) == selected || static_cast<int>(i) == list_hover) {
            brush->SetColor(static_cast<int>(i) == selected ? WithAlpha(pal.accent, ctx.dark ? 0.28f : 0.14f) : pal.hover);
            rt->FillRoundedRectangle(D2D1::RoundedRect(row, 6, 6), brush.Get());
        }
        if (r.open) {
            brush->SetColor(pal.accent);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(12, top + kRowHeight / 2), 3, 3), brush.Get());
        }
        DrawText(rt, brush.Get(), r.dir ? L"\xE8B7" : L"\xE7C3", glyph_format.Get(),
                 D2D1::RectF(18, top, 40, top + kRowHeight), r.dir ? pal.folder : pal.muted);
        DrawText(rt, brush.Get(), r.name, text_format.Get(), D2D1::RectF(48, top + 4, size.width - 14, top + 24),
                 pal.text);
        DrawText(rt, brush.Get(), r.dir ? r.path : Parent(r.path), small_format.Get(),
                 D2D1::RectF(48, top + 23, size.width - 14, top + 40), pal.muted);
    }
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) list_rt.Reset();
}

// --- search -------------------------------------------------------------------

void DialogJump::Impl::OnSearchChanged() {
    wchar_t buffer[512]{};
    GetWindowTextW(edit, buffer, 512);
    query = buffer;
    while (!query.empty() && query.back() == L' ') query.pop_back();
    results.clear();
    known_count = 0;
    selected = -1;
    KillTimer(bar, kTimerSearch);
    if (index_started) index_client.CancelSession(kSession);
    searching = false;
    if (query.empty()) {
        HideList();
        return;
    }
    std::vector<JumpCandidate> candidates;
    candidates.reserve(ctx.folders.size());
    for (const auto& folder : ctx.folders) candidates.push_back({folder.path, folder.label});
    for (size_t index : RankJumpFolders(candidates, query, ctx.pinyin, 5)) {
        const auto& folder = ctx.folders[index];
        results.push_back({folder.path, folder.label.empty() ? Leaf(folder.path) : folder.label, true, folder.open});
    }
    known_count = results.size();
    selected = results.empty() ? -1 : 0;
    if (ctx.index) {
        searching = true;
        SetTimer(bar, kTimerSearch, 90, nullptr);
    }
    ShowList();
}

void DialogJump::Impl::StartIndexSearch() {
    KillTimer(bar, kTimerSearch);
    if (query.empty()) return;
    if (!index_started) {
        index_client.Start(bar, kMsgIndexStatus, kMsgIndexResult);
        index_started = true;
    }
    index::Query q;
    q.session_id = kSession;
    q.needle = ctx.pinyin ? query : L"nopinyin: " + query;
    q.folders_only = dlg.save || dlg.folder_picker;
    q.limit = 24;
    index_client.SearchAsync(q, ++search_id);
}

void DialogJump::Impl::OnIndexResult(uint32_t id) {
    index::SearchResult result;
    if (!index_client.TakeResult(id, result) || id != search_id) return;
    searching = false;
    results.resize(known_count);
    std::vector<Result> files;
    for (const auto& hit : result.hits) {
        bool known = false;
        for (size_t i = 0; i < known_count && !known; ++i) known = SamePath(results[i].path, hit.path);
        if (known) continue;
        Result r{hit.path, hit.name.empty() ? Leaf(hit.path) : hit.name, hit.is_dir, false};
        if (hit.is_dir) results.push_back(std::move(r));
        else if (!dlg.save && !dlg.folder_picker) files.push_back(std::move(r));
    }
    for (auto& file : files) results.push_back(std::move(file));
    if (results.size() > kMaxRows) results.resize(kMaxRows);
    if (selected < 0 && !results.empty()) selected = 0;
    ShowList();
}

void DialogJump::Impl::Pick(size_t row) {
    if (row >= results.size()) return;
    const Result picked = results[row];
    SetWindowTextW(edit, L"");
    Navigate(picked.path, picked.dir);
}

// --- driving the dialog -------------------------------------------------------

void DialogJump::Impl::ReturnToDialog() {
    if (dlg.dialog && GetForegroundWindow() == bar) SetForegroundWindow(dlg.dialog);
}

void DialogJump::Impl::Navigate(const std::wstring& path, bool dir) {
    HideList();
    if (path.empty() || !dlg.dialog || !IsWindow(dlg.dialog)) return;
    KillTimer(bar, kTimerRestore);
    if (dir && (dlg.folder_picker || !dlg.name_edit)) {
        NavigateAddressBar(path);
        return;
    }
    // The file name box: a folder path there plus OK moves the dialog; the
    // name that was typed is put back once it has.
    const std::wstring typed = TextOf(dlg.name_edit);
    set_text = dir ? FolderInput(path) : path;
    if (!SetText(dlg.name_edit, set_text)) return;
    PostMessageW(dlg.dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(dlg.ok));
    restore_text = dir && typed.find_first_of(L"\\/:") == std::wstring::npos ? typed : std::wstring();
    restore_edit = dlg.name_edit;
    restore_ticks = 0;
    if (dir) SetTimer(bar, kTimerRestore, 90, nullptr);
    ReturnToDialog();
}

static bool ModifiersDown() {
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(key) & 0x8000) return true;
    return false;
}

void DialogJump::Impl::NavigateAddressBar(const std::wstring& path) {
    // Alt+D puts the dialog's address bar into edit mode (once Ctrl+G's Ctrl
    // is up); the path goes in once the bar has the focus.
    if (GetForegroundWindow() != dlg.dialog) SetForegroundWindow(dlg.dialog);
    address_path = path;
    address_ticks = 0;
    address_keys_sent = false;
    SetTimer(bar, kTimerAddress, 30, nullptr);
}

void DialogJump::Impl::SendAddressKeys() {
    address_keys_sent = true;
    INPUT keys[4]{};
    for (auto& key : keys) key.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_MENU;
    keys[1].ki.wVk = 'D';
    keys[2].ki.wVk = 'D';
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_MENU;
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (GetForegroundWindow() == dlg.dialog) SendInput(4, keys, sizeof(INPUT));
}

void DialogJump::Impl::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    std::vector<size_t> items;
    for (size_t i = 0; i < ctx.folders.size() && items.size() < kMaxMenu; ++i) {
        const auto& folder = ctx.folders[i];
        std::wstring text = folder.label.empty() ? Leaf(folder.path) : folder.label;
        std::wstring where = Parent(folder.path);
        if (where.size() > 42) where = L"\x2026" + where.substr(where.size() - 41);
        if (!where.empty()) text += L"\t" + where;
        for (size_t at = text.find(L'&'); at != std::wstring::npos; at = text.find(L'&', at + 2)) text.insert(at, 1, L'&');
        AppendMenuW(menu, MF_STRING | (SamePath(folder.path, ctx.current) ? MF_CHECKED : 0u),
                    kMenuBase + static_cast<UINT>(items.size()), text.c_str());
        items.push_back(i);
    }
    if (items.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 1, l10n::Get(StringId::DialogJumpNone).c_str());
    POINT pt{static_cast<LONG>(more_rect.right * scale), static_cast<LONG>(more_rect.bottom * scale)};
    ClientToScreen(bar, &pt);
    SetForegroundWindow(bar);
    const UINT picked = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RETURNCMD |
                                                                   TPM_NONOTIFY, pt.x, pt.y, 0, bar, nullptr));
    DestroyMenu(menu);
    if (picked >= kMenuBase && picked - kMenuBase < items.size())
        Navigate(ctx.folders[items[picked - kMenuBase]].path, true);
    else
        ReturnToDialog();
}

void DialogJump::Impl::FocusEdit() {
    if (!showing || !dlg.dialog) return;
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD self = GetCurrentThreadId();
    const bool attach = foreground_thread && foreground_thread != self &&
                        AttachThreadInput(self, foreground_thread, TRUE) != FALSE;
    SetForegroundWindow(bar);
    SetFocus(edit);
    if (attach) AttachThreadInput(self, foreground_thread, FALSE);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    InvalidateRect(bar, nullptr, FALSE);
}

// --- window procedures --------------------------------------------------------

LRESULT CALLBACK DialogJump::Impl::BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return t_impl ? t_impl->HandleBar(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK DialogJump::Impl::ListProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return t_impl ? t_impl->HandleList(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK DialogJump::Impl::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Impl* self = t_impl;
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_KEYDOWN) {
        const int count = static_cast<int>(std::min(self->results.size(), kMaxRows));
        switch (wp) {
        case VK_DOWN:
            if (count) self->selected = (self->selected + 1) % count;
            InvalidateRect(self->list, nullptr, FALSE);
            return 0;
        case VK_UP:
            if (count) self->selected = (self->selected + count - 1) % count;
            InvalidateRect(self->list, nullptr, FALSE);
            return 0;
        case VK_RETURN:
            if (self->selected >= 0) self->Pick(static_cast<size_t>(self->selected));
            return 0;
        case VK_ESCAPE:
            if (GetWindowTextLengthW(hwnd) > 0) SetWindowTextW(hwnd, L"");
            else self->ReturnToDialog();
            return 0;
        default: break;
        }
    } else if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == L'\n')) {
        return 0;  // no beep
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(self->bar, nullptr, FALSE);
        if (msg == WM_KILLFOCUS) self->HideList();
    }
    return CallWindowProcW(self->edit_proc, hwnd, msg, wp, lp);
}

LRESULT DialogJump::Impl::HandleBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        PaintBar();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE: {
        // Only the search box takes the focus; chips work without stealing it.
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const float x = static_cast<float>(pt.x) / scale, y = static_cast<float>(pt.y) / scale;
        return Contains(field_rect, x, y) ? MA_ACTIVATE : MA_NOACTIVATE;
    }
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
        Hit hit = Hit::None;
        size_t chip_index = 0;
        for (size_t i = 0; i < chips.size(); ++i)
            if (Contains(chips[i].rect, x, y)) {
                hit = Hit::Chip;
                chip_index = i;
            }
        if (Contains(current_rect, x, y)) hit = Hit::Current;
        if (Contains(more_rect, x, y)) hit = Hit::More;
        if (hit != hover || chip_index != hover_chip) {
            hover = hit;
            hover_chip = chip_index;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&tme) != FALSE;
        }
        SetCursor(LoadCursorW(nullptr, hit == Hit::None ? IDC_ARROW : IDC_HAND));
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        if (hover != Hit::None) {
            hover = Hit::None;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
        if (Contains(field_rect, x, y)) FocusEdit();
        return 0;
    }
    case WM_LBUTTONUP: {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / scale, y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
        for (const auto& chip : chips)
            if (Contains(chip.rect, x, y)) {
                Navigate(ctx.folders[chip.folder].path, true);
                return 0;
            }
        if (Contains(current_rect, x, y)) Navigate(ctx.current, true);
        else if (Contains(more_rect, x, y)) ShowMenu();
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, ToColorRef(pal.text));
        SetBkColor(dc, ToColorRef(pal.field));
        return reinterpret_cast<LRESULT>(field_brush);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kEditId && HIWORD(wp) == EN_CHANGE) OnSearchChanged();
        return 0;
    case WM_HOTKEY:
        if (wp == kHotkeyId && !ctx.current.empty()) Navigate(ctx.current, true);
        return 0;
    case WM_TIMER:
        if (wp == kTimerSearch) {
            StartIndexSearch();
        } else if (wp == kTimerProbe) {
            DialogInfo info;
            if (GetForegroundWindow() == probe_target && ProbeDialog(probe_target, info)) Attach(info);
            else if (++probe_tries >= 6 || GetForegroundWindow() != probe_target) KillTimer(hwnd, kTimerProbe);
        } else if (wp == kTimerRestore) {
            // Put the typed name back once the dialog has taken the path.
            const std::wstring now = IsWindow(restore_edit) ? TextOf(restore_edit) : std::wstring();
            if (!IsWindow(restore_edit) || (!now.empty() && now != set_text)) {
                KillTimer(hwnd, kTimerRestore);
            } else if (++restore_ticks >= 2) {
                KillTimer(hwnd, kTimerRestore);
                SetText(restore_edit, restore_text);
                if (!restore_text.empty()) {
                    DWORD_PTR ignored = 0;
                    SendMessageTimeoutW(restore_edit, EM_SETSEL, 0, -1, SMTO_ABORTIFHUNG, 300, &ignored);
                }
            }
        } else if (wp == kTimerAddress) {
            if (!address_keys_sent) {
                if (ModifiersDown() && ++address_ticks < 40) return 0;
                address_ticks = 0;
                SendAddressKeys();
                return 0;
            }
            GUITHREADINFO gti{sizeof(gti)};
            const HWND focus = dlg.dialog && GetGUIThreadInfo(dlg.tid, &gti) ? gti.hwndFocus : nullptr;
            if (focus && focus != dlg.name_edit && IsChild(dlg.dialog, focus) && ClassOf(focus) == L"Edit") {
                KillTimer(hwnd, kTimerAddress);
                SetText(focus, address_path);
                INPUT keys[2]{};
                keys[0].type = keys[1].type = INPUT_KEYBOARD;
                keys[0].ki.wVk = keys[1].ki.wVk = VK_RETURN;
                keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
                if (GetForegroundWindow() == dlg.dialog) SendInput(2, keys, sizeof(INPUT));
            } else if (++address_ticks > 12) {
                KillTimer(hwnd, kTimerAddress);
            }
        }
        return 0;
    case kMsgContext: {
        TakeContext();
        UpdatePalette();
        if (showing) {
            Layout();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (!query.empty()) OnSearchChanged();
        }
        return 0;
    }
    case kMsgFocus:
        FocusEdit();
        return 0;
    case kMsgIndexResult:
        OnIndexResult(static_cast<uint32_t>(wp));
        return 0;
    case kMsgIndexStatus:
        return 0;
    case WM_DPICHANGED:
        return 0;  // Place() sizes from the dialog's monitor
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT DialogJump::Impl::HandleList(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        PaintList();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
        const int row = y < 4 ? -1 : static_cast<int>((y - 4) / kRowHeight);
        const int hovered = row >= 0 && row < static_cast<int>(std::min(results.size(), kMaxRows)) ? row : -1;
        if (hovered != list_hover) {
            list_hover = hovered;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / scale;
        if (y >= 4) Pick(static_cast<size_t>((y - 4) / kRowHeight));
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- public -------------------------------------------------------------------

DialogJump::DialogJump() : impl_(std::make_shared<Impl>()) {}
DialogJump::~DialogJump() { Stop(); }

void DialogJump::Update(HWND service, UINT context_message, bool enabled) {
    impl_->service = service;
    impl_->context_message = context_message;
    if (!enabled) {
        Stop();
        return;
    }
    if (thread_.joinable()) return;
    impl_->ready = false;
    thread_ = std::thread([impl = impl_] { impl->Run(); });
}

void DialogJump::Stop() {
    if (!thread_.joinable()) return;
    // Run() may still be creating its windows.
    for (int i = 0; i < 200 && !impl_->ready.load(); ++i) Sleep(5);
    PostThreadMessageW(GetThreadId(thread_.native_handle()), kMsgStop, 0, 0);
    thread_.join();
    impl_->showing = false;
}

void DialogJump::SetContext(DialogJumpContext context) {
    {
        std::lock_guard lock(impl_->mu);
        impl_->shared_context = std::move(context);
    }
    if (const HWND bar = impl_->bar_window.load()) PostMessageW(bar, kMsgContext, 0, 0);
}

bool DialogJump::FocusSearch() {
    const HWND bar = impl_->bar_window.load();
    if (!bar || !impl_->showing.load()) return false;
    PostMessageW(bar, kMsgFocus, 0, 0);
    return true;
}
} // namespace pulse::app
