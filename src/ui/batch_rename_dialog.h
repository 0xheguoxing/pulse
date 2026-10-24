// batch_rename_dialog.h — Modal 批量重命名 dialog (steps + live preview).
#pragma once
#include "../app/batch_rename.h"
#include <windows.h>
#include <d2d1.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct BatchRenameDialogResult {
    bool accepted = false;
    app::BatchRenameRule rule;                 // legacy; unused by the step dialog
    std::vector<app::BatchRenameItem> items;   // Ok items in a chain-safe order
    bool executed = false;                     // options.execute ran them (task below)
    uint64_t task = 0;
};

// What options.poll reports about a running rename task.
struct BatchRenameProgress {
    bool finished = false;
    size_t done = 0;   // items handled so far
};

struct BatchRenameDialogOptions {
    std::wstring ai_model;   // shown in the AI bar; empty = no connection
    // When set, Enter runs the renames while the dialog shows them going
    // through (progress line, a check per row) and closes when they are done.
    // Returns the task id (0: nothing started). Unset: the caller runs items.
    std::function<uint64_t(const std::vector<app::BatchRenameItem>&)> execute;
    std::function<BatchRenameProgress(uint64_t)> poll;
};

BatchRenameDialogResult ShowBatchRenameDialog(HWND owner,
                                              const std::vector<std::wstring>& paths,
                                              bool dark,
                                              D2D1_COLOR_F accent,
                                              const BatchRenameDialogOptions& options = {});

} // namespace pulse::ui
