// batch_rename_dialog.h — Modal 批量重命名 dialog (steps + live preview).
#pragma once
#include "../app/batch_rename.h"
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>

namespace pulse::ui {

struct BatchRenameDialogResult {
    bool accepted = false;
    app::BatchRenameRule rule;                 // legacy; unused by the step dialog
    std::vector<app::BatchRenameItem> items;   // Ok items in a chain-safe order
};

struct BatchRenameDialogOptions {
    std::wstring ai_model;   // shown in the AI bar; empty = no connection
};

BatchRenameDialogResult ShowBatchRenameDialog(HWND owner,
                                              const std::vector<std::wstring>& paths,
                                              bool dark,
                                              D2D1_COLOR_F accent,
                                              const BatchRenameDialogOptions& options = {});

} // namespace pulse::ui
