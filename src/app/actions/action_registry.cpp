// action_registry.cpp - The action table and what each action does now.
//
// P1 wires the actions the palette offers first (navigation, tabs, windows,
// view, selection, the common file commands, the tray, settings); the rest of
// the inventory joins in P4. Rows only call what their menu row or key calls.
#include "action_registry.h"

#include "action_inventory.h"
#include "action_settings.h"
#include "../app_internal.h"
#include "../app_messages.h"
#include "../context_menu.h"
#include "../favorites_bar.h"
#include "../keymap.h"
#include "../places.h"
#include "../reopen_closed.h"
#include "../tag_query.h"
#include "../tag_view.h"
#include "../vertical_tabs.h"
#include "../window_overview_ui.h"
#include "../../common/localization.h"
#include "../../fs/fs_enum.h"
#include "../../ui/ui_renderer.h"

#include <iterator>
#include <string_view>
#include <utility>

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;
using P = ui::fluent::MenuPictogram;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

// ---- What an action needs; each marks the view unavailable with a reason --

bool Off(ActionView& v, I why) {
    v.enabled = false;
    v.why = Text(why);
    return false;
}

bool NeedSelection(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    return tab && tab->SelectedCount() > 0 ? true : Off(v, I::ActWhySelect);
}

bool NeedOne(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    return tab && tab->SelectedCount() == 1 ? true : Off(v, I::ActWhySelectOne);
}

bool InFolder(AppState& s) {
    const Tab* tab = ActiveTab(s);
    return tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path);
}

bool NeedFolder(AppState& s, ActionView& v) {
    return InFolder(s) ? true : Off(v, I::ActWhyNotFolder);
}

bool NeedPanes(AppState& s, ActionView& v) {
    return LiveLayout(s).VisiblePanes().size() > 1 ? true : Off(v, I::ActWhyOnePane);
}

std::wstring OnOff(bool on) { return Text(on ? I::AiSetOn : I::AiSetOff); }

// The radio row's label of a choice list ("" when none is current).
std::wstring Current(const std::vector<ui::FluentMenuItem>& rows) {
    for (const auto& row : rows)
        if (row.radio) return row.text;
    return {};
}

BackgroundViewOptions ViewOptions(AppState& s, const Tab& tab) {
    std::wstring kind;
    ParsePulsePath(tab.current_path, &kind, nullptr);
    const bool search = kind == L"search" || kind == L"saved-search";
    const bool real = !tab.current_path.empty() && !fs::IsVirtualPath(tab.current_path);
    BackgroundViewOptions o;
    o.view_mode = tab.view_mode;
    o.sort_column = tab.sort_column;
    o.sort_direction = tab.sort_direction;
    o.folder_sort = s.appPrefs.folder_sort_mode;
    o.can_sort = kind != L"starred" && kind != L"recent";
    o.indexed_search = search && !tab.content_results;
    o.show_path = search || kind == L"recycle";
    o.group_virtual = search || IsTagViewKind(kind) || kind == L"recycle";
    o.can_group = !tab.content_results && (real || kind == L"recent" || o.group_virtual);
    o.group_by = tab.group_by;
    o.can_apply_group_all = !tab.content_results && real;
    return o;
}

// ---- The table -------------------------------------------------------------

using Run = void (*)(AppState&);
using Fill = void (*)(AppState&, ActionView&);

struct Def {
    std::string_view id;
    I title;
    I domain;
    const wchar_t* glyph;
    P pictogram;
    const wchar_t* chord;  // default key; shown as the user's key now
    int command;           // the MenuCmd it runs, or 0 for `run`
    Run run;
    Fill fill;             // enabled / why / value / choices; nullptr: always on
};

void Post(AppState& s, UINT message) {
    // Posted like the keys do: windows are not made or closed under the palette.
    PostMessageW(s.hwnd, message, 0, 0);
}

void ShowTagPickerBelowAddress(AppState& s) {
    const D2D1_RECT_F addr = s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width()));
    POINT pt{static_cast<LONG>(addr.left), static_cast<LONG>(addr.bottom)};
    ClientToScreen(s.hwnd, &pt);
    ShowTagPicker(s, pt);
}

void FillViewMode(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    auto rows = BuildViewMenu(tab->view_mode, false, false, tab->wall);
    // The view modes and 照片墙; the panel and apply-to-all rows have their own actions.
    while (!rows.empty() && rows.back().command != CmdViewWall) rows.pop_back();
    if (!rows.empty()) rows.back().separator_after = false;
    v.value = Current(rows);
    v.choices = std::move(rows);
}

void FillSort(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    const BackgroundViewOptions o = ViewOptions(s, *tab);
    if (!o.can_sort) { Off(v, I::ActWhyNoSort); return; }
    v.choices = BuildSortMenu(o);
    v.value = Current(v.choices);
    if (!v.value.empty())
        v.value += tab->sort_direction == ui::SortDirection::Asc ? L" \x2191" : L" \x2193";
}

void FillSortDirection(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    if (!ViewOptions(s, *tab).can_sort) { Off(v, I::ActWhyNoSort); return; }
    const bool asc = tab->sort_direction == ui::SortDirection::Asc;
    v.value = Text(asc ? I::SortAscending : I::SortDescending) + L" \x2192 " +
              Text(asc ? I::SortDescending : I::SortAscending);
}

void RunSortDirection(AppState& s) {
    const Tab* tab = ActiveTab(s);
    if (!tab) return;
    DispatchMenuCommand(s, tab->sort_direction == ui::SortDirection::Asc ? CmdSortDescending
                                                                          : CmdSortAscending);
}

void FillGroup(AppState& s, ActionView& v) {
    const Tab* tab = ActiveTab(s);
    if (!tab) { Off(v, I::ActWhyNotFolder); return; }
    const BackgroundViewOptions o = ViewOptions(s, *tab);
    if (!o.can_group) { Off(v, I::ActWhyNoGroup); return; }
    v.choices = BuildGroupMenu(o).children;
    v.value = Current(v.choices);
}

void FillLayout(AppState& s, ActionView& v) {
    v.choices = BuildSplitMenu(static_cast<int>(LayoutOf(s)));
    v.value = Current(v.choices);
    for (const auto& row : v.choices)
        if (row.radio) v.pictogram = row.pictogram;
}

void FillPaste(AppState& s, ActionView& v) {
    if (!NeedFolder(s, v)) return;
    if (!ClipboardHasFiles()) Off(v, I::ActWhyNoPaste);
}

void FillTargetCopy(AppState& s, ActionView& v) {
    if (NeedPanes(s, v)) NeedSelection(s, v);
}

void FillUndo(AppState& s, ActionView& v) {
    if (!s.ops.CanUndo()) Off(v, I::ActWhyNoUndo);
}

void FillCompare(AppState& s, ActionView& v) {
    if (!NeedPanes(s, v)) return;
    const LayoutTab* lt = s.window_tabs.Active();
    v.value = OnOff(lt && lt->compare);
}

void FillDetails(AppState& s, ActionView& v) { v.value = OnOff(s.showDetailsPanel); }
void FillFolderSizes(AppState& s, ActionView& v) { v.value = OnOff(s.appPrefs.folder_size_auto); }
void FillSelection(AppState& s, ActionView& v) { NeedSelection(s, v); }
void FillOne(AppState& s, ActionView& v) { NeedOne(s, v); }
void FillFolder(AppState& s, ActionView& v) { NeedFolder(s, v); }
void FillPanes(AppState& s, ActionView& v) { NeedPanes(s, v); }

constexpr const wchar_t* kNoKey = nullptr;
constexpr Run kNoRun = nullptr;
constexpr Fill kAlways = nullptr;

constexpr Def kDefs[] = {
    // 导航
    {"nav.back", I::Back, I::ActDomNav, L"\xE72B", P::None, L"Alt+Left", 0,
     +[](AppState& s) { GoBack(s); }, kAlways},
    {"nav.forward", I::Forward, I::ActDomNav, L"\xE72A", P::None, L"Alt+Right", 0,
     +[](AppState& s) { GoForward(s); }, kAlways},
    {"nav.up", I::Up, I::ActDomNav, L"\xE74A", P::None, L"Alt+Up", 0,
     +[](AppState& s) { GoUp(s); }, kAlways},
    {"nav.refresh", I::Refresh, I::ActDomNav, L"\xE72C", P::None, L"F5", CmdRefresh, kNoRun, kAlways},
    {"nav.recent_changes", I::ActRecentChanges, I::ActDomNav, L"\xE81C", P::None, kNoKey,
     CmdViewRecentChanges, kNoRun, FillFolder},
    {"nav.recycle_bin", I::ActOpenRecycle, I::ActDomNav, L"\xE75C", P::None, kNoKey,
     CmdOpenRecycle, kNoRun, kAlways},
    // 标签页
    {"tab.new", I::TooltipNewTab, I::ActDomTabs, L"\xE710", P::None, L"Ctrl+T", 0,
     +[](AppState& s) { OpenNewTab(s); }, kAlways},
    {"tab.close", I::TabClose, I::ActDomTabs, L"\xE711", P::None, L"Ctrl+W", 0,
     +[](AppState& s) { CloseActiveTab(s); }, kAlways},
    {"window.reopen", I::ActReopenClosed, I::ActDomTabs, L"\xE7A7", P::None, L"Ctrl+Shift+T", 0,
     +[](AppState& s) { ReopenLastClosed(s); }, kAlways},
    {"tab.to_window", I::ActTabToWindow, I::ActDomTabs, L"\xE8A7", P::None, L"Ctrl+Shift+M", 0,
     +[](AppState& s) { Post(s, WM_MOVE_TAB_TO_NEW_WINDOW); }, kAlways},
    // 窗口与分栏
    {"window.layout", I::ActLayout, I::ActDomWindow, nullptr, P::LayoutSideBySide, kNoKey, 0,
     kNoRun, FillLayout},
    {"pane.mark_target", I::ActMarkTarget, I::ActDomWindow, L"\xE7C1", P::None, L"Ctrl+Alt+D", 0,
     +[](AppState& s) { MarkTargetPane(s); }, FillPanes},
    {"window.new", I::ActNewWindow, I::ActDomWindow, L"\xE78B", P::None, L"Ctrl+N", 0,
     +[](AppState& s) { Post(s, WM_NEW_WINDOW); }, kAlways},
    {"window.close", I::ActCloseWindow, I::ActDomWindow, L"\xE8BB", P::None, L"Ctrl+Shift+W", 0,
     +[](AppState& s) { Post(s, WM_CLOSE); }, kAlways},
    {"window.overview", I::WindowOverviewTitle, I::ActDomWindow, L"\xE7C4", P::None, L"Ctrl+Shift+O", 0,
     +[](AppState& s) { ToggleWindowOverview(s); }, kAlways},
    {"window.merge", I::ActMergeWindows, I::ActDomWindow, L"\xE8C8", P::None, L"Ctrl+Shift+J", 0,
     +[](AppState& s) { MergeAllWindows(s); }, kAlways},
    {"window.maximize", I::ActMaximize, I::ActDomWindow, L"\xE922", P::None, L"F11", 0,
     +[](AppState& s) { ShowWindow(s.hwnd, IsZoomed(s.hwnd) ? SW_RESTORE : SW_MAXIMIZE); }, kAlways},
    {"sidebar.toggle", I::ActSidebar, I::ActDomWindow, L"\xE700", P::None, L"Ctrl+B", 0,
     +[](AppState& s) { ToggleSidebarCollapsed(s); }, kAlways},
    {"favbar.toggle", I::ActFavoritesBar, I::ActDomWindow, L"\xE734", P::None, L"Ctrl+Shift+B", 0,
     +[](AppState& s) { ToggleFavoritesBar(s); }, kAlways},
    {"details.toggle", I::DetailsPane, I::ActDomWindow, L"\xE946", P::None, kNoKey, CmdDetailsPanel,
     kNoRun, FillDetails},
    // 视图
    {"view.mode", I::ActViewMode, I::ActDomView, L"\xE8A9", P::None, kNoKey, 0, kNoRun, FillViewMode},
    {"view.sort", I::SortBy, I::ActDomView, L"\xE8CB", P::None, kNoKey, 0, kNoRun, FillSort},
    {"view.sort", I::ActSortDirection, I::ActDomView, L"\xE8CB", P::None, kNoKey, 0,
     RunSortDirection, FillSortDirection},
    {"view.group", I::GroupBy, I::ActDomView, L"\xF168", P::None, kNoKey, 0, kNoRun, FillGroup},
    {"view.folder_sizes", I::ActFolderSizes, I::ActDomView, L"\xE8B7", P::None, kNoKey,
     CmdToggleFolderSizes, kNoRun, FillFolderSizes},
    {"view.columns", I::ColumnLayout, I::ActDomView, L"\xF5C1", P::None, kNoKey, CmdColumnLayout,
     kNoRun, kAlways},
    {"view.apply_all", I::ApplyViewAllFolders, I::ActDomView, L"\xF5B6", P::None, kNoKey,
     CmdApplyViewToAllFolders, kNoRun, FillFolder},
    {"view.filter", I::HelpFilter, I::ActDomView, L"\xE71C", P::None, L"Ctrl+F", 0,
     +[](AppState& s) { ShowFilterEditor(s); }, kAlways},
    {"view.compare", I::ActCompare, I::ActDomView, L"\xF5B5", P::None, kNoKey, CmdCompareToggle,
     kNoRun, FillCompare},
    // 选择
    {"select.all", I::SelectAll, I::ActDomSelect, L"\xE8B3", P::None, L"Ctrl+A", CmdSelectAll, kNoRun, kAlways},
    {"select.invert", I::InvertSelection, I::ActDomSelect, L"\xE7A1", P::None, L"Ctrl+I",
     CmdInvertSelection, kNoRun, kAlways},
    {"select.pattern", I::ActSelectPattern, I::ActDomSelect, L"\xF5B4", P::None, L"Ctrl+Shift+A",
     CmdSelectWildcard, kNoRun, kAlways},
    // 文件
    {"edit.undo", I::Undo, I::ActDomFiles, L"\xE7A7", P::None, L"Ctrl+Z", CmdUndo, kNoRun, FillUndo},
    {"file.copy", I::ActCopy, I::ActDomFiles, L"\xE8C8", P::None, L"Ctrl+C", CmdCopy, kNoRun, FillSelection},
    {"file.cut", I::Cut, I::ActDomFiles, L"\xE8C6", P::None, L"Ctrl+X", CmdCut, kNoRun, FillSelection},
    {"file.paste", I::Paste, I::ActDomFiles, L"\xE77F", P::None, L"Ctrl+V", CmdPaste, kNoRun, FillPaste},
    {"file.copy_path", I::CopyPath, I::ActDomFiles, L"\xF5B2", P::None, L"Ctrl+Shift+C", CmdCopyPath,
     kNoRun, kAlways},
    {"file.copy_to", I::CopyToTarget, I::ActDomFiles, L"\xF5B8", P::None, L"Ctrl+Alt+C", CmdCopyToTarget,
     kNoRun, FillTargetCopy},
    {"file.move_to", I::MoveToTarget, I::ActDomFiles, L"\xF5B9", P::None, L"Ctrl+Alt+X", CmdMoveToTarget,
     kNoRun, FillTargetCopy},
    {"file.new_folder", I::NewFolder, I::ActDomFiles, L"\xE8F4", P::None, L"Ctrl+Shift+N", CmdNewFolder,
     kNoRun, FillFolder},
    {"file.new_file", I::NewTextDocument, I::ActDomFiles, L"\xE8A5", P::None, kNoKey, CmdNewTextFile,
     kNoRun, FillFolder},
    {"file.rename", I::Rename, I::ActDomFiles, L"\xE8AC", P::None, L"F2", CmdRename, kNoRun, FillOne},
    {"file.batch_rename", I::BatchRename, I::ActDomFiles, L"\xF5B3", P::None, L"Ctrl+Shift+R",
     CmdBatchRename, kNoRun, kAlways},
    {"file.group_into_folder", I::ActGroupIntoFolder, I::ActDomFiles, L"\xF5BB", P::None, L"Ctrl+G",
     CmdGroupIntoFolder, kNoRun, FillSelection},
    {"file.dissolve", I::ActDissolveFolder, I::ActDomFiles, L"\xF5BB", P::None, L"Ctrl+Shift+G",
     CmdDissolveFolder, kNoRun, FillSelection},
    {"file.zip", I::TrayMenuZip, I::ActDomFiles, L"\xF012", P::None, kNoKey, CmdCompressZip, kNoRun,
     FillSelection},
    {"file.recycle", I::Delete, I::ActDomFiles, L"\xE74D", P::None, L"Delete", CmdDelete, kNoRun,
     FillSelection},
    {"file.properties", I::Properties, I::ActDomFiles, L"\xE946", P::None, L"Alt+Enter", CmdProperties,
     kNoRun, kAlways},
    {"file.terminal", I::OpenInTerminal, I::ActDomFiles, L"\xE756", P::None, kNoKey, CmdOpenTerminal,
     kNoRun, kAlways},
    // 标签与收藏
    {"tag.panel", I::ActTagPanel, I::ActDomTags, L"\xE8EC", P::None, L"Alt+T", 0,
     ShowTagPickerBelowAddress, FillSelection},
    {"tag.clear", I::ActClearTags, I::ActDomTags, L"\xE8EC", P::None, L"Alt+0", 0,
     +[](AppState& s) { if (const Tab* tab = ActiveTab(s)) ClearTagsWithUndo(s, SelectedFullPaths(*tab)); },
     FillSelection},
    {"workspace.pin", I::PinWorkspace, I::ActDomTags, L"\xF5BE", P::None, kNoKey, CmdPinWorkspace,
     kNoRun, kAlways},
    {"quick_access.pin", I::PinQuickAccess, I::ActDomTags, L"\xF5BD", P::None, kNoKey, CmdPinQuickAccess,
     kNoRun, FillFolder},
    {"alias.manage", I::ManageAliasesEllipsis, I::ActDomTags, L"\xE8EC", P::None, kNoKey,
     CmdManageAliases, kNoRun, kAlways},
    // 暂存区
    {"tray.add", I::ActStageToTray, I::ActDomTray, L"\xE7B8", P::None, L"Ctrl+Shift+S", CmdStageToTray,
     kNoRun, FillSelection},
    // 搜索
    {"search.advanced", I::AdvancedSearch, I::ActDomSearch, L"\xF5B7", P::None, L"Ctrl+Shift+F",
     CmdAdvancedSearch, kNoRun, kAlways},
    {"search.project", I::ActProject, I::ActDomSearch, L"\xE8B7", P::None, L"Ctrl+P", 0,
     +[](AppState& s) { ShowOmnibar(s, OmnibarMode::Project); }, kAlways},
    {"index.install_full", I::EnableFullIndex, I::ActDomSearch, L"\xE721", P::None, kNoKey,
     CmdInstallFullIndex, kNoRun, kAlways},
    // 预览
    {"preview.toggle", I::ActQuickPreview, I::ActDomPreview, L"\xE8FF", P::None, L"Space", 0,
     +[](AppState& s) { ToggleQuickPreview(s); }, kAlways},
    // 网络
    {"net.connect_server", I::ConnectServerMenu, I::ActDomNetwork, L"\xE968", P::None, kNoKey,
     CmdConnectServer, kNoRun, kAlways},
    {"net.map_drive", I::ActMapDrive, I::ActDomNetwork, L"\xE8CE", P::None, kNoKey, CmdMapNetworkDrive,
     kNoRun, kAlways},
    // 应用
    {"app.settings_page", I::ActOpenSettings, I::ActDomApp, L"\xE713", P::None, kNoKey, CmdSettings,
     kNoRun, kAlways},
    {"app.shortcut_help", I::ShortcutHints, I::ActDomApp, L"\xE765", P::None, L"Ctrl+/", CmdShortcutHelp,
     kNoRun, kAlways},
    {"app.exit", I::ExitPulse, I::ActDomApp, L"\xE7E8", P::None, kNoKey, CmdExitPulse, kNoRun, kAlways},
};

constexpr int kDefCount = static_cast<int>(std::size(kDefs));

constexpr bool AllIdsKnown() {
    for (const Def& d : kDefs)
        if (!FindAction(d.id)) return false;
    return true;
}
static_assert(AllIdsKnown(), "kDefs: an id is missing from action_inventory.h");

std::wstring SettingsDomain(int page) {
    std::wstring text = Text(I::ActDomSettings);
    const size_t at = text.find(L"%s");
    if (at != std::wstring::npos) text.replace(at, 2, SettingsPageTitle(page));
    return text;
}

// The settings page's own icon for each row (ui_settings_core.cpp).
constexpr std::pair<std::string_view, const wchar_t*> kSettingGlyphs[] = {
    {"theme", L"\xE706"}, {"window_effect", L"\xE81E"}, {"text_rendering", L"\xE8D2"},
    {"font_size", L"\xE8E9"}, {"language", L"\xE774"}, {"row_height", L"\xECE7"},
    {"click_to_open", L"\xE8B0"}, {"name_highlight", L"\xE7E6"}, {"blank_double_click", L"\xE72B"},
    {"folders_first", L"\xE8CB"}, {"alias_mark", L"\xE8AC"}, {"mtime_marks", L"\xE916"},
    {"mtime_range", L"\xEC92"}, {"startup_open", L"\xE81C"}, {"new_tab_open", L"\xECCD"},
    {"external_open", L"\xE8AD"}, {"tray_icon", L"\xECAA"}, {"open_folders_with_pulse", L"\xEC50"},
    {"staging_icon_size", L"\xED1E"}, {"change_days", L"\xE9D5"},
    {"menu_acrylic", L"\xE700"}, {"hover_select", L"\xE815"}, {"item_checkboxes", L"\xE73A"},
    {"show_hidden", L"\xED1A"}, {"show_protected", L"\xEA18"}, {"smart_dates", L"\xE787"},
    {"zebra_rows", L"\xE7BC"}, {"size_bars", L"\xE9D2"}, {"tag_name_color", L"\xE8EC"},
    {"selection_outline", L"\xE8B3"}, {"thumbnail_app_badges", L"\xE8B9"}, {"mtime_dim_older", L"\xE793"},
    {"mtime_bold_today", L"\xE8DD"}, {"mtime_folders", L"\xE8B7"}, {"mtime_other_views", L"\xE8A9"},
    {"mtime_flash", L"\xE945"}, {"onedrive_status", L"\xE753"}, {"svn_status", L"\xE8AB"},
    {"vertical_tabs", L"\xE8A0"}, {"pinned_tab_names", L"\xE718"}, {"double_click_closes_tab", L"\xEA39"},
    {"close_window_with_last_tab", L"\xE89F"}, {"launch_at_signin", L"\xEC57"}, {"start_in_tray", L"\xE896"},
    {"keep_running", L"\xE9F5"}, {"explorer_tag_menu", L"\xE838"}, {"staging_hide_when_empty", L"\xE8F6"},
    {"change_tracking", L"\xE823"}, {"lan_discovery", L"\xE701"}, {"network_recent", L"\xE81C"},
    {"hidden_shares", L"\xED1A"}, {"confirm_delete", L"\xE897"}, {"global_search", L"\xE721"},
    {"pinyin_search", L"\xE721"},
};

const wchar_t* SettingGlyph(std::string_view key) {
    for (const auto& [k, glyph] : kSettingGlyphs)
        if (k == key) return glyph;
    return L"\xE713";
}

ActionView SettingView(AppState& s, const SetDef& d, int index) {
    ActionView v;
    v.index = index;
    v.id = "settings.set";
    v.key = d.id;
    v.title = Text(d.label);
    v.domain = SettingsDomain(d.page);
    v.glyph = SettingGlyph(d.id);
    const int now = d.get(s);
    v.value = SettingText(d, now);
    if (!IsSwitch(d)) {
        const int current = ChoiceIndex(d, now);
        for (size_t i = 0; i < d.choices.size() && d.choices[i].key; ++i) {
            ui::FluentMenuItem row;
            row.text = Text(d.choices[i].label);
            row.radio_group = true;
            row.radio = static_cast<int>(i) == current;
            v.choices.push_back(std::move(row));
        }
    }
    return v;
}

} // namespace

std::vector<ActionView> ListActions(AppState& s) {
    const auto settings = SettingDefs();
    std::vector<ActionView> out;
    out.reserve(static_cast<size_t>(kDefCount) + settings.size());
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        ActionView v;
        v.index = i;
        v.id = d.id;
        v.title = Text(d.title);
        v.domain = Text(d.domain);
        if (d.glyph) v.glyph = d.glyph;
        v.pictogram = d.pictogram;
        if (d.chord) v.shortcut = ShownShortcut(d.chord);
        if (d.fill) d.fill(s, v);
        out.push_back(std::move(v));
    }
    for (size_t i = 0; i < settings.size(); ++i)
        out.push_back(SettingView(s, settings[i], kDefCount + static_cast<int>(i)));
    return out;
}

bool RunAction(AppState& s, int index, int choice) {
    if (index < 0) return false;
    if (index >= kDefCount) {
        const auto settings = SettingDefs();
        const size_t at = static_cast<size_t>(index - kDefCount);
        if (at >= settings.size()) return false;
        const SetDef& d = settings[at];
        if (IsSwitch(d)) {
            ApplySetting(s, d, d.get(s) != 0 ? 0 : 1);
            return true;
        }
        if (choice < 0 || choice >= static_cast<int>(d.choices.size()) || !d.choices[static_cast<size_t>(choice)].key)
            return false;
        ApplySetting(s, d, choice);
        return true;
    }
    const Def& d = kDefs[index];
    ActionView v;
    if (d.fill) d.fill(s, v);
    if (!v.enabled) return false;
    if (choice >= 0) {
        if (choice >= static_cast<int>(v.choices.size())) return false;
        const auto& row = v.choices[static_cast<size_t>(choice)];
        if (!row.enabled || row.command == CmdNone) return false;
        DispatchMenuCommand(s, row.command);
        return true;
    }
    if (d.command != 0) DispatchMenuCommand(s, d.command);
    else if (d.run) d.run(s);
    else return false;
    return true;
}

int ActionOfMenuCommand(AppState& s, int command) {
    if (command == CmdNone) return -1;
    for (int i = 0; i < kDefCount; ++i) {
        const Def& d = kDefs[i];
        if (d.command == command) return i;
        if (!d.fill || d.command != 0 || d.run) continue;  // only choice actions remain
        ActionView v;
        d.fill(s, v);
        for (const auto& row : v.choices)
            if (row.command == command) return i;
    }
    return -1;
}

} // namespace pulse::app::actions
