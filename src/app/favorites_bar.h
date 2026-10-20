#pragma once
#include "app_runtime.h"

// 收藏栏 (toolbar redesign §8): the starred list (places.starred_items) laid
// out as a row under the command row; the sidebar's 收藏夹 is the same list
// standing up. Hidden by default, Ctrl+Shift+B or 自定义工具栏 shows it.
namespace pulse {
// Pushes the renderer flag and fills vm.favorites_bar. Called by BuildVm.
void FillFavoritesBar(AppState& s, ui::WindowViewModel& vm);
void ToggleFavoritesBar(AppState& s);
// Mouse handlers; each returns true when it consumed the hit. A click opens
// the favorite (Ctrl: a new tab, Shift: the other pane); » lists the ones
// without room.
bool HandleFavoritesBarPress(AppState& s, const ui::HitTestResult& hit);
bool HandleFavoritesBarContextMenu(AppState& s, const ui::HitTestResult& hit, POINT screen);
// Middle click on a favorite folder: the path to open in a new tab ("" none).
std::wstring FavoritesBarMiddleClickPath(const AppState& s, const ui::HitTestResult& hit);
}
