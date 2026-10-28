// quick_access_editor.h — the 编辑快速访问 panel.
//
// A Fluent flyout (layered Direct2D window, the ColorPickerPopup pattern) that
// lists the quick-access rows in sidebar order: built-in links with a show/hide
// check box, pinned folders with a remove button. Rows reorder by dragging (or
// Alt+Up/Down); 恢复默认 brings back the default links and order and keeps the
// pinned folders. The panel only edits the rows; the caller stores them.
#pragma once

#include "FluentTokens.h"

#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

namespace pulse::ui {

class Compositor;

struct QuickAccessEditorRow {
    std::wstring key;              // layout key: L"builtin:<name>" or the folder path
    std::wstring label;
    std::wstring detail;           // dimmed text after the label (a pin's folder path)
    std::wstring glyph;
    D2D1_COLOR_F color{};
    bool builtin = false;
    bool visible = true;           // built-in links only; pinned folders always show
    bool default_visible = true;   // what 恢复默认 restores
    int default_index = 0;         // a built-in link's place in a fresh sidebar
};

struct QuickAccessEditorText {
    std::wstring title;
    std::wstring hint;
    std::wstring reset;
    std::wstring add_folder;
    std::wstring done;
};

enum class QuickAccessEditorResult {
    Cancelled,   // Esc: the rows are back to what the caller passed in
    Applied,     // 完成, Enter, or a click outside the panel
    AddFolder,   // 添加文件夹…: the rows hold the edits so far
};

// 恢复默认: built-in links first in default_index order with their default
// visibility, then the pinned folders in their current order.
void ResetQuickAccessEditorRows(std::vector<QuickAccessEditorRow>& rows);
// Moves row `from` to index `to` (clamped); returns whether anything moved.
bool MoveQuickAccessEditorRow(std::vector<QuickAccessEditorRow>& rows, size_t from, size_t to);

class QuickAccessEditorPopup {
public:
    // Modal; the panel opens at screen_pt, clamped to the work area. `rows` is
    // edited in place: order, visibility, and removed pins (dropped from the
    // vector). For AddFolder the caller picks a folder, appends a row and
    // calls Edit again with the same rows.
    static QuickAccessEditorResult Edit(HWND owner, Compositor* compositor, float scale,
                                        POINT screen_pt, bool dark, D2D1_COLOR_F accent,
                                        const QuickAccessEditorText& text,
                                        std::vector<QuickAccessEditorRow>& rows);

    // GUI verification: render the panel to a PNG without showing it. The
    // second row is drawn hovered.
    static bool SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path, bool dark,
                                  D2D1_COLOR_F accent, float scale,
                                  const QuickAccessEditorText& text,
                                  const std::vector<QuickAccessEditorRow>& rows);
};

} // namespace pulse::ui
