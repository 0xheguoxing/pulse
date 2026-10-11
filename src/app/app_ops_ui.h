// app_ops_ui.h — Delete/tray/paste/conflict/recycle operations UI.
#pragma once
#include "app_runtime.h"
#include "../ui/confirm_dialog.h"

namespace pulse {
bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request, uint64_t* task_id = nullptr);
void ReleaseTrayBatch(AppState& s, size_t idx);
void PasteIntoCurrent(AppState& s);
void DeleteSelected(AppState& s, bool permanent);
void DeletePaths(AppState& s, std::vector<std::wstring> paths, bool permanent);
// Confirmation shown before Delete moves items to the Recycle Bin (opt-in setting).
ui::ConfirmDialogSpec BuildRecycleDeleteConfirm(const std::vector<std::wstring>& paths);
void RestoreSelected(AppState& s);
// Restores every listed recycle-bin item (the filtered view when a filter is on).
void RestoreAllRecycle(AppState& s);
void EmptyRecycleBin(AppState& s);
void CollectToTray(AppState& s, bool move_intent);
void CollectPathsToTray(AppState& s, const std::vector<std::wstring>& paths, bool move_intent);
void ShowBatchRename(AppState& s);
// Staging tray recent drop destinations (appPrefs.tray_dests, newest first, max 3).
std::vector<std::wstring> TrayDestList(const AppState& s);
void RememberTrayDest(AppState& s, const std::wstring& dir);
// Copy (or move) every staged item that still exists into dir.
void SendTrayToDest(AppState& s, const std::wstring& dir, bool move);
// Batch rename an explicit path list (staging tray); tray entries follow.
void ShowBatchRenamePaths(AppState& s, const std::vector<std::wstring>& paths);
// Builds this window's operations window the first time it has to show it.
bool EnsureOperationWindow(AppState& s);
void PinAndShowOperationWindow(AppState& s);
void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog);
// Makes `to` the window that shows file operations (multi-window M4c, plan
// §8.1): ops notifications and shell dialogs go to it, and the progress
// window's state moves along, shown there without focus if it was showing.
void MoveOperationUi(AppState& from, AppState& to);
} // namespace pulse
