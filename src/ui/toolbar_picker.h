// toolbar_picker.h — the 「自定义工具栏」 popover (toolbar picker T3,
// pulse_toolbar_picker.html v3). It hangs under the toolbar's More button and
// shows the toolbar's tools and pinned apps as tiles, one page each. Every
// change applies at once through the callbacks, so the toolbar behind the
// popover updates while it is open; there is no Cancel.
#pragma once

#include <d2d1.h>
#include <windows.h>

#include "../common/menu_icon.h"

#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct ToolbarPickerTile {
    std::wstring glyph;          // tools: the command icon
    std::wstring label;
    std::wstring caption;        // apps: the file types it opens (tooltip line)
    std::wstring keywords;       // tools: extra search words (shortcut, stable id)
    common::MenuIconRef icon;    // apps: the program icon (instead of glyph)
    std::wstring badge;          // apps: an accent note under the name (此文件夹常用)
    std::wstring value;          // apps: the program path, returned untouched
    int section = 0;             // tools: index into ToolbarCustomizeSpec::sections
    bool checked = false;        // on the toolbar
    bool toggle = false;         // tools: the tile carries a switch for its setting
    bool toggle_on = false;
};
using ToolbarCustomizeRow = ToolbarPickerTile;

struct ToolbarCustomizeSpec {
    std::wstring title;          // 自定义工具栏
    std::wstring done_text;      // 完成
    std::wstring tools_tab;      // 工具
    std::wstring apps_tab;       // 应用
    std::wstring tools_desc;
    std::wstring apps_desc;
    std::wstring search_hint;    // 搜索工具
    std::wstring empty_text;     // no tool matches the search
    std::wstring apps_full_text; // the toolbar holds max_apps apps
    std::vector<std::wstring> sections;
    std::vector<ToolbarPickerTile> pins;     // tools page
    // Apps page (FillToolbarAppRows): pinned apps (checked), then suggestions.
    std::wstring add_app_text;   // 添加软件…
    std::wstring add_app_caption;
    std::vector<ToolbarPickerTile> apps;
    size_t max_apps = 8;
    // 添加软件…: picks a program (owner = the popover). True fills `row`, which is
    // appended checked unless a row with the same value exists (that one is checked).
    std::function<bool(HWND owner, ToolbarPickerTile& row)> add_app;
    // Reading the app rows takes a while (file types, suggestions), so the
    // popover can fill them later: when the apps page is first needed, or
    // once the opening animation has settled. apps_pinned stands in for the
    // tab's count until then.
    std::function<void(ToolbarCustomizeSpec& spec)> load_apps;
    size_t apps_pinned = 0;

    int page = 0;                // 0 tools, 1 apps
    RECT anchor{};               // screen rect of the button the popover hangs from

    // Live edits. on_tool: pins[index] was checked / unchecked. on_switch:
    // flip the setting behind pins[index], returns its new state. on_apps:
    // the app tiles changed (checked state or a new row).
    std::function<void(size_t index, bool checked)> on_tool;
    std::function<bool(size_t index)> on_switch;
    std::function<void(const std::vector<ToolbarPickerTile>& apps)> on_apps;
};

// Runs until the popover closes (Esc, Done, a click elsewhere). Returns at
// once when a picker is already open.
void ShowToolbarPicker(HWND owner, ToolbarCustomizeSpec& spec, bool dark, D2D1_COLOR_F accent);
bool ToolbarPickerOpen() noexcept;

} // namespace pulse::ui
