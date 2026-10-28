// shortcuts_view.h — what Settings > 快捷键 (page 7) draws. Built by
// app::ShortcutSettings on the UI thread; immutable once published, so view
// models share it instead of copying ~50 rows per frame.
#pragma once

#include <d2d1.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {

// HitTestResult::SettingsShortcuts indices.
enum ShortcutsHit : int {
    kScSearch = 0,        // header: the search box (focus it)
    kScSearchClear = 1,   // header: ✕ in the search box
    kScFind = 2,          // header: 按键查找 (next chord filters the rows)
    kScTester = 3,        // header: 按键测试 popover
    kScMore = 4,          // header: ⋯ popover
    kScRecheck = 5,       // conflict strip: 重新检测
    kScResetAll = 6,      // filter bar: 全部恢复默认
    kScToastUndo = 7,     // toast: 撤销
    kScBackdrop = 8,      // a click outside the open popover: close it
    kScPopBody = 9,       // inside the popover but on no control: nothing
    kScFilterBase = 10,   // + 0 全部 / 1 有问题 / 2 已修改
    kScChipBase = 20,     // + conflict chip: open that row's info popover
    kScPopClose = 40,     // popover: ✕ / 取消
    kScPopSave = 41,      // popover: 保存 / 替换并保存
    kScPopRemove = 42,    // popover: 删除这个按键
    kScPopReset = 43,     // popover: 恢复默认
    kScPopIgnore = 44,    // info popover (IME): 已关闭，不再提示
    kScPopRecord = 45,    // info popover: 自己录一个
    kScPopModBase = 50,   // range popover: + 0 Ctrl / 1 Alt / 2 Shift
    kScPopSuggestBase = 60,  // popover: + suggestion (applied at once)
    kScMoreBase = 70,     // ⋯ popover: + item (see ShortcutsMoreItem)
    kScRowBase = 1000,    // + action: the row body (info for issues, else add a key)
    kScKeyBase = 2000,    // + action * 4 + slot: a binding (slot 3: the + button)
};

enum ShortcutsMoreItem : int { kScMoreCopy = 0, kScMorePaste = 1, kScMoreReset = 2, kScMoreCount = 3 };

// 0 ok, 1 info, 2 warn (amber), 3 block (red): app::ChordLevel values.
struct ShortcutBindingView {
    std::vector<std::wstring> caps;  // modifiers first, then the key
    uint8_t level = 0;
    std::wstring tip;                // why it is tinted (tooltip / info)
};

struct ShortcutRowView {
    int action = 0;
    std::wstring name;
    std::vector<ShortcutBindingView> bindings;
    std::wstring range_label;  // "1–9" for numbered groups
    bool fixed = false;        // lock, not editable
    bool range = false;
    bool global = false;       // 全局 tag
    bool global_off = false;   // the global hotkey is switched off
    bool modified = false;
    bool can_add = false;      // fewer than the maximum bindings
    bool found = false;        // 按键查找 hit
    uint8_t level = 0;         // worst binding
};

struct ShortcutGroupView {
    std::wstring title;
    std::vector<ShortcutRowView> rows;
    int issues = 0;
    uint8_t level = 0;
};

struct ShortcutChipView {
    int action = 0;
    std::wstring text;  // "复制到目标栏 · Ctrl+Alt+C"
    uint8_t level = 0;
};

struct ShortcutPopView {
    enum Kind : uint8_t { None, Info, Record, Range, Tester, More } kind = None;
    int action = -1;   // anchor row (None for Tester / More: header anchored)
    int slot = -1;
    std::wstring title;            // action name, or the popover's name
    std::wstring subtitle;         // current keys / hint
    std::vector<std::wstring> caps;  // Record / Tester: the chord pressed so far
    bool waiting = true;           // nothing pressed yet
    uint8_t level = 0;             // verdict colour
    std::wstring verdict;          // one line, bold
    std::wstring detail;           // wrapped, secondary
    std::vector<std::wstring> steps;  // Info (IME): how to switch the hotkey off
    std::vector<std::wstring> suggestions;
    std::wstring save_label;       // empty: no save button
    bool can_save = false;
    bool can_remove = false;
    bool can_reset = false;
    bool can_ignore = false;
    bool can_record = false;
    uint8_t range_mods = 0;        // Range: modifier toggles (app::kKey* bits)
    std::vector<std::wstring> digit_labels;  // Range: "Ctrl+1" ...
    std::vector<uint8_t> digit_levels;
    std::vector<std::wstring> more_items;    // More
};

struct ShortcutsView {
    std::vector<ShortcutGroupView> groups;  // filtered; empty groups dropped
    std::vector<ShortcutChipView> chips;
    int red = 0;
    int amber = 0;
    bool probing = false;          // 正在检测…
    bool ime_installed = false;
    int filter = 0;                // 0 全部 / 1 有问题 / 2 已修改
    int counts[3]{};
    std::wstring query;
    bool search_focused = false;
    bool finding = false;          // 按键查找: waiting for a chord
    std::vector<std::wstring> find_caps;  // the chord being looked up
    bool any_modified = false;
    ShortcutPopView pop;
    std::wstring toast;
    bool toast_undo = false;
};

// Geometry of page 7 (LayoutSettingsShortcuts in settings_shortcuts_layout.h).
struct ShortcutsRowLayout {
    D2D1_RECT_F rect{};
    int action = 0;
    int group = 0;
    int row = 0;
    std::vector<D2D1_RECT_F> keys;  // one per binding (range: the whole chord)
    D2D1_RECT_F add{};              // the + button (can_add)
    float name_right = 0.0f;
};

struct ShortcutsCardLayout {
    D2D1_RECT_F rect{}, head{};
    int group = 0;
    std::vector<ShortcutsRowLayout> rows;
};

struct ShortcutsLayout {
    D2D1_RECT_F lead{}, search{}, search_clear{}, find{}, tester{}, more{};
    D2D1_RECT_F strip{}, strip_text{}, recheck{};
    std::vector<D2D1_RECT_F> chips;
    D2D1_RECT_F seg[3]{}, reset_all{}, empty{};
    std::vector<ShortcutsCardLayout> cards;
    // The open popover (ShortcutsView::pop), all parts in page coordinates.
    D2D1_RECT_F pop_anchor{};  // the row or tool the popover belongs to
    D2D1_RECT_F pop{}, pop_title{}, pop_close{}, pop_sub{}, pop_box{}, pop_verdict{}, pop_detail{};
    D2D1_RECT_F pop_save{}, pop_cancel{}, pop_remove{}, pop_reset{}, pop_ignore{}, pop_record{};
    D2D1_RECT_F pop_mods[3]{}, pop_suggest_label{};
    std::vector<D2D1_RECT_F> pop_steps, pop_suggest, pop_digits, pop_more;
    D2D1_RECT_F toast{}, toast_text{}, toast_undo{};
};

} // namespace pulse::ui
