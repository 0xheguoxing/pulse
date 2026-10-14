// toolbar_customize_dialog.h — the 「自定义工具栏」 sheet (toolbar redesign note 5):
// the New button's main action (radio rows) and the commands pinned to the
// toolbar (check rows). The caller supplies the rows; the dialog only edits
// their checked state.
#pragma once

#include <d2d1.h>
#include <windows.h>

#include <string>
#include <vector>

namespace pulse::ui {

struct ToolbarCustomizeRow {
    std::wstring glyph;     // pin rows: command icon; radio rows: empty
    std::wstring label;
    std::wstring caption;   // right-aligned: a shortcut, or "always shown"
    bool locked = false;    // shown checked and not changeable
    bool checked = false;
};

struct ToolbarCustomizeSpec {
    std::wstring title;
    std::wstring intro;
    std::wstring primary_heading;
    std::wstring pin_heading;
    std::wstring done_text;
    std::wstring cancel_text;
    std::vector<ToolbarCustomizeRow> primary;   // one of them checked
    std::vector<ToolbarCustomizeRow> pins;
};

// Modal. True when the user pressed Done; `spec` then holds the choices.
bool ShowToolbarCustomizeDialog(HWND owner, ToolbarCustomizeSpec& spec, bool dark,
                                D2D1_COLOR_F accent);

} // namespace pulse::ui
