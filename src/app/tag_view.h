// tag_view.h — clicks on a tag view's condition bar, and sidebar Ctrl/Shift
// clicks that combine tags (pulse_tag_redesign §4, §5).
#pragma once
#include "app_runtime.h"

namespace pulse {
// Sidebar tag row released without a drag: Ctrl adds the tag to (or drops it
// from) the open tag view's combination, Shift adds it as 不含. False for a
// plain click, or when no tag view is open (the row then opens as usual).
bool CombineSidebarTagClick(AppState& s, const std::wstring& tag_path);
// Condition bar chip: the term menu (且 / 或 / 不含 / 移除), the 位置 menu, or
// ＋ 条件 (the tags not in the query).
void HandleTagQueryChip(AppState& s, int index, POINT screen_pt);
void HandleTagQueryChipRemove(AppState& s, int index);
// 细分 chip: narrows to the tag (Shift: without it) or to the folder.
void HandleTagQueryRefine(AppState& s, int index, bool exclude);
// The tags a drop on the active tag view would add: its 且 terms.
bool TagViewDropTags(const AppState& s, const std::wstring& view_path, std::vector<std::wstring>* tag_ids);
// Files dropped on a tag (sidebar row) or a tag view: the tags go on, then
// the snack 已给 N 项加上「…」 · 撤销 (§4). Undo takes off only what the drop
// added (WM_TAG_DROP_UNDO).
void ApplyDropTags(AppState& s, const std::vector<std::wstring>& tag_ids, const std::vector<std::wstring>& paths);
void UndoDropTags(AppState& s);
// Takes these paths off these tags with the snack 已移除 N 项找不到的文件 · 撤销
// (the same undo).
// Alt+N / Alt+0 (the keyboard only): the change, then a snack that undoes
// exactly the (tag, item) pairs it touched.
void ToggleTagWithUndo(AppState& s, const std::wstring& tag_id, const std::vector<std::wstring>& paths);
void ClearTagsWithUndo(AppState& s, const std::vector<std::wstring>& paths);
// A row's tag dot: this folder shows only that tag (the filter box gets
// #name); the same dot again clears it. False for any other hit.
bool HandleRowTagDotPress(AppState& s, const ui::HitTestResult& hit);
void RemoveTagPathsUndoable(AppState& s,
                            const std::vector<std::pair<std::wstring, std::vector<std::wstring>>>& removals);
}
