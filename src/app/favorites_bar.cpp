#include "favorites_bar.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "../common/display_path.h"
#include "../common/localization.h"
#include "keymap.h"
#include "../ui/fluent_menu.h"
#include <algorithm>
#include <cmath>
#include <cwchar>

namespace pulse {
namespace {
constexpr int kFavoriteMenuBase = 36000;   // » menu: kFavoriteMenuBase + favorite index
constexpr int kIconOnlyCommand = 36900, kHideCommand = 36901;
constexpr wchar_t kFolderGlyph[] = L"\xE8B7";
constexpr wchar_t kFileGlyph[] = L"\xE8A5";

const app::StarredItem* FavoriteAt(const AppState& s, int index) {
    if (index < 0 || static_cast<size_t>(index) >= s.places.starred_items.size()) return nullptr;
    return &s.places.starred_items[static_cast<size_t>(index)];
}

// how: 0 here, 1 a new tab, 2 the other pane (a new tab without one).
void OpenFavorite(AppState& s, const app::StarredItem& item, int how) {
    const std::wstring path = item.path;
    if (item.kind == app::PlaceItemKind::File) {
        OpenPath(s, path);   // the default program
        return;
    }
    if (how == 2) {
        app::Pane* other = s.targetPane && s.targetPane != s.pane ? s.targetPane : nullptr;
        if (!other)
            for (app::Pane* pane : LiveLayout(s).VisiblePanes())
                if (pane && pane != s.pane) { other = pane; break; }
        if (other) {
            FocusPane(s, other);
            NavigateTo(s, path);
            return;
        }
        how = 1;
    }
    if (how == 1) OpenFolderTab(s, path);
    else NavigateTo(s, path);
}

int HowFromKeys() {
    if (GetKeyState(VK_SHIFT) & 0x8000) return 2;
    if (GetKeyState(VK_CONTROL) & 0x8000) return 1;
    return 0;
}

void SaveAndRepaint(AppState& s) {
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Right-click on the bar's empty part: icons only, hide.
void ShowFavoritesBarMenu(AppState& s, POINT screen) {
    if (!EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem icons;
    icons.command = kIconOnlyCommand;
    icons.text = l10n::Get(l10n::StringId::FavoritesBarIconOnly);
    icons.checked = s.appPrefs.favorites_bar_icon_only;
    items.push_back(std::move(icons));
    ui::FluentMenuItem hide;
    hide.command = kHideCommand;
    hide.text = l10n::Get(l10n::StringId::FavoritesBarHide);
    hide.glyph = L"\xED1A";   // hide
    hide.shortcut = app::ShownShortcut(L"Ctrl+Shift+B");
    items.push_back(std::move(hide));
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (cmd == kIconOnlyCommand) {
        s.appPrefs.favorites_bar_icon_only = !s.appPrefs.favorites_bar_icon_only;
        SaveAndRepaint(s);
    } else if (cmd == kHideCommand) {
        ToggleFavoritesBar(s);
    }
}

// »: the favorites without room on the bar, in list order.
void ShowFavoritesOverflow(AppState& s, int first, POINT screen) {
    if (!EnsureMenu(s)) return;
    const auto& list = s.places.starred_items;
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = static_cast<size_t>(std::max(first, 0)); i < list.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = kFavoriteMenuBase + static_cast<int>(i);
        item.text = app::TabTitle(list[i].path);
        item.glyph = list[i].kind == app::PlaceItemKind::File ? kFileGlyph : kFolderGlyph;
        items.push_back(std::move(item));
    }
    if (items.empty()) return;
    const int cmd = s.menu->TrackPopup(screen, std::move(items));
    if (const app::StarredItem* item = FavoriteAt(s, cmd - kFavoriteMenuBase); item && cmd >= kFavoriteMenuBase)
        OpenFavorite(s, app::StarredItem(*item), HowFromKeys());
}

POINT BelowControl(const AppState& s, const ui::HitTestResult& hit) {
    POINT pt{static_cast<LONG>(std::lround(hit.control_bounds.left)),
             static_cast<LONG>(std::lround(hit.control_bounds.bottom))};
    ClientToScreen(s.hwnd, &pt);
    return pt;
}
} // namespace

void FillFavoritesBar(AppState& s, ui::WindowViewModel& vm) {
    const bool on = s.appPrefs.favorites_bar && !s.renderer.Embedded();
    s.renderer.SetFavoritesBar(on);
    vm.favorites_bar_on = on;
    if (!on) return;
    vm.favorites_bar_icon_only = s.appPrefs.favorites_bar_icon_only;
    vm.favorites_bar_hint = l10n::Get(l10n::StringId::FavoritesBarEmpty);
    vm.favorites_bar_more = l10n::Get(l10n::StringId::FavoritesBarMore);
    const app::Tab* tab = ActiveTab(s);
    // Starred paths are normalized (\\?\C:\...); compare as display text.
    const std::wstring here = tab ? pulse::path::FriendlyPathText(tab->current_path) : std::wstring{};
    vm.favorites_bar.reserve(s.places.starred_items.size());
    for (const auto& entry : s.places.starred_items) {
        ui::WindowViewModel::FavoritesBarItem item;
        item.path = entry.path;
        item.label = app::TabTitle(entry.path);
        item.folder = entry.kind != app::PlaceItemKind::File;
        item.current = item.folder && !here.empty() &&
                       _wcsicmp(here.c_str(), pulse::path::FriendlyPathText(entry.path).c_str()) == 0;
        vm.favorites_bar.push_back(std::move(item));
    }
}

void ToggleFavoritesBar(AppState& s) {
    s.appPrefs.favorites_bar = !s.appPrefs.favorites_bar;
    s.renderer.SetFavoritesBar(s.appPrefs.favorites_bar && !s.renderer.Embedded());
    SaveAndRepaint(s);
}

bool HandleFavoritesBarPress(AppState& s, const ui::HitTestResult& hit) {
    if (hit.region == ui::HitTestResult::FavoritesBarMore) {
        ShowFavoritesOverflow(s, hit.index, BelowControl(s, hit));
        return true;
    }
    if (hit.region != ui::HitTestResult::FavoritesBarItem) return false;
    if (const app::StarredItem* item = FavoriteAt(s, hit.index))
        OpenFavorite(s, app::StarredItem(*item), HowFromKeys());
    return true;   // the bar's empty part does nothing
}

bool HandleFavoritesBarContextMenu(AppState& s, const ui::HitTestResult& hit, POINT screen) {
    if (hit.region != ui::HitTestResult::FavoritesBarItem && hit.region != ui::HitTestResult::FavoritesBarMore)
        return false;
    // A favorite: the same menu as its row in the sidebar's 收藏夹.
    if (const app::StarredItem* item = FavoriteAt(s, hit.index);
        item && hit.region == ui::HitTestResult::FavoritesBarItem) {
        ShowCuratedItemMenu(s, std::wstring(item->path), false, screen);
        return true;
    }
    ShowFavoritesBarMenu(s, screen);
    return true;
}

std::wstring FavoritesBarMiddleClickPath(const AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::FavoritesBarItem) return {};
    const app::StarredItem* item = FavoriteAt(s, hit.index);
    return item && item->kind != app::PlaceItemKind::File ? item->path : std::wstring{};
}
} // namespace pulse
