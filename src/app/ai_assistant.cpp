// ai_assistant.cpp - Pulse 助手: panel state, composer EDIT, pointer routing.
#include "ai_assistant.h"

#include "ai_assistant_chat.h"
#include "ai_assistant_conn.h"
#include "ai_settings.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "../common/localization.h"
#include "../ui/typography.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse {
namespace {

namespace ai = app::ai;
using I = l10n::StringId;

constexpr UINT_PTR kComposerSubclass = 0x4149;   // 'AI'
constexpr UINT_PTR kTipTimer = 0x4154;           // 'AT'
constexpr UINT kTipDelayMs = 520;
constexpr uint64_t kMenuFadeMs = 240;

struct ModelInfo { const char* id; const wchar_t* name; I detail; };
constexpr ModelInfo kModels[] = {
    {"deepseek-flash", L"deepseek-flash", I::AiAsstModelFlashDetail},
    {"deepseek-v4-pro", L"deepseek-v4-pro", I::AiAsstModelProDetail},
};
constexpr int kModelCount = static_cast<int>(sizeof(kModels) / sizeof(kModels[0]));
constexpr I kEffortNames[] = {I::AiAsstEffortOff, I::AiAsstEffortLow, I::AiAsstEffortMid, I::AiAsstEffortHigh};
constexpr I kEffortDetails[] = {I::AiAsstEffortOffDetail, I::AiAsstEffortLowDetail, I::AiAsstEffortMidDetail,
                                I::AiAsstEffortHighDetail};

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

COLORREF ToColorRef(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<BYTE>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void LoadPrefs(AiAssistantState& a) {
    if (a.prefs_loaded) return;
    a.prefs_loaded = true;
    const ai::Settings settings = ai::LoadSettings();
    a.floating = settings.assistant.floating;
    a.effort = std::clamp(settings.assistant.effort, 0, 3);
    a.model = 0;
    for (int i = 0; i < kModelCount; ++i)
        if (settings.assistant.model == kModels[i].id) a.model = i;
}

void SavePrefs(const AiAssistantState& a) {
    bool malformed = false;
    ai::Settings settings = ai::LoadSettings(&malformed);
    if (malformed) return;   // never overwrite a file we could not read
    settings.assistant.floating = a.floating;
    settings.assistant.model = kModels[std::clamp(a.model, 0, kModelCount - 1)].id;
    settings.assistant.effort = a.effort;
    (void)ai::SaveSettings(settings);
}

// The folder the assistant sees: the active tab's, if it is a real folder.
bool CurrentFolder(AppState& s, std::wstring& path, std::wstring& name, size_t& count) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty()) return false;
    if (tab->current_path.rfind(L"pulse:", 0) == 0) return false;
    path = tab->current_path;
    name = app::TabTitle(path);
    count = tab->EntryCount();
    return !name.empty();
}

std::vector<ui::AiSuggestionView> Suggestions(AppState& s) {
    std::wstring path, folder;
    size_t count = 0;
    const bool has_folder = CurrentFolder(s, path, folder, count);
    struct Sug { ui::AiIcon icon; ui::AiTint tint; I title; I detail; bool folder; };
    static constexpr Sug kSugs[] = {
        {ui::AiIcon::Move, ui::AiTint::Image, I::AiAsstSugSortTitle, I::AiAsstSugSortDetail, true},
        {ui::AiIcon::Size, ui::AiTint::Archive, I::AiAsstSugSizeTitle, I::AiAsstSugSizeDetail, true},
        {ui::AiIcon::Duplicate, ui::AiTint::Document, I::AiAsstSugDupTitle, I::AiAsstSugDupDetail, false},
        {ui::AiIcon::Camera, ui::AiTint::Video, I::AiAsstSugPhotoTitle, I::AiAsstSugPhotoDetail, false},
    };
    std::vector<ui::AiSuggestionView> out;
    for (const Sug& sug : kSugs) {
        // Folder suggestions only make sense with a folder open.
        if (sug.folder && !has_folder) continue;
        out.push_back({sug.icon, sug.tint, Fill(Text(sug.title), L"{folder}", folder), Text(sug.detail)});
    }
    return out;
}

void ApplyPanel(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    s.renderer.SetAiPanel(a.open, a.floating ? ui::AiPanelMode::Floating : ui::AiPanelMode::Docked, animate);
    Redraw(s);
}

void ReadComposerMetrics(AiAssistantState& a) {
    if (!a.edit) return;
    a.text_len = GetWindowTextLengthW(a.edit);
    a.lines = (std::max)(1, static_cast<int>(SendMessageW(a.edit, EM_GETLINECOUNT, 0, 0)));
}

std::wstring ComposerText(const AiAssistantState& a) {
    if (!a.edit) return {};
    const int n = GetWindowTextLengthW(a.edit);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(a.edit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

// The composer's folder chip as the user bubble shows it ("" when removed).
std::wstring ContextText(AppState& s) {
    std::wstring path, folder;
    size_t count = 0;
    if (!CurrentFolder(s, path, folder, count) || s.aiAssistant.context_removed_for == path) return {};
    return Fill(Fill(Text(I::AiAsstContext), L"{folder}", folder), L"{n}", std::to_wstring(count));
}

bool Submit(AppState& s, const std::wstring& text) {
    if (Chat(s).busy) return false;
    AiChatSubmit(s, text, ContextText(s));
    return true;
}

void SubmitComposer(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    std::wstring text = ComposerText(a);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    if (text.empty()) return;
    if (Submit(s, text) && a.edit) SetWindowTextW(a.edit, L"");
}

void SyncComposer(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    const int old_len = a.text_len, old_lines = a.lines;
    ReadComposerMetrics(a);
    if ((old_len == 0) != (a.text_len == 0) && a.edit) InvalidateRect(a.edit, nullptr, TRUE);   // placeholder
    if (old_len != a.text_len || old_lines != a.lines) Redraw(s);
}

// The placeholder over the empty EDIT, in the panel's text3.
void PaintPlaceholder(AppState& s, HWND hwnd) {
    AiAssistantState& a = s.aiAssistant;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    HideCaret(hwnd);
    RECT r{};
    SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&r));
    HFONT old = static_cast<HFONT>(SelectObject(hdc, a.font));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ToColorRef(s.renderer.AiPanel().PlaceholderColor()));
    const std::wstring text = Text(I::AiAsstPlaceholder);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, old);
    ShowCaret(hwnd);
    ReleaseDC(hwnd, hdc);
}

void CloseMenu(AppState& s);

LRESULT CALLBACK ComposerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiAssistantState& a = s.aiAssistant;
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            SubmitComposer(s);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (a.menu_open) {
                CloseMenu(s);
                Redraw(s);
            } else if (a.chat && a.chat->busy) {
                AiChatStop(s);
            } else {
                AiAssistantClose(s);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        // Enter sends (WM_KEYDOWN); Shift+Enter is a new line. No beeps.
        if ((wp == L'\r' || wp == L'\n') && (GetKeyState(VK_SHIFT) & 0x8000) == 0) return 0;
        if (wp == 27) return 0;
        if (wp == 1) {   // Ctrl+A: select all (a multiline EDIT does not)
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        a.focused = msg == WM_SETFOCUS;
        Redraw(s);
        return r;
    }
    case WM_PAINT: {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (GetWindowTextLengthW(hwnd) == 0) PaintPlaceholder(s, hwnd);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ComposerProc, kComposerSubclass);
        break;
    default:
        break;
    }
    const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
    case EM_UNDO: case WM_SETTEXT: case EM_REPLACESEL: case WM_SIZE:
        SyncComposer(s);
        break;
    default:
        break;
    }
    return r;
}

void EnsureFont(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.font && a.font_scale == s.scale) return;
    if (a.font) DeleteObject(a.font);
    a.font = ui::typography::CreateEditFont(s.scale, 13.5f);
    a.font_scale = s.scale;
    a.line_px = 0;
    if (a.edit && a.font) {
        SendMessageW(a.edit, WM_SETFONT, reinterpret_cast<WPARAM>(a.font), TRUE);
        SendMessageW(a.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    if (a.font) {
        HDC hdc = GetDC(s.hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, a.font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(s.hwnd, hdc);
        a.line_px = static_cast<int>(tm.tmHeight);
    }
}

void EnsureEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.edit || !s.hwnd) return;
    // A redirected layered child like the other hosted editors (edit_host.h),
    // but multi-line and wrapping (no ES_AUTOHSCROLL).
    a.edit = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"",
                             WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                             0, 0, 0, 0, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!a.edit) return;
    SetLayeredWindowAttributes(a.edit, 0, 255, LWA_ALPHA);
    SetWindowTheme(a.edit, L"", L"");
    SendMessageW(a.edit, EM_LIMITTEXT, 8000, 0);
    a.font_scale = 0.0f;
    EnsureFont(s);
    SetWindowSubclass(a.edit, ComposerProc, kComposerSubclass, reinterpret_cast<DWORD_PTR>(&s));
    ReadComposerMetrics(a);
}

void HideEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.edit || !a.edit_visible) return;
    const bool had_focus = GetFocus() == a.edit;
    ShowWindow(a.edit, SW_HIDE);
    a.edit_visible = false;
    if (had_focus && s.hwnd) SetFocus(s.hwnd);
}

void CloseMenu(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AiConnClose(s);
    if (!a.menu_open) return;
    a.menu_open = false;
    a.menu_closed_at = ui::motion::NowMs();
}

// Header buttons and the answer's acts show a tip after a dwell.
bool HasTip(ui::AiPanelPart part) {
    using P = ui::AiPanelPart;
    return part == P::NewChat || part == P::Mode || part == P::Close || part == P::ActCopy || part == P::ActRetry ||
           part == P::ActThumb || part == P::ConnBack || part == P::ConnClose || part == P::ConnEye;
}

void ShowTipLater(AppState& s) {
    if (s.hwnd) SetTimer(s.hwnd, kTipTimer, kTipDelayMs, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
        KillTimer(hwnd, id);
        InvalidateRect(hwnd, nullptr, FALSE);
    });
}

void Activate(AppState& s, const ui::AiPanelHit& hit) {
    AiAssistantState& a = s.aiAssistant;
    using P = ui::AiPanelPart;
    if (bool close_menu = false; AiConnActivate(s, hit, close_menu)) {
        if (close_menu) {
            CloseMenu(s);
            a.focus_pending = true;
        }
        Redraw(s);
        return;
    }
    switch (hit.part) {
    case P::NewChat:
        CloseMenu(s);
        AiChatReset(s);
        if (a.edit) SetWindowTextW(a.edit, L"");
        a.context_removed_for.clear();
        s.renderer.AiPanel().Rewake(ui::motion::NowMs());
        a.focus_pending = true;
        break;
    case P::Mode:
        CloseMenu(s);
        a.floating = !a.floating;
        a.focus_pending = GetFocus() == a.edit;
        ApplyPanel(s, true);
        SavePrefs(a);
        break;
    case P::Close:
        AiAssistantClose(s);
        return;
    case P::Suggestion: {
        CloseMenu(s);
        const std::vector<ui::AiSuggestionView> suggestions = Suggestions(s);
        if (hit.index >= 0 && hit.index < static_cast<int>(suggestions.size()))
            Submit(s, suggestions[static_cast<size_t>(hit.index)].title);
        break;
    }
    case P::ContextRemove: {
        std::wstring path, name;
        size_t count = 0;
        if (CurrentFolder(s, path, name, count)) a.context_removed_for = path;
        a.focus_pending = true;
        break;
    }
    case P::ModelButton:
        if (a.menu_open) CloseMenu(s);
        else a.menu_open = true;
        break;
    case P::MenuModel:
        if (hit.index >= 0 && hit.index < kModelCount && hit.index != a.model) {
            a.model = hit.index;
            SavePrefs(a);
        }
        CloseMenu(s);
        a.focus_pending = true;
        break;
    case P::MenuEffort:
        // The menu stays open to show the chosen strength's line.
        if (hit.index >= 0 && hit.index < 4 && hit.index != a.effort) {
            a.effort = hit.index;
            SavePrefs(a);
        }
        break;
    case P::MenuManage:
        // The menu unfolds into the 管理连接 card in place.
        AiConnOpen(s);
        break;
    case P::Send:
        CloseMenu(s);
        if (a.chat && a.chat->busy) AiChatStop(s);
        else SubmitComposer(s);
        a.focus_pending = true;
        break;
    case P::ThinkToggle:
    case P::StepsToggle:
    case P::PlanToggle:
    case P::ConfirmDeny:
    case P::ConfirmAccept:
    case P::ResultUndo:
    case P::ResultShow:
    case P::ActCopy:
    case P::ActRetry:
    case P::ActThumb:
        CloseMenu(s);
        AiChatActivate(s, hit.part, hit.index);
        break;
    case P::AddFiles:
    case P::ContextChip:
    case P::Input:
    case P::Panel:
    case P::Conversation:
        CloseMenu(s);
        a.focus_pending = true;
        break;
    default:
        break;
    }
    Redraw(s);
}

} // namespace

bool AiAssistantIsOpen(const AppState& s) { return s.aiAssistant.open; }

void AiAssistantOpen(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    LoadPrefs(a);
    if (a.open) return;
    a.open = true;
    a.focus_pending = true;
    if (a.chat) a.chat->unread = false;
    EnsureEdit(s);
    ApplyPanel(s, animate);
}

void AiAssistantClose(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return;
    a.open = false;
    AiConnClose(s);
    a.menu_open = false;
    a.hover = {};
    a.pressed = {};
    if (a.captured && GetCapture() == s.hwnd) ReleaseCapture();
    a.captured = false;
    HideEdit(s);
    ApplyPanel(s, animate);
}

void AiAssistantToggle(AppState& s) {
    if (s.aiAssistant.open) AiAssistantClose(s);
    else AiAssistantOpen(s);
}

void FillAiAssistantVm(AppState& s, ui::WindowViewModel& vm) {
    AiAssistantState& a = s.aiAssistant;
    // The chip is always there: "✦ AI" opens the panel.
    if (vm.status.ai_state == ui::StatusAiState::Hidden) vm.status.ai_state = ui::StatusAiState::Idle;
    if (vm.status.ai_state == ui::StatusAiState::Idle) vm.status.ai_text.clear();
    vm.status.ai_label = Text(I::AiAsstChipLabel);
    vm.status.ai_open = a.open;
    // While Pulse 助手 works its words take the chip.
    if (a.chat && a.chat->chip != ui::StatusAiState::Idle) {
        vm.status.ai_state = a.chat->chip;
        vm.status.ai_text = a.chat->chip_text;
        vm.status.ai_progress = a.chat->chip_progress;
        vm.status.ai_unread = a.chat->unread;
    }
    if (!a.open && !s.renderer.AiPanel().Visible()) return;
    LoadPrefs(a);

    ui::AiPanelViewModel& p = vm.ai_panel;
    p.dark = vm.dark;
    p.title = Text(I::AiAsstTitle);
    AiChat& chat = Chat(s);
    p.empty = chat.blocks.empty();
    p.blocks = chat.blocks;
    if (chat.think_id) {
        // The running thinking header counts seconds.
        for (ui::AiBlockView& b : p.blocks)
            if (b.id == chat.think_id && b.running) {
                const uint64_t secs = (ui::motion::NowMs() - chat.think_start) / 1000;
                if (secs > 0) b.seconds = Fill(Text(I::AiAsstSeconds), L"{n}", std::to_wstring(secs));
            }
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    p.greeting = Text(now.wHour < 11 ? I::AiAsstGreetMorning : now.wHour < 13 ? I::AiAsstGreetNoon
                      : now.wHour < 18 ? I::AiAsstGreetAfternoon : I::AiAsstGreetEvening);
    p.intro = Text(I::AiAsstIntro);
    std::wstring path, folder;
    size_t count = 0;
    const bool has_folder = CurrentFolder(s, path, folder, count);
    p.suggestions = Suggestions(s);
    p.safe_note = Text(I::AiAsstSafe);

    p.context_visible = has_folder && a.context_removed_for != path;
    if (p.context_visible)
        p.context_text = Fill(Fill(Text(I::AiAsstContext), L"{folder}", folder), L"{n}", std::to_wstring(count));
    p.composer_focused = a.focused && a.edit_visible;
    p.composer_empty = a.text_len == 0;
    p.composer_lines = a.lines;
    p.composer_line_px = static_cast<float>(a.line_px);
    const bool menu_covers = a.menu_open || ui::motion::NowMs() - a.menu_closed_at < kMenuFadeMs;
    if (!p.composer_empty && (!a.edit_visible || menu_covers)) {
        p.draw_composer_text = true;
        p.composer_text = ComposerText(a);
    }
    p.placeholder = Text(I::AiAsstPlaceholder);
    p.add_files = Text(I::AiAsstAddFiles);
    const int model = std::clamp(a.model, 0, kModelCount - 1);
    p.model_label = kModels[model].name;
    p.effort = std::clamp(a.effort, 0, 3);
    p.effort_label = p.effort == 0 ? std::wstring() : L" \u00B7 " + Text(kEffortNames[p.effort]);
    p.send = chat.busy ? ui::AiSendState::Busy : p.composer_empty ? ui::AiSendState::Dim : ui::AiSendState::Ready;

    p.menu_open = a.menu_open;
    p.model_heading = Text(I::AiAsstModelHeading);
    p.effort_heading = Text(I::AiAsstEffortHeading);
    for (const ModelInfo& m : kModels) p.models.push_back({m.name, Text(m.detail)});
    p.model = model;
    for (size_t i = 0; i < 4; ++i) p.effort_names[i] = Text(kEffortNames[i]);
    p.effort_detail = Text(kEffortDetails[p.effort]);
    // Connected once a DeepSeek key is stored (ai_model_key.h).
    p.connected = AiAgentConnected();
    p.connection_text = p.connected ? Fill(Text(I::AiAsstConnected), L"{host}", AiAgentHost())
                                    : Text(I::AiAsstNotConnected);
    p.manage_text = Text(I::AiAsstManage);
    AiConnFill(s, p);

    p.hover = a.hover;
    p.pressed = a.pressed;
    // Header tips after a short dwell (the timer repaints).
    using P = ui::AiPanelPart;
    if (HasTip(a.hover.part) && a.pressed.part == P::None && GetTickCount64() - a.hover_since >= kTipDelayMs) {
        const P part = a.hover.part;
        std::wstring conn_tip;
        if (AiConnTip(s, part, conn_tip)) {
            // A card part under a pointer that has not moved since ‹ / ⌄.
            if (AiConnIsOpen(s)) vm.tooltip_text = std::move(conn_tip);
        } else vm.tooltip_text = Text(part == P::NewChat ? I::AiAsstNewChat
                               : part == P::Close ? I::AiAsstCollapse
                               : part == P::ActCopy ? I::AiAsstCopy
                               : part == P::ActRetry ? I::AiAsstRetry
                               : part == P::ActThumb ? I::AiAsstHelpful
                               : a.floating ? I::AiAsstDock : I::AiAsstFloat);
        vm.tooltip_x = static_cast<float>(a.hover_point.x);
        vm.tooltip_y = static_cast<float>(a.hover_point.y);
    }
}

void AiAssistantLayout(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AiConnLayout(s);
    if (!a.edit) return;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const bool menu_covers = a.menu_open || ui::motion::NowMs() - a.menu_closed_at < kMenuFadeMs;
    if (!a.open || !view.InputPlaced() || menu_covers) {
        HideEdit(s);
        if (a.open && menu_covers && !a.menu_open) Redraw(s);   // show it again after the fade
        return;
    }
    EnsureFont(s);
    const D2D1_RECT_F r = view.InputRect();
    const int x = static_cast<int>(std::lround(r.left));
    const int y = static_cast<int>(std::lround(r.top));
    const int w = (std::max)(1, static_cast<int>(std::lround(r.right - r.left)));
    const int h = (std::max)(1, static_cast<int>(std::lround(r.bottom - r.top)));
    RECT cur{};
    GetWindowRect(a.edit, &cur);
    MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed = cur.left == x && cur.top == y && cur.right - cur.left == w && cur.bottom - cur.top == h;
    if (!placed || !a.edit_visible) {
        SetWindowPos(a.edit, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        a.edit_visible = true;
        Redraw(s);   // the panel stops drawing the text itself
    }
    if (a.focus_pending) {
        a.focus_pending = false;
        if (GetForegroundWindow() == s.hwnd || GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) SetFocus(a.edit);
    }
}

bool AiAssistantMouseMove(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return false;
    const ui::AiPanelHit hit = s.renderer.AiPanel().HitTest(static_cast<float>(x), static_cast<float>(y));
    if (!(hit == a.hover)) {
        a.hover = hit;
        a.hover_since = GetTickCount64();
        using P = ui::AiPanelPart;
        if (HasTip(hit.part)) ShowTipLater(s);
        Redraw(s);
    }
    a.hover_point = POINT{x, y};
    if (hit.part == ui::AiPanelPart::None && !a.captured) return false;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, s.hwnd, 0};
    TrackMouseEvent(&tme);
    return true;
}

bool AiAssistantMouseDown(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return false;
    const ui::AiPanelHit hit = s.renderer.AiPanel().HitTest(static_cast<float>(x), static_cast<float>(y));
    if (hit.part == ui::AiPanelPart::None) {
        // Outside: the menu folds away and the click goes on to Pulse.
        if (a.menu_open) {
            CloseMenu(s);
            Redraw(s);
        }
        return false;
    }
    a.pressed = hit;
    a.hover = hit;
    SetCapture(s.hwnd);
    a.captured = true;
    Redraw(s);
    return true;
}

bool AiAssistantMouseUp(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.captured) return false;
    a.captured = false;
    if (GetCapture() == s.hwnd) ReleaseCapture();
    const ui::AiPanelHit pressed = a.pressed;
    a.pressed = {};
    const ui::AiPanelHit hit = s.renderer.AiPanel().HitTest(static_cast<float>(x), static_cast<float>(y));
    a.hover = hit;
    if (a.open && hit == pressed) Activate(s, hit);
    Redraw(s);
    return true;
}

bool AiAssistantMouseOther(AppState& s, int x, int y) {
    const AiAssistantState& a = s.aiAssistant;
    return a.open && s.renderer.AiPanel().Contains(static_cast<float>(x), static_cast<float>(y));
}

bool AiAssistantWheel(AppState& s, int x, int y, int delta) {
    if (!AiAssistantMouseOther(s, x, y)) return false;
    if (delta != 0) {
        // Three 16 px lines a notch, like the page's list.
        s.renderer.AiPanel().ScrollBy(-static_cast<float>(delta) / WHEEL_DELTA * 48.0f * s.scale);
        Redraw(s);
    }
    return true;
}

void AiAssistantMouseLeave(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.hover.part == ui::AiPanelPart::None || a.captured) return;
    a.hover = {};
    Redraw(s);
}

bool AiAssistantSetCursor(AppState& s, int x, int y) {
    const AiAssistantState& a = s.aiAssistant;
    if (!a.open || !s.renderer.AiPanel().Contains(static_cast<float>(x), static_cast<float>(y))) return false;
    const ui::AiPanelHit hit = s.renderer.AiPanel().HitTest(static_cast<float>(x), static_cast<float>(y));
    const bool text = hit.part == ui::AiPanelPart::Input || AiConnIsField(hit.part);
    SetCursor(LoadCursorW(nullptr, text ? IDC_IBEAM : IDC_ARROW));
    return true;
}

bool AiAssistantEscape(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return false;
    if (a.menu_open) {
        CloseMenu(s);
        Redraw(s);
        return true;
    }
    if (a.chat && a.chat->busy) {
        AiChatStop(s);
        return true;
    }
    return false;
}

HBRUSH AiAssistantCtlColor(AppState& s, HDC hdc, HWND edit) {
    AiAssistantState& a = s.aiAssistant;
    if (!edit) return nullptr;
    if (edit != a.edit) return AiConnCtlColor(s, hdc, edit);
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const COLORREF back = ToColorRef(view.CardColor());
    if (!a.brush || a.brush_color != back) {
        if (a.brush) DeleteObject(a.brush);
        a.brush = CreateSolidBrush(back);
        a.brush_color = back;
    }
    SetTextColor(hdc, ToColorRef(view.TextColor()));
    SetBkColor(hdc, back);
    SetBkMode(hdc, OPAQUE);
    return a.brush;
}

void AiAssistantDpiChanged(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AiConnDpiChanged(s);
    if (!a.edit) return;
    EnsureFont(s);
    ReadComposerMetrics(a);
    Redraw(s);
}

void AiAssistantDestroy(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (s.hwnd) KillTimer(s.hwnd, kTipTimer);
    AiConnDestroy(s);
    if (a.edit) DestroyWindow(a.edit);
    a.edit = nullptr;
    if (a.font) DeleteObject(a.font);
    a.font = nullptr;
    if (a.brush) DeleteObject(a.brush);
    a.brush = nullptr;
}

} // namespace pulse
