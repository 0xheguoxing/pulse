#include "../ui/edit_host.h"
#include "../ui/FluentTokens.h"
// app_hosted_edit.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "address_bar_command.h"
#include "folder_alias.h"
#include "keymap.h"
#include "tab_shortcuts.h"
#include "window_overview_ui.h"
#include "window_switcher.h"
#include <imm.h>
#include "../ui/address_search_layout.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>

using namespace pulse;

namespace pulse {
std::wstring FormatAddressPath(const std::wstring& text) {
    std::wstring t = text;
    if (t.empty()) return L"C:\\";
    auto start = t.find_first_not_of(L" \"");
    auto end = t.find_last_not_of(L" \"");
    if (start == std::wstring::npos) return L"C:\\";
    t = t.substr(start, end - start + 1);
    if (t.size() == 2 && t[1] == L':') t += L'\\';
    return t;
}

// The editor shows This PC by its display name, so committing it unchanged
// (or typing it) must reach This PC, not a folder of that name resolved
// against the working directory (it opened "C:\Program Files\Pulse\此电脑").
static bool IsThisPcDisplayName(std::wstring_view text) {
    while (!text.empty() && (iswspace(text.front()) || text.front() == L'"')) text.remove_prefix(1);
    while (!text.empty() && (iswspace(text.back()) || text.back() == L'"' || text.back() == L'\\'))
        text.remove_suffix(1);
    if (text.empty()) return false;
    const auto same = [&](std::wstring_view name) {
        return !name.empty() && CompareStringOrdinal(text.data(), static_cast<int>(text.size()),
            name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    };
    return same(l10n::Get(l10n::StringId::ThisPc)) || same(L"This PC") ||
        same(L"\u6B64\u7535\u8111") || same(L"\u672C\u6A5F");
}

std::wstring AddressNavigationTarget(const std::wstring& text, bool* shortcut) {
    // 别名 jump: "发布包" or "发布包\\v1.0.56" opens the aliased folder.
    if (std::wstring aliased; app::AliasStore::Instance().ResolveTyped(text, aliased)) {
        if (shortcut) *shortcut = true;
        return FormatAddressPath(aliased);
    }
    if (IsThisPcDisplayName(text)) {
        if (shortcut) *shortcut = true;
        return std::wstring();
    }
    const app::AddressShortcut resolved = app::ResolveAddressShortcut(text);
    if (shortcut) *shortcut = resolved.resolved;
    if (!resolved.resolved) return FormatAddressPath(text);
    if (resolved.path.empty() || resolved.path.starts_with(L"pulse:")) return resolved.path;
    return FormatAddressPath(resolved.path);
}

LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
void EnsureEditVisuals(AppState& s);

static bool EraseHostedEditBackground(HWND hwnd, WPARAM wParam, AppState* s) {
    if (!s) return false;
    EnsureEditVisuals(*s);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(reinterpret_cast<HDC>(wParam), &rc, ui::EditBackBrush(s->editBrush));
    return true;
}

void PlaceHostedEdit(HWND hwnd, HWND owner, const D2D1_RECT_F& cell, float scale,
                            int left_margin_dip, int right_margin_dip) {
    POINT pt{ static_cast<int>(std::lround(cell.left)),
              static_cast<int>(std::lround(cell.top)) };
    (void)owner; // cell is already in the parent client coordinate space.
    const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left)));
    const int cellH = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    int lineH = cellH;
    if (font) {
        HDC hdc = GetDC(hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        lineH = std::max(1, static_cast<int>(tm.tmHeight));
    }
    lineH = std::min(lineH, cellH);
    const int y = pt.y + std::max(0, (cellH - lineH) / 2);
    const int left = std::max(0, static_cast<int>(std::lround(static_cast<float>(left_margin_dip) * scale)));
    const int right = std::max(0, static_cast<int>(std::lround(static_cast<float>(right_margin_dip) * scale)));
    const LPARAM margins=MAKELPARAM(left,right);
    // Layout runs after every parent frame. Reapplying identical margins enters
    // the LumaText redraw guard, briefly hiding the redirected edit surface.
    if (SendMessageW(hwnd,EM_GETMARGINS,0,0)!=margins)
        SendMessageW(hwnd,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,margins);
    // Layout also runs after every parent frame; an unconditional move still
    // sends WM_WINDOWPOSCHANGING and reports a layout change to the IME.
    RECT current{};
    GetWindowRect(hwnd, &current);
    MapWindowPoints(nullptr, GetParent(hwnd), reinterpret_cast<POINT*>(&current), 2);
    const bool placed = current.left == pt.x && current.top == y &&
        current.right - current.left == w && current.bottom - current.top == lineH;
    const bool topmost = GetWindow(hwnd, GW_HWNDPREV) == nullptr;
    if (placed && topmost) return;
    SetWindowPos(hwnd, topmost ? nullptr : HWND_TOP, pt.x, y, w, lineH,
                 SWP_NOACTIVATE | (topmost ? SWP_NOZORDER : 0u));
}

HWND CreateHostedEdit(AppState& s, SUBCLASSPROC proc) {
    EnsureEditVisuals(s);
    HWND hwnd = ui::CreateRedirectedChildEdit(s.hwnd);
    if (!hwnd) return nullptr;
    SetWindowTheme(hwnd, L"", L"");
    // All hosted editors use redirected surfaces: uploaded
    // layered bitmaps can disappear under the main composition surface.
    // Their procedures suppress native drawing and present LumaText to the DC.
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)s.editFont, TRUE);
    SetWindowSubclass(hwnd, proc, 1, reinterpret_cast<DWORD_PTR>(&s));
    return hwnd;
}

void LayoutAddressEditor(AppState& s) {
    if (!s.hwndAddressEdit || !s.hwnd) return;
    SyncSearchBarWidth(s);
    D2D1_RECT_F addr = s.addressSearching ? s.renderer.SearchBarRect((float)s.compositor.Width()) : s.renderer.AddressBarRect((float)s.compositor.Width());
    if (s.addressSearching) {
        PlaceHostedEdit(s.hwndAddressEdit, s.hwnd, ui::LayoutAddressSearch(addr, s.scale).input,
                        s.scale, 0, 0);
        return;
    }
    const float insetX = 10.0f * s.scale;
    const float insetY = 2.0f * s.scale;
    PlaceHostedEdit(s.hwndAddressEdit, s.hwnd,
        D2D1::RectF(addr.left + insetX, addr.top + insetY,
                    addr.right - insetX, addr.bottom - insetY),
        s.scale, 0, 0);
}

void EnsureEditVisuals(AppState& s) {
    if (!s.editFont) s.editFont = ui::typography::CreateEditFont(s.scale);
    if (!s.editBrush) {
        s.editBrush = CreateSolidBrush(s.darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
    }
}

void HideRenameOverlay(AppState& s, bool commit);

void ShowAddressEditor(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.addressSearching = false;
    s.addressEditing = true;
    s.addressSuggest.clear();
    s.addressSuggestSel = -1;
    // DestroyWindow synchronously sends WM_KILLFOCUS to the old editor.
    s.addressIgnoreKillFocus = true;
    if (s.hwndAddressEdit) {
        if (IsWindow(s.hwndAddressEdit)) DestroyWindow(s.hwndAddressEdit);
        s.hwndAddressEdit = nullptr;
    }
    if (!s.hwndAddressEdit) {
        s.hwndAddressEdit = CreateHostedEdit(s, AddressEditProc);
        if (!s.hwndAddressEdit) {
            s.addressEditing = false;
            s.addressIgnoreKillFocus = false;
            return;
        }
    }
    const std::wstring shown = ClipboardPath(tab->current_path);
    SendMessageW(s.hwndAddressEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L""));
    SetWindowTextW(s.hwndAddressEdit, shown.empty() ? l10n::Get(l10n::StringId::ThisPc).c_str() : shown.c_str());
    LayoutAddressEditor(s);
    ShowWindow(s.hwndAddressEdit, SW_SHOW);
    SetForegroundWindow(GetAncestor(s.hwndAddressEdit, GA_ROOT));
    SetFocus(s.hwndAddressEdit);
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Explorer starts the program in the folder being shown. Virtual places (This
// PC, search results, recycle bin) have no working directory, so they use the
// profile folder instead.
static void RunAddressBarCommand(AppState& s, const app::AddressBarCommand& command) {
    const app::Tab* tab = ActiveTab(s);
    std::wstring dir;
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        dir = ClipboardPath(tab->current_path);
    if (dir.empty()) {
        wchar_t profile[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, ARRAYSIZE(profile));
        if (n > 0 && n < ARRAYSIZE(profile)) dir.assign(profile, n);
    }
    s.ops.OpenProgramIn(app::AddressBarProgramExe(command.program), dir, command.args);
}

void HideAddressEditor(AppState& s, bool navigate) {
    if (!s.hwndAddressEdit) return;
    FlushAddressSearch(s);
    SaveAddressSearchDraft(s);
    if (navigate) {
        wchar_t buf[MAX_PATH * 4];
        GetWindowTextW(s.hwndAddressEdit, buf, ARRAYSIZE(buf));
        // cmd / powershell / pwsh / wt open a console here instead of navigating.
        const app::AddressBarCommand command = app::ParseAddressBarCommand(buf);
        if (command.program != app::AddressBarProgram::None) RunAddressBarCommand(s, command);
        else NavigateTo(s, AddressNavigationTarget(buf));
    }
    s.addressSuggest.clear();
    s.addressSuggestSel = -1;
    s.addressIgnoreKillFocus = true;
    ShowWindow(s.hwndAddressEdit, SW_HIDE);
    s.addressEditing = false;
    s.addressSearching = false;
    s.addressAnimationTick = GetTickCount64();
    if (s.hwnd) SetFocus(s.hwnd);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool DismissPathEditorOutside(AppState& s, int x, int y) {
    if (!s.addressEditing || s.addressSearching) return false;
    const float w = static_cast<float>(s.compositor.Width());
    const auto bounds = s.renderer.AddressBarRect(w);
    if (x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom) return false;
    const int row = s.renderer.AddressSuggestionAt(w, s.addressSuggest.size(), static_cast<float>(x),
                                                   static_cast<float>(y));
    if (row >= 0) {
        const std::wstring path = s.addressSuggest[static_cast<size_t>(row)].path;
        HideAddressEditor(s, false);
        NavigateTo(s, path);
        return true;
    }
    if (row == -2) return true;  // the card's padding: keep editing
    // Owner-drawn controls need not transfer keyboard focus from the native edit.
    HideAddressEditor(s, false);
    return false;
}

void UpdateAddressSuggestions(AppState& s) {
    const size_t before = s.addressSuggest.size();
    s.addressSuggest.clear();
    s.addressSuggestHeader.clear();
    s.addressSuggestSel = -1;
    const auto done = [&] { if (before || !s.addressSuggest.empty()) InvalidateRect(s.hwnd, nullptr, FALSE); };
    if (!s.addressEditing || s.addressSearching || !s.hwndAddressEdit ||
        app::AliasStore::Instance().Empty()) return done();
    const int len = GetWindowTextLengthW(s.hwndAddressEdit);
    if (len <= 0 || len > 260) return done();
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(s.hwndAddressEdit, text.data(), len + 1)));
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    // Paths, shell: and %VAR% shortcuts keep the plain editor; "别名\rest"
    // suggests the aliased folder plus the rest (pulse_alias.html).
    if (text.empty() || text.find_first_of(L":%\"") != std::wstring::npos ||
        text.front() == L'\\' || text.front() == L'/') return done();
    const size_t cut = text.find_first_of(L"\\/");
    std::wstring head = text.substr(0, cut);
    while (!head.empty() && iswspace(head.back())) head.pop_back();
    std::wstring rest = cut == std::wstring::npos ? std::wstring() : text.substr(cut);
    for (auto& c : rest) if (c == L'/') c = L'\\';
    while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
    if (head.empty()) return done();
    const auto aliases = app::AliasStore::Instance().SuggestRanked(head, 6);
    if (aliases.empty()) return done();
    // A real subfolder with that very name wins over any alias.
    const app::Tab* tab = ActiveTab(s);
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path)) {
        std::wstring child = tab->current_path;
        if (child.back() != L'\\') child += L'\\';
        child += text;
        const DWORD attrs = GetFileAttributesW(child.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            s.addressSuggest.push_back({text, ClipboardPath(child), l10n::Get(l10n::StringId::AliasSuggestSubfolder), true});
    }
    s.addressSuggestHeader = l10n::Get(s.addressSuggest.empty() ? l10n::StringId::AliasSuggestHeader
                                                                 : l10n::StringId::AliasSuggestSubfolderFirst);
    for (const auto& [rank, e] : aliases) {
        if (s.addressSuggest.size() >= 6) break;
        std::wstring path = e.path;
        if (!rest.empty()) {
            if (!path.empty() && path.back() == L'\\') path.pop_back();
            path += rest;
        }
        if (!s.addressSuggest.empty() && _wcsicmp(s.addressSuggest.front().path.c_str(), path.c_str()) == 0)
            continue;
        const auto hint = rank == 2 ? l10n::StringId::AliasSuggestByPinyin
                        : rank == 4 ? l10n::StringId::AliasSuggestByName
                                    : l10n::StringId::AliasSuggestEnter;
        s.addressSuggest.push_back({e.alias + rest, std::move(path), l10n::Get(hint), false});
    }
    // Enter takes the first row unless the text is a known shortcut (desktop, ...).
    if (!app::ResolveAddressShortcut(text).resolved) s.addressSuggestSel = 0;
    done();
}

void ApplyFilterCue(AppState& s) {
    if (!s.hwndFilterEdit) return;
    s.filterCue = l10n::Get(s.filterSelectMode
        ? l10n::StringId::SelectWildcardHint
        : l10n::StringId::FilterPlaceholder);
    SendMessageW(s.hwndFilterEdit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(s.filterCue.c_str()));
}

void LayoutFilterEditor(AppState& s) {
    if (!s.hwndFilterEdit || !s.hwnd || !s.filterEditing) return;
    const float expand = s.pane ? s.pane->filter_expand : 0.0f;
    PlaceHostedEdit(s.hwndFilterEdit, s.hwnd,
                    s.renderer.FilterEditRect(D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),
                        static_cast<float>(s.compositor.Height())), expand,
                        ActiveTab(s) && !ActiveTab(s)->filter_text.empty()),
                    s.scale, 0, 0);
}

void ShowFilterEditor(AppState& s, bool select_mode) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (select_mode) {
        s.filterSelectMode = true;
        s.filterSelectRestore = tab->filter_text;
    } else if (!s.filterEditing) {
        s.filterSelectMode = false;
        s.filterSelectRestore.clear();
    }
    if (s.filterEditing && s.hwndFilterEdit && !s.filterFocusPending) {
        ApplyFilterCue(s);
        SetForegroundWindow(GetAncestor(s.hwndFilterEdit, GA_ROOT));
        SetFocus(s.hwndFilterEdit);
        SendMessageW(s.hwndFilterEdit, EM_SETSEL, 0, -1);
        return;
    }
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.addressEditing) HideAddressEditor(s, false);
    s.filterEditing = true;
    if (!s.hwndFilterEdit) {
        s.hwndFilterEdit = CreateHostedEdit(s, FilterEditProc);
        if (!s.hwndFilterEdit) {
            s.filterEditing = false;
            s.filterSelectMode = false;
            return;
        }
    }
    s.filterIgnoreKillFocus = true;
    SetWindowTextW(s.hwndFilterEdit, tab->filter_text.c_str());
    ApplyFilterCue(s);
    ShowWindow(s.hwndFilterEdit, SW_HIDE);
    s.filterFocusPending = true;
    s.filterIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowWildcardSelect(AppState& s) {
    ShowFilterEditor(s, true);
}

void SyncFilterEditor(AppState& s) {
    if (!s.filterEditing || s.filterIgnoreKillFocus || !s.hwndFilterEdit) return;
    if (auto* tab = ActiveTab(s)) {
        wchar_t text[512]{};
        GetWindowTextW(s.hwndFilterEdit, text, ARRAYSIZE(text));
        if (tab->filter_text != text) {
            CancelScrollAnimation(s);
            tab->filter_text = text;
            tab->scroll_y = 0;
            tab->scroll_x = 0;
            s.scrollTargetY = 0;
        }
        if (!s.filterFocusPending) LayoutFilterEditor(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

void ClearPaneFilter(AppState& s) {
    if (s.filterEditing) HideFilterEditor(s, false);
    CancelScrollAnimation(s);
    if (auto* tab = ActiveTab(s)) {
        tab->filter_text.clear();
        tab->view_filter_map.reset();
        tab->view_cache_filter_text.clear();
        tab->scroll_y = 0;
        tab->scroll_x = 0;
        s.scrollTargetY = 0;
    }
    if (s.hwnd) { SetFocus(s.hwnd); InvalidateRect(s.hwnd, nullptr, FALSE); }
}

void HideFilterEditor(AppState& s, bool commit) {
    if (!s.hwndFilterEdit || !s.filterEditing) return;
    const bool owned_focus = GetFocus() == s.hwndFilterEdit;
    const bool select_mode = s.filterSelectMode;
    const std::wstring restore = s.filterSelectRestore;
    wchar_t buf[512]{};
    GetWindowTextW(s.hwndFilterEdit, buf, ARRAYSIZE(buf));
    s.filterIgnoreKillFocus = true;
    ShowWindow(s.hwndFilterEdit, SW_HIDE);
    s.filterEditing = false;
    s.filterFocusPending = false;
    s.filterSelectMode = false;
    s.filterSelectRestore.clear();
    if (s.hwnd && owned_focus) SetFocus(s.hwnd);
    s.filterIgnoreKillFocus = false;
    if (app::Tab* tab = ActiveTab(s)) {
        if (select_mode) {
            if(commit && tab->content_results) {
                SelectContentPattern(s,buf);
                InvalidateRect(s.hwnd,nullptr,FALSE); return;
            }
            if (commit) {
                tab->filter_text = buf;
                std::vector<int> matches;
                app::CollectFilterMatches(*tab, &s.places, matches);
                tab->SelectIndices(matches);
                tab->filter_text.clear();
                tab->view_filter_map.reset();
                tab->view_cache_filter_text.clear();
                if (tab->selected_index >= 0) EnsureRowVisible(s, *tab, tab->selected_index);
            } else {
                tab->filter_text = restore;
            }
        } else if (commit) {
            tab->filter_text = buf;
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void LayoutRenameOverlay(AppState& s) {
    if (!s.hwndRenameEdit || s.renameIndex < 0 || !s.hwnd) return;
    ui::WindowViewModel vm = BuildVm(s, false);
    D2D1_RECT_F field{};
    bool wraps = false;
    for (const auto& slot : vm.pane_slots) {
        if (!slot.focused) continue;
        const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
        field = s.renderer.RenameFieldRect(slot.pane, list, s.renameIndex);
        wraps = slot.pane.view_mode == ui::ViewMode::ExtraLargeIcons ||
                slot.pane.view_mode == ui::ViewMode::LargeIcons ||
                slot.pane.view_mode == ui::ViewMode::MediumIcons;
        break;
    }
    if (field.right <= field.left) return;
    if (wraps) s.renameFieldW = std::max(s.renameFieldW, field.right - field.left);  // never narrows
    // Icon grids edit the name wrapped over as many lines as it needs (the
    // field grows with it, see RenameFieldRect); other views scroll one line.
    const bool wrapped_before = GetPropW(s.hwndRenameEdit, L"Pulse.EditWrap") != nullptr;
    if (wraps) SetPropW(s.hwndRenameEdit, L"Pulse.EditWrap", reinterpret_cast<HANDLE>(1));
    else RemovePropW(s.hwndRenameEdit, L"Pulse.EditWrap");
    if (wraps != wrapped_before) InvalidateRect(s.hwndRenameEdit, nullptr, FALSE);
    // Seat the EDIT inside the Fluent frame: frame stroke + text padding.
    const float insetX = 3.0f * s.scale;
    const float insetY = 2.0f * s.scale;
    field.left += insetX;
    field.right -= insetX;
    field.top += insetY;
    field.bottom -= insetY;
    if (!wraps) {
        PlaceHostedEdit(s.hwndRenameEdit, s.hwnd, field, s.scale, 4, 4);
        return;
    }
    // The whole wrapped block: PlaceHostedEdit would trim it to one line.
    const int x = static_cast<int>(std::lround(field.left));
    const int y = static_cast<int>(std::lround(field.top));
    const int w = std::max(40, static_cast<int>(std::lround(field.right)) - x);
    const int h = std::max(18, static_cast<int>(std::lround(field.bottom)) - y);
    RECT now{};
    GetWindowRect(s.hwndRenameEdit, &now);
    MapWindowPoints(HWND_DESKTOP, s.hwnd, reinterpret_cast<POINT*>(&now), 2);
    if (now.left != x || now.top != y || now.right - now.left != w || now.bottom - now.top != h)
        SetWindowPos(s.hwndRenameEdit, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
}

// The whole rename field shows: a long name near the bottom scrolls the list.
static void EnsureRenameFieldVisible(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || s.renameIndex < 0) return;
    const ui::WindowViewModel vm = BuildVm(s, false);
    for (const auto& slot : vm.pane_slots) {
        if (!slot.focused) continue;
        const D2D1_RECT_F list = s.renderer.PaneListRect(slot.pane, slot.rect);
        const D2D1_RECT_F field = s.renderer.RenameFieldRect(slot.pane, list, s.renameIndex);
        const float over = field.bottom + 4.0f * s.scale - list.bottom;
        if (field.right > field.left && over > 0.5f) {
            tab->scroll_y += over;
            ClampScroll(s);
            s.scrollTargetY = tab->scroll_y;
            LayoutRenameOverlay(s);
        }
        break;
    }
}

// Typing can add or remove a wrapped line: regrow the field and its frame.
static void RegrowRenameField(AppState& s) {
    if (!s.hwndRenameEdit || s.renameIndex < 0 || !s.hwnd) return;
    if (!GetPropW(s.hwndRenameEdit, L"Pulse.EditWrap")) return;
    RECT before{};
    GetWindowRect(s.hwndRenameEdit, &before);
    LayoutRenameOverlay(s);
    RECT after{};
    GetWindowRect(s.hwndRenameEdit, &after);
    if (!EqualRect(&before, &after)) {
        EnsureRenameFieldVisible(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}
// Every Pulse window of this process repaints: rows cache their alias.
static void BroadcastAliasChanged(AppState& s) {
    if (!s.hwnd) return;
    struct Ctx { wchar_t cls[64]; DWORD pid; } ctx{};
    GetClassNameW(s.hwnd, ctx.cls, ARRAYSIZE(ctx.cls));
    ctx.pid = GetCurrentProcessId();
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        const Ctx& c = *reinterpret_cast<const Ctx*>(p);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        wchar_t cls[64]{};
        if (pid == c.pid && GetClassNameW(w, cls, ARRAYSIZE(cls)) && wcscmp(cls, c.cls) == 0)
            InvalidateRect(w, nullptr, FALSE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

void ShowRenameOverlay(AppState& s) {
    if(DeferContentSelection(s,[](AppState& v){ShowRenameOverlay(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->selected_index < 0) return;
    if (tab->selected_index < static_cast<int>(tab->EntryCount()) &&
        tab->EntryAt(static_cast<size_t>(tab->selected_index)).change_record_only) return;
    if (tab->net_readonly || IsRecycleTab(tab)) return;
    if (tab->SelectedCount() >= 2) {
        ShowBatchRename(s);
        return;
    }
    if (tab->selected_index >= (int)tab->EntryCount()) return;
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.renameIndex = tab->selected_index;
    s.renameFieldW = 0.0f;
    s.renameHint.clear();
    s.renameConfirmExt = false;
    EnsureRowVisible(s, *tab, s.renameIndex);

    // Protect the new model index before tearing down a focused old editor.
    s.renameIgnoreKillFocus = true;
    if (s.hwndRenameEdit) {
        if (IsWindow(s.hwndRenameEdit)) DestroyWindow(s.hwndRenameEdit);
        s.hwndRenameEdit = nullptr;
    }
    if (!s.hwndRenameEdit) {
        s.hwndRenameEdit = CreateHostedEdit(s, RenameEditProc);
        if (!s.hwndRenameEdit) {
            s.renameIndex = -1;
            s.renameIgnoreKillFocus = false;
            return;
        }
    }

    const std::wstring& name = tab->EntryAt(s.renameIndex).name;
    std::wstring initial = name;
    if (s.renameAlias) {
        const std::wstring alias = app::AliasStore::Instance().Find(EntryFullPath(*tab, s.renameIndex));
        if (!alias.empty()) initial = alias;
    }
    SetWindowTextW(s.hwndRenameEdit, initial.c_str());
    LayoutRenameOverlay(s);
    EnsureRenameFieldVisible(s);
    ShowWindow(s.hwndRenameEdit, SW_SHOW);
    SetForegroundWindow(GetAncestor(s.hwndRenameEdit, GA_ROOT));
    SetFocus(s.hwndRenameEdit);
    int stem = (int)name.find_last_of(L'.');
    bool isDir = tab->EntryAt(s.renameIndex).is_dir;
    SendMessageW(s.hwndRenameEdit, EM_SETSEL, 0, (stem > 0 && !isDir && !s.renameAlias) ? stem : -1);
    s.renameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowAliasOverlay(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->SelectedCount() != 1 || tab->selected_index < 0 ||
        tab->selected_index >= static_cast<int>(tab->EntryCount()) || IsRecycleTab(tab))
        return;
    if (EntryFullPath(*tab, tab->selected_index).empty()) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    s.renameAlias = true;
    ShowRenameOverlay(s);
    if (s.renameIndex < 0) s.renameAlias = false;
}

void HideRenameOverlay(AppState& s, bool commit) {
    if (!s.hwndRenameEdit) return;
    if (s.renameIndex < 0) {
        if (IsWindow(s.hwndRenameEdit) && IsWindowVisible(s.hwndRenameEdit))
            ShowWindow(s.hwndRenameEdit, SW_HIDE);
        return;
    }
    const int index = s.renameIndex;
    s.renameIndex = -1;
    const bool alias_edit = s.renameAlias;
    s.renameAlias = false;
    if (s.renameConfirmExt) commit = false;  // an unanswered extension question never commits
    s.renameConfirmExt = false;
    s.renameHint.clear();
    s.renameFieldW = 0.0f;
    if (commit && alias_edit) {
        app::Tab* tab = ActiveTab(s);
        wchar_t buf[512];
        GetWindowTextW(s.hwndRenameEdit, buf, ARRAYSIZE(buf));
        if (tab && tab->snapshot && index < static_cast<int>(tab->EntryCount())) {
            const std::wstring full = EntryFullPath(*tab, index);
            // Typing the real name back is the same as clearing the alias.
            const bool same = _wcsicmp(buf, tab->EntryAt(index).name.c_str()) == 0;
            app::AliasStore::Instance().Set(full, same ? std::wstring_view() : std::wstring_view(buf));
            BroadcastAliasChanged(s);
        }
    } else if (commit) {
        app::Tab* tab = ActiveTab(s);
        wchar_t buf[512];
        GetWindowTextW(s.hwndRenameEdit, buf, ARRAYSIZE(buf));
        if (tab && buf[0] && index < (int)(tab->snapshot ? tab->EntryCount() : 0)) {
            std::wstring full = EntryFullPath(*tab, index);
            if (!full.empty() && buf != tab->EntryAt(index).name) {
                ops::OpRequest req;
                req.type = ops::OpType::Rename;
                req.sources.push_back(full);
                req.new_name = buf;
                // Naming the folder 合并到文件夹 just made is part of that
                // action: one undo takes the name, the move and the folder.
                if (s.groupRenameUndo && _wcsicmp(full.c_str(), s.groupRenameFolder.c_str()) == 0) {
                    req.undo_group = s.groupRenameUndo;
                    req.undo_label = l10n::Get(l10n::StringId::NewFolderWithSelection).c_str();
                }
                tab->pending_selected_name = buf;
                tab->pending_selected_names = { buf };
                tab->pending_selection_revision = tab->selection_revision;
                // The renamed row keeps its place even if the refresh beats
                // the watcher event (#13).
                tab->held_renames.push_back({tab->EntryAt(index).name, buf});
                s.ops.Submit(std::move(req));  // its 别名 follows on completion (app_main)
            }
        }
    }
    s.groupRenameUndo = 0;
    s.groupRenameFolder.clear();
    s.renameIgnoreKillFocus = true;
    ShowWindow(s.hwndRenameEdit, SW_HIDE);
    if (s.hwnd) SetFocus(s.hwnd);
    s.renameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    // Start no editor here: a fresh snapshot will choose the next successful
    // creation after this editor and its kill-focus handling are fully closed.
    if (auto* tab = ActiveTab(s); tab && app::PendingCreateRenameIntent(
        tab->create_rename_intents, tab->current_path, tab->view_generation))
        RefreshActiveTab(s, RefreshReason::OperationCompleted);
}

bool TagRenameCell(AppState& s, const app::TagId& tag_id, D2D1_RECT_F& cell) {
    ui::WindowViewModel vm = BuildVm(s);
    const std::wstring path = app::MakeTagPath(tag_id);
    for (int group = 0; group < static_cast<int>(vm.sidebar.size()); ++group) {
        for (int item = 0; item < static_cast<int>(vm.sidebar[group].items.size()); ++item) {
            if (vm.sidebar[group].items[item].path != path) continue;
            if (!s.renderer.TagItemRect(vm, static_cast<float>(s.compositor.Width()),
                                        static_cast<float>(s.compositor.Height()),
                                        group, item, &cell)) {
                return false;
            }
            cell.left += 34.0f * s.scale;
            cell.right -= 38.0f * s.scale;
            cell.top += 2.0f * s.scale;
            cell.bottom -= 2.0f * s.scale;
            return cell.right > cell.left;
        }
    }
    return false;
}

void LayoutTagRenameOverlay(AppState& s) {
    if (!s.hwndTagRenameEdit || s.tagRenameId.empty() || !s.hwnd) return;
    D2D1_RECT_F cell{};
    if (TagRenameCell(s, s.tagRenameId, cell))
        PlaceHostedEdit(s.hwndTagRenameEdit, s.hwnd, cell, s.scale, 0, 0);
}

void ShowTagRenameOverlay(AppState& s, const app::TagId& tag_id) {
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.tagRenameId = tag_id;
    if (!s.hwndTagRenameEdit)
        s.hwndTagRenameEdit = CreateHostedEdit(s, TagRenameEditProc);
    if (!s.hwndTagRenameEdit) {
        s.tagRenameId.clear();
        return;
    }
    s.tagRenameIgnoreKillFocus = true;
    SetWindowTextW(s.hwndTagRenameEdit, tag->name.c_str());
    LayoutTagRenameOverlay(s);
    ShowWindow(s.hwndTagRenameEdit, SW_SHOW);
    SetForegroundWindow(GetAncestor(s.hwndTagRenameEdit, GA_ROOT));
    SetFocus(s.hwndTagRenameEdit);
    SendMessageW(s.hwndTagRenameEdit, EM_SETSEL, 0, -1);
    s.tagRenameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void HideTagRenameOverlay(AppState& s, bool commit) {
    if (!s.hwndTagRenameEdit || s.tagRenameId.empty()) return;
    const app::TagId tag_id = s.tagRenameId;
    if (commit) {
        wchar_t text[512]{};
        GetWindowTextW(s.hwndTagRenameEdit, text, ARRAYSIZE(text));
        const app::ColorTag* before = s.places.FindTag(tag_id);
        if (before && before->name != text) {
            const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
            if (s.places.RenameTag(tag_id, text))
                QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
            else
                MessageBeep(MB_ICONWARNING);
        }
    }
    s.tagRenameId.clear();
    s.tagRenameIgnoreKillFocus = true;
    ShowWindow(s.hwndTagRenameEdit, SW_HIDE);
    if (s.hwnd) SetFocus(s.hwnd);
    s.tagRenameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}


D2D1_COLOR_F HostedEditForeground(const AppState& s) {
    return ui::ColorFromRef(ui::EditTextColor(s.darkMode));
}

D2D1_COLOR_F HostedEditBackground(const AppState& s) {
    return ui::ColorFromRef(ui::EditBackColor(s.darkMode));
}

IDWriteTextFormat* HostedEditFormat(AppState& s, HWND hwnd) {
    if (hwnd == s.hwndAddressEdit) return s.compositor.AddressFormat();
    return s.compositor.TextFormat();
}

bool HandleHostedEditMessage(AppState& s, HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam, LRESULT& result) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        // Keys the user gave to 窗口总览, 切换窗口 or 下一个标签页 (Settings >
        // 快捷键) act from text boxes like their defaults below: the main
        // window translates them (keymap_input.h).
        WPARAM vk = wParam;
        if (vk == VK_PROCESSKEY) vk = ImmGetVirtualKey(hwnd);
        if (!app::IsModifierKey(static_cast<UINT>(vk))) {
            const auto t = app::ActiveKeymap().Translate(app::CurrentChord(static_cast<UINT>(vk)));
            if (t.kind == app::Keymap::Translation::Rewrite &&
                (t.action == app::KeyAction::Overview || t.action == app::KeyAction::SwitchWindow ||
                 t.action == app::KeyAction::NextTab)) {
                SetFocus(s.hwnd);
                result = SendMessageW(s.hwnd, msg, vk, lParam);
                return true;
            }
        }
    }
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) &&
        app::IsTabShortcut(static_cast<UINT>(wParam),
                           (GetKeyState(VK_CONTROL) & 0x8000) != 0,
                           (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                           (GetKeyState(VK_MENU) & 0x8000) != 0)) {
        if (msg == WM_KEYDOWN) HandleKeyDown(&s, s.hwnd, msg, wParam, lParam);
        result = 0;
        return true;
    }
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
        // Window keys an edit box has no use for: 窗口总览 and the window
        // switcher answer on the main window, which also takes the keys
        // that follow (Esc, arrows), so the focus moves there first.
        WPARAM vk = wParam;
        if (vk == VK_PROCESSKEY) vk = ImmGetVirtualKey(hwnd);
        if (IsWindowOverviewKey(vk) || app::IsWindowSwitcherScanCode(lParam)) {
            SetFocus(s.hwnd);
            result = SendMessageW(s.hwnd, WM_KEYDOWN, vk, lParam);
            return true;
        }
    }
    EnsureEditVisuals(s);
    return ui::HandleChildEditMessage(s.compositor, HostedEditFormat(s, hwnd),
        HostedEditForeground(s), HostedEditBackground(s), ui::EditBackBrush(s.editBrush),
        hwnd, msg, wParam, lParam, result);
}

static LRESULT DefPresentedHostedEditProc(AppState* s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!s) return DefSubclassProc(hwnd, msg, wParam, lParam);
    return ui::DefPresentedChildEditProc(s->compositor, HostedEditFormat(*s, hwnd),
        HostedEditForeground(*s), HostedEditBackground(*s), hwnd, msg, wParam, lParam);
}
LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    if (s && msg == WM_IME_STARTCOMPOSITION) s->addressSearchComposing = true;
    if (s && msg == WM_IME_ENDCOMPOSITION) {
        s->addressSearchComposing = false;
        QueueAddressSearch(*s);
    }

    if (s && s->addressSearchComposing && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR))
        return DefPresentedHostedEditProc(s, hwnd, msg, wParam, lParam);
    if (s && msg == WM_LBUTTONUP && s->addressSearching && !s->searchHistoryOpen)
        PostMessageW(s->hwnd, WM_SEARCH_HISTORY, 0, 0);
    LRESULT handled = 0;
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (s->addressSearching && wParam == VK_DOWN) {
            // On a results page Down moves into the list; otherwise it opens history.
            if (IsAddressSearchResults(ActiveTab(*s))) FocusSearchResults(*s, true);
            else ShowAddressSearchHistory(*s);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) &&
            (wParam == L'K' || wParam == L'L')) {
            if (wParam == L'K' && !(GetKeyState(VK_SHIFT) & 0x8000)) {
                ShowAddressSearch(*s);
                return 0;
            }
            HideAddressEditor(*s, false);
            ShowOmnibar(*s, wParam == L'K' ? OmnibarMode::Mixed : OmnibarMode::Path);
            return 0;
        }
        if (s->addressSearching && wParam == VK_TAB) {
            ShowAddressSearchScope(*s);
            return 0;
        }
        if (!s->addressSearching && !s->addressSuggest.empty() && (wParam == VK_DOWN || wParam == VK_UP)) {
            const int n = static_cast<int>(s->addressSuggest.size());
            int& sel = s->addressSuggestSel;
            sel = wParam == VK_DOWN ? (sel + 1 >= n ? -1 : sel + 1) : (sel < 0 ? n - 1 : sel - 1);
            InvalidateRect(s->hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_RETURN && !s->addressSearching && s->addressSuggestSel >= 0 &&
            s->addressSuggestSel < static_cast<int>(s->addressSuggest.size())) {
            const std::wstring path = s->addressSuggest[static_cast<size_t>(s->addressSuggestSel)].path;
            HideAddressEditor(*s, false);
            NavigateTo(*s, path);
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (s->addressSearching) {
                // Shift+Enter searches every indexed location.
                if (GetKeyState(VK_SHIFT) & 0x8000) s->addressSearchCurrent = false;
                SubmitAddressSearch(*s);
                FocusSearchResults(*s, false);
            }
            else HideAddressEditor(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (!s->addressSearching && !s->addressSuggest.empty()) {
                // First Esc closes the suggestions, the next one the editor.
                s->addressSuggest.clear();
                s->addressSuggestSel = -1;
                InvalidateRect(s->hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->menu && s->menu->IsOpen()) s->menu->Dismiss();
            else if (s->addressSearching) EscapeAddressSearch(*s);
            else HideAddressEditor(*s, false);
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (s->addressSearching && wParam == VK_RETURN) {
            // Alt+Enter searches file contents.
            if (!s->addressSearchContent) SwitchAddressSearchMode(*s, true);
            SubmitAddressSearch(*s);
            FocusSearchResults(*s, false);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (s->addressSearching && wParam == VK_RETURN) return 0;
        break;
    case WM_CHAR:
        if (s->addressSearching && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB))
            return 0;
        break;
    case WM_KILLFOCUS:
        if (s->addressSearching && reinterpret_cast<HWND>(wParam) == s->hwnd) break;
        if (!s->addressIgnoreKillFocus) HideAddressEditor(*s, false);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    return DefPresentedHostedEditProc(s, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    LRESULT handled = 0;
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
            HideFilterEditor(*s, wParam == VK_RETURN);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam != VK_RETURN && wParam != VK_ESCAPE) {
            LRESULT lr = DefPresentedHostedEditProc(s, hwnd, msg, wParam, lParam);
            SyncFilterEditor(*s);
            return lr;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->filterIgnoreKillFocus) HideFilterEditor(*s, true);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    return DefPresentedHostedEditProc(s, hwnd, msg, wParam, lParam);
}

static LRESULT RenameEditProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR dwRefData);
// Inline rename keys (P1f) --------------------------------------------------
static bool IsIllegalNameChar(wchar_t c) {
    return c != 0 && wcschr(L"\\/:*?\"<>|", c) != nullptr;
}

static void ShowRenameHint(AppState& s, std::wstring text, bool until_answered) {
    s.renameHint = std::move(text);
    s.renameHintUntil = until_answered ? 0 : GetTickCount64() + 3000;
    if (!until_answered) s.renameConfirmExt = false;  // the question is no longer on screen
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Typing again answers the extension question with "keep editing".
static void DropRenameQuestion(AppState& s) {
    if (!s.renameConfirmExt) return;
    s.renameConfirmExt = false;
    s.renameHint.clear();
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

static std::wstring_view NameExtension(std::wstring_view name) {
    const size_t dot = name.find_last_of(L'.');
    return dot == std::wstring_view::npos || dot == 0 ? std::wstring_view() : name.substr(dot);
}

// A file (not a folder or a 别名) whose typed name ends in another extension.
static bool RenameChangesExtension(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || s.renameAlias || s.renameIndex < 0 ||
        s.renameIndex >= static_cast<int>(tab->EntryCount()) || !s.hwndRenameEdit)
        return false;
    const auto& entry = tab->EntryAt(static_cast<size_t>(s.renameIndex));
    if (entry.is_dir) return false;
    wchar_t buf[512]{};
    GetWindowTextW(s.hwndRenameEdit, buf, ARRAYSIZE(buf));
    if (!buf[0] || entry.name == buf) return false;
    const std::wstring_view before = NameExtension(entry.name);
    const std::wstring_view after = NameExtension(buf);
    return before.size() != after.size() ||
           (!before.empty() && CompareStringOrdinal(before.data(), static_cast<int>(before.size()), after.data(),
                                                    static_cast<int>(after.size()), TRUE) != CSTR_EQUAL);
}

// Tab / Shift+Tab: commit, then rename the next / previous item in view order.
static void RelayRename(AppState& s, int dir) {
    app::Tab* tab = ActiveTab(s);
    const int from = s.renameIndex;
    if (!tab || from < 0 || s.renameAlias) {
        HideRenameOverlay(s, true);
        return;
    }
    const ui::WindowViewModel vm = BuildVm(s, false);
    const int view = vm.pane.ViewIndex(from);
    const int next = view + dir;
    HideRenameOverlay(s, true);
    if (view < 0 || next < 0 || next >= static_cast<int>(vm.pane.EntryCount())) return;
    const int source = vm.pane.SourceIndex(next);
    if (source < 0) return;
    tab->SelectOnly(source);
    ShowRenameOverlay(s);
}

// Enter (dir 0) or Tab (dir +-1). A changed extension asks once first, like
// File Explorer: Enter again renames, Esc goes back to the extension.
static void SubmitRename(AppState& s, int dir) {
    if (s.renameConfirmExt) {
        s.renameConfirmExt = false;  // answered
    } else if (RenameChangesExtension(s)) {
        s.renameConfirmExt = true;
        ShowRenameHint(s, l10n::Get(l10n::StringId::RenameExtensionConfirm), true);
        return;
    }
    if (dir == 0) HideRenameOverlay(s, true);
    else RelayRename(s, dir);
}

static void BackToExtension(AppState& s, HWND hwnd) {
    DropRenameQuestion(s);
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(hwnd, text.data(), length + 1)));
    const size_t dot = text.find_last_of(L'.');
    const int from = dot == std::wstring::npos || dot == 0 ? length : static_cast<int>(dot) + 1;
    SendMessageW(hwnd, EM_SETSEL, static_cast<WPARAM>(from), static_cast<LPARAM>(length));
}

// Pasted text loses what a name cannot hold; line breaks go quietly.
static bool PasteRenameText(AppState& s, HWND hwnd) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(hwnd)) return false;
    std::wstring text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = chars;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    std::wstring clean;
    bool refused = false;
    for (const wchar_t c : text) {
        if (c == L'\r' || c == L'\n' || c == L'\t') continue;
        if (IsIllegalNameChar(c)) {
            refused = true;
            continue;
        }
        clean.push_back(c);
    }
    if (clean.size() == text.size()) return false;  // nothing to drop: the EDIT pastes
    DropRenameQuestion(s);
    SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(clean.c_str()));
    if (refused) ShowRenameHint(s, l10n::Get(l10n::StringId::ANameCanTContainAnyOfTheseCharacters), false);
    return true;
}

// Keys taken before the shared edit handling sees them.
static bool RenameKeyFilter(AppState& s, HWND hwnd, UINT msg, WPARAM wParam) {
    const bool plain = (GetKeyState(VK_CONTROL) & 0x8000) == 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
    if (msg == WM_KEYDOWN && wParam == VK_TAB && plain) {
        SubmitRename(s, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
        return true;
    }
    if (msg == WM_CHAR && wParam == L'\t') return true;
    if (msg == WM_CHAR && IsIllegalNameChar(static_cast<wchar_t>(wParam))) {
        MessageBeep(MB_OK);
        ShowRenameHint(s, l10n::Get(l10n::StringId::ANameCanTContainAnyOfTheseCharacters), false);
        return true;
    }
    if ((msg == WM_CHAR && wParam >= 0x20) || (msg == WM_CHAR && wParam == VK_BACK) ||
        (msg == WM_KEYDOWN && wParam == VK_DELETE))
        DropRenameQuestion(s);
    if (msg == WM_PASTE) return PasteRenameText(s, hwnd);
    return false;
}

LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    static const bool trace = GetEnvironmentVariableW(L"PULSE_EDIT_TRACE", nullptr, 0) > 0;
    static thread_local bool inside = false;
    if (!trace || inside || msg == WM_TIMER || msg == WM_MOUSEMOVE || msg == WM_NCHITTEST || msg == WM_SETCURSOR ||
        msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_GETTEXT || msg == WM_GETTEXTLENGTH || msg == EM_GETSEL)
        return RenameEditProcImpl(hwnd, msg, wParam, lParam, dwRefData);
    DWORD a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    inside = true;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&a0), reinterpret_cast<LPARAM>(&a1));
    inside = false;
    const LRESULT r = RenameEditProcImpl(hwnd, msg, wParam, lParam, dwRefData);
    inside = true;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&b0), reinterpret_cast<LPARAM>(&b1));
    POINT cp{}; GetCaretPos(&cp);
    wchar_t line[240];
    swprintf_s(line, L"%04X w=%llX l=%llX sel %lu,%lu -> %lu,%lu len=%d r=%lld first=%lld caret=%ld,%ld\n", msg,
        static_cast<unsigned long long>(wParam), static_cast<unsigned long long>(lParam), a0, a1, b0, b1,
        GetWindowTextLengthW(hwnd), static_cast<long long>(r),
        static_cast<long long>(SendMessageW(hwnd, EM_CHARFROMPOS, 0, MAKELPARAM(6, 4)) & 0xFFFF), cp.x, cp.y);
    inside = false;
    wchar_t path[MAX_PATH];
    if (GetEnvironmentVariableW(L"PULSE_EDIT_TRACE", path, MAX_PATH)) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"a") == 0 && f) { fputws(line, f); fclose(f); }
    }
    return r;
}
static LRESULT RenameEditProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    LRESULT handled = 0;
    if (s && RenameKeyFilter(*s, hwnd, msg, wParam)) {
        RegrowRenameField(*s);
        return 0;
    }
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            SubmitRename(*s, 0);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (s->renameConfirmExt) BackToExtension(*s, hwnd);
            else HideRenameOverlay(*s, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->renameIgnoreKillFocus) HideRenameOverlay(*s, true);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    const bool edits_text = msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_PASTE || msg == WM_CUT ||
        msg == WM_CLEAR || msg == WM_UNDO || msg == EM_UNDO || msg == EM_REPLACESEL ||
        msg == WM_SETTEXT || msg == WM_IME_COMPOSITION || msg == WM_IME_ENDCOMPOSITION;
    const LRESULT result = DefPresentedHostedEditProc(s, hwnd, msg, wParam, lParam);
    if (s && edits_text) RegrowRenameField(*s);
    return result;
}

LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    LRESULT handled = 0;
    if (s && HandleHostedEditMessage(*s, hwnd, msg, wParam, lParam, handled)) return handled;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            HideTagRenameOverlay(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideTagRenameOverlay(*s, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->tagRenameIgnoreKillFocus) HideTagRenameOverlay(*s, true);
        break;
    case WM_ERASEBKGND: {
        return EraseHostedEditBackground(hwnd, wParam, s) ? 1 : 0;
    }
    }
    return DefPresentedHostedEditProc(s, hwnd, msg, wParam, lParam);
}

} // namespace pulse
