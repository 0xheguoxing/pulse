// toolbar_customize_dialog.h — the 「自定义工具栏」 sheet (toolbar redesign note 5):
// the New button's main action (radio rows), the commands pinned to the
// toolbar and the pinned apps (check rows). The caller supplies the rows; the
// dialog edits their checked state and, through add_app, appends app rows.
#pragma once

#include <d2d1.h>
#include <windows.h>

#include "../common/menu_icon.h"

#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct ToolbarCustomizeRow {
    std::wstring glyph;     // pin rows: command icon; radio rows: empty
    std::wstring label;
    std::wstring caption;   // right-aligned: a shortcut, "always shown", an app's file types
    common::MenuIconRef icon;   // app rows: the program icon (instead of glyph)
    std::wstring badge;     // app rows: an accent tag after the label (此文件夹常用)
    std::wstring value;     // app rows: the program path, returned untouched
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
    // Pinned apps (checked) and suggestions (unchecked); at most max_apps checked.
    std::wstring app_heading;
    std::wstring app_hint;        // after the heading: 单击打开选中项 · 拖文件到图标上打开
    std::wstring add_app_text;    // 添加软件…
    std::wstring add_app_caption;
    std::wstring app_footnote;    // under the rows, wrapped
    std::vector<ToolbarCustomizeRow> apps;
    size_t max_apps = 8;
    // 添加软件…: picks a program (owner = the sheet). True fills `row`, which is
    // appended checked unless a row with the same value exists (that one is checked).
    std::function<bool(HWND owner, ToolbarCustomizeRow& row)> add_app;
};

// Modal. True when the user pressed Done; `spec` then holds the choices.
bool ShowToolbarCustomizeDialog(HWND owner, ToolbarCustomizeSpec& spec, bool dark,
                                D2D1_COLOR_F accent);

} // namespace pulse::ui
