#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>
#include <string>

namespace pulse::ui {
inline float NavigationStep(float available, float scale) {
    return (available < 500 * scale ? 26.0f : 34.0f) * scale;
}
inline D2D1_RECT_F NavigationButtonAt(float x, float y, float scale, float step) {
    return {x, y, x + step - 4 * scale, y + 32 * scale};
}
// Toolbar pins: the pinned commands (自定义工具栏), then the pinned apps.
constexpr int kToolbarCommandPins = 5;
constexpr int kToolbarAppPins = 8;
constexpr int kToolbarPinSlots = kToolbarCommandPins + kToolbarAppPins;
struct ToolbarPinOptions {
    int app_from = -1;          // index of the first app pin (a short gap goes before it)
    bool drop_mode = false;     // a file drag is over the window: pins also in the narrow tier
    float label_width = 0.0f;   // drop mode: "拖到软件上打开" / "固定到工具栏" ahead of the pins
};
struct ToolbarLayout {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, create{};
    D2D1_RECT_F sort{}, filter{}, overflow{};
    D2D1_RECT_F group{};   // "Group" button / active chip (empty when not offered)
    std::array<D2D1_RECT_F, 8> commands{};
    std::array<D2D1_RECT_F, kToolbarPinSlots> pins{};   // pinned commands, then pinned apps
    D2D1_RECT_F pin_label{};   // drop mode only (ToolbarPinOptions::label_width)
};

inline float SelectionChipSpan(float scale, float label_width, int n);

// search_min_dip widens the search field while a search is active or being
// edited, borrowing from the address bar but leaving it at least 120 DIP.
inline ToolbarLayout MakeToolbarLayout(float width, float scale, float top, float margin, float create_width,
                                      float left, float filter_expand = 0.0f, float search_min_dip = 0.0f,
                                      float group_width = 0.0f, float sort_width = 0.0f,
                                      float filter_width = 0.0f, int pin_count = 0,
                                      float selection_span = 0.0f, const ToolbarPinOptions& pin_options = {}) {
    ToolbarLayout out;
    const float available = width - left;
    const float nav_step = NavigationStep(available, scale);
    float x = left + margin;
    for (int i=0;i<3;++i) {
        out.navigation[i] = NavigationButtonAt(x, top + 6 * scale, scale, nav_step);
        x += nav_step;
    }
    float search_width = std::clamp(available*0.27f, 140*scale, 360*scale);
    if (search_min_dip > 0.0f && available >= 480*scale) {
        // While a query is being edited (>= 500 DIP requested) the address bar may
        // shrink to 60 DIP; a results page keeps 120 DIP of breadcrumb.
        const float address_min = (search_min_dip >= 500.0f ? 60.0f : 120.0f) * scale;
        const float cap = width - margin - (x + margin + address_min) - nav_step - 2*margin;
        search_width = std::max(search_width, std::min(search_min_dip*scale, cap));
    }
    out.search = {width-margin-search_width,top+4*scale,width-margin,top+40*scale};
    out.address = {x+margin,top+4*scale,std::max(x+margin+60*scale,out.search.left-margin),top+40*scale};
    const bool overflow = available < 540*scale;
    if (available < 480*scale) {
        out.search.left=out.search.right-32*scale;
    }
    out.navigation[3] = NavigationButtonAt(out.search.left - margin - nav_step, top + 6 * scale, scale, nav_step);
    out.address.right = out.navigation[3].left-margin;
    out.create = {left+margin,top+50*scale,left+margin+create_width,top+82*scale};
    x = out.create.right + 12*scale;
    for (int i=0;i<5;++i) {
        out.commands[i] = {x,top+50*scale,x+30*scale,top+82*scale};
        x += 34*scale;
    }
    if (overflow) { for (auto& command : out.commands) command = {}; x = out.create.right; }
    // Left cluster ends here: 新建 + file commands ("act on files"), plus the
    // pinned commands placed below.
    float left_end = x + 8*scale;
    // Right cluster ("how to look at files"), right-aligned like the prototype:
    // [排序][筛选][分组] | [layout][preview][columns] [more]; narrow: ... [more].
    // More is always there (prototype note 5): select all, terminal,
    // properties, 自定义工具栏, plus the folded commands when narrow.
    out.overflow = {width-margin-32*scale,top+50*scale,width-margin,top+82*scale};
    if (!overflow) {
        const float pane_left = out.overflow.left-4*scale-98*scale;
        for (int i=5;i<8;++i) {
            const float cx = pane_left+(i-5)*34*scale;
            out.commands[i] = {cx,top+50*scale,cx+30*scale,top+82*scale};
        }
    }
    const float view_end = overflow ? out.overflow.left-8*scale : out.commands[5].left-13*scale;
    // Pinned commands and apps (自定义工具栏) follow the file commands after a
    // divider, as many as leave the view group its icon-only minimum. The rest,
    // and all of them in the narrow tier, wait in the More menu. While files are
    // dragged over the window the toolbar holds drop targets only (the caller
    // passes the apps alone): they show in the narrow tier too, after a label
    // when it fits together with every one of them.
    // An open filter (filter_expand > 0) asks for 200 DIP to type in: the
    // pinned commands and apps fold into More, the view labels to icons and
    // the selection chip to its smallest form (more, clear) first. That
    // happens as the field starts to open, not midway through the animation.
    const bool filter_open = filter_expand > 0.0f;
    const float filter_open_w = 200*scale;
    const float chip_span = filter_open ? std::min(selection_span, SelectionChipSpan(scale, 0.0f, 0)) : selection_span;
    const bool drop_mode = pin_options.drop_mode;
    if ((!overflow || drop_mode) && (pin_count > 0 || (drop_mode && pin_options.label_width > 0.0f))) {
        const float view_min = 70*scale + (group_width > 0.0f ? 38*scale : 0.0f) +
                               (filter_open ? filter_open_w - 32*scale + chip_span : 0.0f);
        const int count = std::min(pin_count, kToolbarPinSlots);
        const bool gap = pin_options.app_from > 0 && pin_options.app_from < count;
        float px = x + (overflow ? 13.0f : 9.0f)*scale;   // 13 DIP after the last command, like the selection chip
        if (drop_mode && pin_options.label_width > 0.0f) {
            const float all = pin_options.label_width + 4*scale + static_cast<float>(count)*34*scale;
            if (px+all+8*scale+view_min <= view_end) {
                out.pin_label = {px,top+50*scale,px+pin_options.label_width,top+82*scale};
                px = out.pin_label.right + 4*scale;
                left_end = px + 4*scale;
            }
        }
        for (int i=0;i<count;++i) {
            const float lead = gap && i == pin_options.app_from ? 9*scale : 0.0f;
            if (px+lead+30*scale+8*scale+view_min > view_end) break;
            px += lead;
            out.pins[i] = {px,top+50*scale,px+30*scale,top+82*scale};
            px += 34*scale;
            left_end = px + 8*scale;
        }
    }
    // sort_width: measured "icon + label + chevron" width (0 = legacy 88 DIP).
    float sort_w = available >= 700*scale ? (sort_width > 0.0f ? sort_width : 88*scale) : 32*scale;
    // filter_width: measured "icon + label" width (0 = legacy 88 DIP); kept above the
    // 60 DIP icon-only threshold. Expanding grows the field to the left.
    float filter_base = available >= 700*scale
        ? (filter_width > 0.0f ? std::max(filter_width, 64*scale) : 88*scale) : 32*scale;
    float group_w = group_width;
    // While something is selected the selection chip (prototype note 4) comes
    // before the view labels: they fold to icons first so the chip keeps its
    // quick actions. The chip itself shrinks into whatever is left.
    const float room = view_end - left_end - (overflow ? 0.0f : chip_span);
    auto block_of = [&](float filter) { return sort_w+6*scale+filter+(group_w > 0.0f ? group_w+6*scale : 0.0f); };
    // Priority collapse when the labels do not fit: icon-only sort / filter
    // first, then a narrower group chip (never below its icon).
    if (block_of(filter_base) > room) { sort_w = 32*scale; filter_base = 32*scale; }
    const float filter_goal = filter_open ? std::max(filter_base, filter_open_w) : filter_base;
    if (block_of(filter_goal) > room) sort_w = 32*scale;
    if (group_w > 0.0f && block_of(filter_goal) > room)
        group_w = std::max(32*scale, group_w-(block_of(filter_goal)-room));
    const float filter_max = std::clamp(room-block_of(0.0f),filter_base,220*scale);
    const float filter_w = filter_base + (filter_max-filter_base)*std::clamp(filter_expand,0.0f,1.0f);
    // Still too narrow to right-align: start right after the left cluster instead.
    x = std::max(left_end, view_end-block_of(filter_w));
    out.sort = {x,top+50*scale,x+sort_w,top+82*scale};
    x = out.sort.right + 6*scale;
    out.filter = {x,top+50*scale,x+filter_w,top+82*scale};
    if (group_width > 0.0f) {
        const float gx = out.filter.right+6*scale;
        out.group = {gx,top+50*scale,gx+group_w,top+82*scale};
    }
    return out;
}

// Selection context (prototype note 4): up to kSelChipMaxActions commands of
// the Pulse item menu in prototype priority (标签, 复制路径, ...; not filtered
// by the right-click menu's visibility settings), then "more" (the full item
// menu) and "clear".
// Priority collapse: the label goes first, then trailing quick actions;
// "more" and "clear" stay. An empty chip means it does not fit at all.
struct SelectionChipAction {
    int command = 0;
    std::wstring glyph;
    std::wstring label;   // tooltip, with the shortcut
    bool enabled = true;
};
constexpr int kSelChipMaxActions = 6;
constexpr int kSelChipMore = kSelChipMaxActions;
constexpr int kSelChipClear = kSelChipMaxActions + 1;
constexpr int kSelChipSlots = kSelChipMaxActions + 2;
struct ToolbarSelectionChip {
    D2D1_RECT_F chip{}, label{};
    std::array<D2D1_RECT_F, kSelChipSlots> buttons{};   // quick actions, more, clear
};
// Chip width with `n` quick actions (label_width 0 = no label).
inline float SelectionChipWidth(float scale, float label_width, int n) {
    const float buttons = static_cast<float>((n + 2)*30 - 2)*scale + (n > 0 ? 7*scale : 0.0f);
    return (label_width > 0.0f ? 10*scale + label_width + 4*scale : 4*scale) + buttons + 4*scale;
}
// Room the chip asks of the toolbar row: the 13 DIP lead (divider) and the
// 10 DIP gap before the view group included.
inline float SelectionChipSpan(float scale, float label_width, int n) {
    return 13*scale + SelectionChipWidth(scale, label_width, std::clamp(n, 0, kSelChipMaxActions)) + 10*scale;
}
inline ToolbarSelectionChip MakeToolbarSelectionChip(const ToolbarLayout& l, float scale, float label_width,
                                                     int action_count) {
    ToolbarSelectionChip out;
    D2D1_RECT_F anchor = l.commands[4].right > l.commands[4].left ? l.commands[4] : l.create;
    for (const D2D1_RECT_F& pin : l.pins) if (pin.right > pin.left) anchor = pin;
    const float x0 = anchor.right + 13*scale;   // a divider sits at x0 - 7
    const float limit = l.sort.left - 10*scale;
    int count = std::clamp(action_count, 0, kSelChipMaxActions);
    const auto width = [&](float label, int n) { return SelectionChipWidth(scale, label, n); };
    if (label_width > 0.0f && x0 + width(label_width, count) > limit) label_width = 0.0f;
    while (count > 0 && x0 + width(label_width, count) > limit) --count;
    if (x0 + width(label_width, count) > limit) return out;
    out.chip = {x0, l.create.top, x0 + width(label_width, count), l.create.bottom};
    float x = x0 + 4*scale;
    if (label_width > 0.0f) {
        out.label = {x0 + 10*scale, l.create.top, x0 + 10*scale + label_width, l.create.bottom};
        x = out.label.right + 4*scale;
    }
    const auto place = [&](int slot) {
        out.buttons[static_cast<size_t>(slot)] = {x, l.create.top + 2*scale, x + 28*scale, l.create.bottom - 2*scale};
        x += 30*scale;
    };
    for (int i = 0; i < count; ++i) place(i);
    if (count > 0) x += 7*scale;   // room for a divider before more / clear
    place(kSelChipMore);
    place(kSelChipClear);
    return out;
}

// Trailing "x" of the active group chip (removes grouping).
inline D2D1_RECT_F ToolbarGroupClearRect(const D2D1_RECT_F& chip, float scale) {
    return {chip.right-28*scale,chip.top+5*scale,chip.right-6*scale,chip.bottom-5*scale};
}
}
