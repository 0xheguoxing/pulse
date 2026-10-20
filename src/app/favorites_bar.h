#pragma once
#include "app_runtime.h"

// 收藏栏 (toolbar redesign §8): the starred list (places.starred_items) laid
// out as a row under the command row; the sidebar's 收藏夹 is the same list
// standing up. Hidden by default, Ctrl+Shift+B or 自定义工具栏 shows it; while
// it is hidden the address bar ends in ★⌄, the same list as a dropdown.
namespace pulse {
// Pushes the renderer flag and fills vm.favorites_bar. Called by BuildVm.
void FillFavoritesBar(AppState& s, ui::WindowViewModel& vm);
void ToggleFavoritesBar(AppState& s);
// Mouse handlers; each returns true when it consumed the hit. A click opens
// the favorite (Ctrl: a new tab, Shift: the other pane) on release; dragging
// it along the bar reorders (the toolbar pins' motion), down and out removes
// it (the snack undoes). ⌄ lists a folder's subfolders, » the favorites
// without room, the address bar's ★⌄ the whole list.
bool HandleFavoritesBarPress(AppState& s, const ui::HitTestResult& hit);
bool HandleFavoritesBarContextMenu(AppState& s, const ui::HitTestResult& hit, POINT screen);
bool FavoritesBarDragging();
// True once the press became a drag (the hover then goes away).
bool MoveFavoritesBarDrag(AppState& s, POINT client);
void EndFavoritesBarDrag(AppState& s, bool cancel);
// WM_FAVORITE_UNDO: the favorite dragged off the bar comes back in its place.
void UndoFavoriteRemoval(AppState& s);
// Middle click on a favorite folder: the path to open in a new tab ("" none).
std::wstring FavoritesBarMiddleClickPath(const AppState& s, const ui::HitTestResult& hit);
// The folder a drop on this favorite goes into ("" for a file, or one that
// cannot be reached (*offline): no drop).
std::wstring FavoritesBarDropFolder(const AppState& s, const ui::HitTestResult& hit, bool* offline = nullptr);
// The favorite's path, and why it is dimmed when it cannot be reached.
std::wstring FavoritesBarTooltip(const AppState& s, const ui::HitTestResult& hit);
// 收藏夹分组 (P2): a group is one entry on the bar (a dropdown of its folders)
// and a folding row in the sidebar's 收藏夹. Its menu: the name as an edit
// field, 全部在新标签页打开, the separator after it, 取消分组.
void ShowStarredGroupMenu(AppState& s, const std::wstring& group_id, POINT screen);
// 放进分组 › 新建分组…: a group holding this starred folder, then its menu to name it.
void CreateStarredGroupAndEdit(AppState& s, const std::wstring& path, POINT screen);
// Middle click: a group's path (pulse:star-group:) opens all its folders in
// new tabs; false for any other path.
bool OpenFavoritesGroupTabs(AppState& s, const std::wstring& path);
}
