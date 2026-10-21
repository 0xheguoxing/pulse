// tag_manager_panel.h — the 管理标签 panel.
//
// A Fluent flyout (the QuickAccessEditorPopup surface) listing every tag:
// check box, color, name, group, use count and the 常用 star. 全部 / 常用 /
// 未使用 filter the list; rows multi-select (click, Ctrl, Shift, Ctrl+A).
// Stars toggle in place; everything else (rename, merge, group, color,
// delete) goes back to the caller, which edits the catalog and reopens the
// panel where it was.
#pragma once

#include "FluentTokens.h"

#include <windows.h>
#include <string>
#include <vector>

namespace pulse::ui {

class Compositor;

struct TagManagerRow {
    std::wstring id;
    std::wstring name;
    std::wstring group;     // the group's name, empty when ungrouped
    D2D1_COLOR_F color{};
    int count = 0;          // tagged items
    int missing = 0;        // of them, gone from a drive that is there (count shown red)
    bool favorite = false;
    bool selected = false;
};

struct TagManagerText {
    std::wstring title;
    std::wstring hint;
    std::wstring filter_all;
    std::wstring filter_favorites;
    std::wstring filter_unused;
    std::wstring filter_missing;
    std::wstring merge;
    std::wstring group;
    std::wstring remove;
    std::wstring done;
    std::wstring empty;
};

enum class TagManagerFilter { All = 0, Favorites = 1, Unused = 2, Missing = 3 };

enum class TagManagerAction {
    Cancelled,  // Esc: star changes are dropped
    Closed,     // 完成, Enter, or a click outside: star changes are kept
    Rename,     // double-click / F2 on `focus`
    Merge,      // 合并… with two or more rows selected
    Group,      // 移到分组… for the selected rows
    Delete,     // 删除 / Del for the selected rows
    RowMenu,    // right-click: the selected rows (the clicked row included)
};

// Survives a reopen: the caller keeps it across Edit calls.
struct TagManagerView {
    TagManagerFilter filter = TagManagerFilter::All;
    float scroll = 0.0f;          // DIP
    std::wstring focus;           // focused row id
    std::wstring anchor;          // Shift range anchor id
    bool reopen = false;          // skip the entrance motion
    POINT action_pt{};            // screen point for the caller's follow-up menu
};

class TagManagerPopup {
public:
    // Modal; opens at screen_pt clamped to the work area. `rows` carries the
    // selection and favorite flags back.
    static TagManagerAction Edit(HWND owner, Compositor* compositor, float scale, POINT screen_pt,
                                 bool dark, D2D1_COLOR_F accent, const TagManagerText& text,
                                 std::vector<TagManagerRow>& rows, TagManagerView& view,
                                 int max_favorites);

    // GUI verification: render to a PNG without showing it; the second row
    // is drawn hovered.
    static bool SaveDebugSnapshot(Compositor* compositor, const wchar_t* png_path, bool dark,
                                  D2D1_COLOR_F accent, float scale, const TagManagerText& text,
                                  const std::vector<TagManagerRow>& rows,
                                  const TagManagerView& view);
};

} // namespace pulse::ui
