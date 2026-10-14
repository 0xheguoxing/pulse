// settings_sysctx_layout.h — Settings > 系统右键菜单 (page 6): texts, geometry
// and hit testing. Included by ui_renderer_internal.h inside its anonymous
// namespace (like settings_layout_sections.h), so no includes here. Drawing:
// ui_settings_sysctx.cpp; clicks: HitTestResult::SettingsSysCtx, handled in
// app/settings_ui.cpp; data: app::SystemCtxModel.
//
// Layout follows the prototype (pulse_system_ctx_manager.html): intro with
// the page commands, the 本地 / 系统 layer cards, then scene list | entries |
// preview & details. Narrow windows drop the third column and open the
// details under the selected row instead.

namespace sysctx_text {
inline const wchar_t* Title() { return l10n::Pick(L"系统右键菜单", L"System context menu"); }
// One sidebar entry for both context-menu pages; the layer cards under the intro
// switch between Pulse's own menu (page 2) and the system handlers (page 6).
inline const wchar_t* PageTitle() { return l10n::Pick(L"右键菜单", L"Context menu"); }
inline const wchar_t* Intro() { return l10n::Pick(
    L"改动作用于资源管理器和所有应用的右键菜单。每次应用前自动导出 .reg 备份，随时可以撤销或全部还原。",
    L"Changes apply to the context menu in File Explorer and every app. A .reg backup is exported before "
    L"each change; undo or restore everything at any time."); }
inline const wchar_t* Undo() { return l10n::Pick(L"撤销", L"Undo"); }
inline const wchar_t* Log() { return l10n::Pick(L"操作日志", L"Activity log"); }
inline const wchar_t* RestoreAll() { return l10n::Pick(L"全部还原…", L"Restore all…"); }
inline const wchar_t* LocalTitle() { return l10n::Pick(L"Pulse 右键菜单", L"Pulse context menu"); }
inline const wchar_t* LocalCaption() { return l10n::Pick(L"只影响 Pulse · 不需要权限 · 随时恢复",
                                                         L"Only inside Pulse · no permission needed · always reversible"); }
inline const wchar_t* LocalTag() { return l10n::Pick(L"切换 →", L"Switch →"); }
inline const wchar_t* SystemTitle() { return l10n::Pick(L"系统右键菜单", L"System context menu"); }
inline const wchar_t* SystemCaption() { return l10n::Pick(
    L"影响资源管理器和所有应用 · 当前用户直接改，所有用户一批改动只需一次 UAC",
    L"File Explorer and every app · your entries change at once; all-user changes need one UAC per batch"); }
inline const wchar_t* Current() { return l10n::Pick(L"当前", L"Current"); }
inline const wchar_t* Group(int g) {
    return g == 0 ? l10n::Pick(L"基本对象", L"Basic objects")
         : g == 1 ? l10n::Pick(L"特定类型", L"Specific types")
                  : l10n::Pick(L"专门菜单", L"Special menus");
}
inline const wchar_t* Filter(int f) {
    switch (f) {
    case kSysCtxFilterOff: return l10n::Pick(L"已禁用", L"Off");
    case kSysCtxFilterAdmin: return l10n::Pick(L"需要管理员", L"Needs admin");
    case kSysCtxFilterThirdParty: return l10n::Pick(L"第三方", L"Third-party");
    case kSysCtxFilterMissing: return l10n::Pick(L"残留项", L"Leftovers");
    case kSysCtxFilterDefaultHidden: return l10n::Pick(L"系统默认隐藏", L"Hidden by Windows");
    default: return l10n::Pick(L"全部", L"All");
    }
}
inline std::wstring Chip(const SysCtxView& v, int f) {
    return std::wstring(Filter(f)) + L"  " + std::to_wstring(v.filter_counts[static_cast<size_t>(f)]);
}
inline const wchar_t* SubLabel(bool perceived) {
    return perceived ? l10n::Pick(L"感知类型", L"Type") : l10n::Pick(L"扩展名", L"Extension");
}
inline const wchar_t* Loading() { return l10n::Pick(L"正在读取注册表…", L"Reading the registry…"); }
inline std::wstring PendingTitle(int n) {
    return l10n::Pick(L"", L"") + std::to_wstring(n) + l10n::Pick(L" 项更改待应用", L" change(s) waiting");
}
inline const wchar_t* PendingCaption() { return l10n::Pick(
    L"涉及所有用户的条目 · 先导出 .reg 备份，只确认一次管理员权限",
    L"All-user entries · a .reg backup first, then one administrator confirmation"); }
inline const wchar_t* Applying() { return l10n::Pick(L"正在应用… 如有管理员权限提示，请确认。",
                                                     L"Applying… confirm the administrator prompt if one appears."); }
inline const wchar_t* Apply() { return l10n::Pick(L"应用更改", L"Apply changes"); }
inline const wchar_t* Discard() { return l10n::Pick(L"放弃", L"Discard"); }
inline const wchar_t* Empty(bool filtered) {
    return filtered ? l10n::Pick(L"没有符合这个筛选的条目。", L"Nothing matches this filter.")
                    : l10n::Pick(L"这里没有条目。", L"No entries here.");
}
inline const wchar_t* Admin() { return l10n::Pick(L"管理员", L"Admin"); }
inline const wchar_t* NoAdmin() { return l10n::Pick(L"无需管理员", L"No admin"); }
inline const wchar_t* Missing() { return l10n::Pick(L"残留 · 程序已卸载", L"Leftover · uninstalled"); }
inline const wchar_t* DefaultHidden() { return l10n::Pick(L"系统默认隐藏", L"Hidden by Windows"); }
inline const wchar_t* Queued() { return l10n::Pick(L"待应用", L"Pending"); }
inline const wchar_t* Shift() { return L"\x21E7 Shift"; }
inline const wchar_t* Protected() { return l10n::Pick(L"受保护", L"Protected"); }
inline const wchar_t* SystemOwned() { return l10n::Pick(L"系统所有", L"Owned by Windows"); }
inline const wchar_t* ReadOnly() { return l10n::Pick(L"暂不可改", L"Not yet"); }
inline const wchar_t* TabPreview() { return l10n::Pick(L"实时预览", L"Live preview"); }
inline const wchar_t* TabDetail() { return l10n::Pick(L"详情", L"Details"); }
inline const wchar_t* Key(int i) {
    return i == 0 ? l10n::Pick(L"注册表", L"Registry")
         : i == 1 ? l10n::Pick(L"文件", L"File")
         : i == 2 ? l10n::Pick(L"发布者", L"Publisher")
                  : l10n::Pick(L"权限", L"Permission");
}
inline std::wstring Permission(const SysCtxRowView& r) {
    return r.machine ? l10n::Pick(L"需要管理员 · 一批改动只弹一次 UAC", L"Needs admin · one UAC per batch")
                     : l10n::Pick(L"无需管理员 · 立即生效", L"No admin · applies at once");
}
inline const wchar_t* EnabledTitle() { return l10n::Pick(L"在系统里启用", L"Enabled in Windows"); }
inline const wchar_t* EnabledCaption() { return l10n::Pick(L"资源管理器和所有应用", L"File Explorer and every app"); }
inline const wchar_t* PulseShownTitle() { return l10n::Pick(L"在 Pulse 右键菜单中显示", L"Shown in Pulse's menu"); }
inline const wchar_t* PulseShownCaption() { return l10n::Pick(L"只影响 Pulse，不改动 Windows", L"Pulse only; Windows is unchanged"); }
inline const wchar_t* PulseHidden() { return l10n::Pick(L"Pulse 中隐藏", L"Hidden in Pulse"); }
inline const wchar_t* CopyKey() { return l10n::Pick(L"复制注册表路径", L"Copy registry path"); }
inline const wchar_t* OpenFolder() { return l10n::Pick(L"打开所在文件夹", L"Open folder"); }
inline const wchar_t* NoSelection() { return l10n::Pick(L"在左侧列表里点一个条目查看详情。",
                                                        L"Pick an entry in the list to see its details."); }
inline std::wstring PreviewHead(const SysCtxView& v) {
    // 按扩展名 / 按感知类型: name the picked extension or type, not the scene.
    const bool picked = !v.subs.empty() && v.sub >= 0 && v.sub < static_cast<int>(v.subs.size());
    const std::wstring target = picked ? v.subs[static_cast<size_t>(v.sub)] + l10n::Pick(L" 文件", L" files") : v.scene_title;
    return l10n::Pick(L"右键单击「", L"Right-click on ") + target + l10n::Pick(L"」时", L"");
}
inline const wchar_t* PreviewNote() { return l10n::Pick(L"示意图 · 拖动条目可置顶、置底或隐藏",
                                                        L"Sketch of this scene's entries. Drag to pin or hide."); }
inline const wchar_t* Zone(int zone) {
    return zone == kSysCtxZoneTop ? l10n::Pick(L"置顶", L"Top")
         : zone == kSysCtxZoneBottom ? l10n::Pick(L"置底", L"Bottom") : l10n::Pick(L"默认位置", L"Default");
}
inline const wchar_t* Moved(int zone) {
    return zone == kSysCtxZoneTop ? l10n::Pick(L"将置顶", L"To top")
         : zone == kSysCtxZoneBottom ? l10n::Pick(L"将置底", L"To bottom") : l10n::Pick(L"将恢复位置", L"To default");
}
inline std::wstring ShelfHead(int n) {
    return l10n::Pick(L"已隐藏 · ", L"Hidden · ") + std::to_wstring(n);
}
inline const wchar_t* ShelfEmpty() { return l10n::Pick(L"把条目拖到这里即可隐藏", L"Drop an entry here to hide it"); }
inline const wchar_t* DragHintVerb() {
    return l10n::Pick(L"拖到「置顶 / 置底」分区，或拖到下方隐藏", L"Drop on Top / Bottom, or below to hide");
}
inline const wchar_t* DragHintHandler() {
    return l10n::Pick(L"第三方扩展的位置由 Windows 决定，只能拖到下方隐藏", L"Windows places extensions itself: drop below to hide");
}
inline const wchar_t* WillMove() { return l10n::Pick(L"将移动", L"Moves"); }
inline const wchar_t* WillHide() { return l10n::Pick(L"将隐藏", L"Hides"); }
inline const wchar_t* WillShow() { return l10n::Pick(L"将显示", L"Shows"); }
inline std::wstring More(int n) {
    return l10n::Pick(L"… 还有 ", L"… ") + std::to_wstring(n) + l10n::Pick(L" 项", L" more");
}
// 概览 (the prototype's home scene).
inline const wchar_t* Overview() { return l10n::Pick(L"概览", L"Overview"); }
inline std::wstring OverviewKey(const SysCtxView& v) {
    return l10n::Pick(L"共 ", L"") + std::to_wstring(v.total) + l10n::Pick(L" 个条目 · ", L" entries · ") +
           std::to_wstring(v.scenes.size()) + l10n::Pick(L" 个场景", L" scenes");
}
inline const wchar_t* Stat(int i) {
    switch (i) {
    case 0: return l10n::Pick(L"已禁用", L"Switched off");
    case 1: return l10n::Pick(L"由 ContextMenuManager 禁用", L"Off by ContextMenuManager");
    case 2: return l10n::Pick(L"拖慢右键 ≥150 ms 的扩展", L"Extensions slowing the menu ≥150 ms");
    default: return l10n::Pick(L"Pulse 做过的改动", L"Changes made by Pulse");
    }
}
inline const wchar_t* SlowTitle() { return l10n::Pick(L"慢扩展排行", L"Slowest extensions"); }
inline const wchar_t* SlowCaption() {
    return l10n::Pick(L"来自 Pulse 右键菜单的实测数据（每个场景最近一次）。禁用后，资源管理器和 Pulse 的右键都会变快。",
                      L"Measured while Pulse built its own menu (the last query per scene). Switching one off "
                      L"speeds up File Explorer and Pulse alike.");
}
inline const wchar_t* SlowEmpty() {
    return l10n::Pick(L"还没有数据。在 Pulse 里右键几次文件或文件夹，这里就会显示各扩展的耗时。",
                      L"No data yet. Right-click a few files or folders in Pulse and the times show up here.");
}
inline const wchar_t* PulseSkip() { return l10n::Pick(L"Pulse 跳过", L"Skip in Pulse"); }
inline const wchar_t* PulseSkipped() { return l10n::Pick(L"Pulse 已跳过", L"Skipped in Pulse"); }
inline const wchar_t* TurnOff() { return l10n::Pick(L"禁用", L"Turn off"); }
inline const wchar_t* Off() { return l10n::Pick(L"已禁用", L"Off"); }
inline const wchar_t* MarksTitle() { return l10n::Pick(L"来自其他工具的禁用标记", L"Marks left by other tools"); }
inline const wchar_t* MarksCaption() {
    return l10n::Pick(L"识别 ContextMenuManager 写下的标记，显示在对应场景里，也可以在这里一键还原。",
                      L"Marks written by ContextMenuManager show in their scenes and can be restored here.");
}
inline const wchar_t* MarksEmpty() { return l10n::Pick(L"没有了 ✓", L"None ✓"); }
inline const wchar_t* Restore() { return l10n::Pick(L"还原", L"Restore"); }
inline const wchar_t* RecentTitle() { return l10n::Pick(L"最近的改动", L"Recent changes"); }
inline const wchar_t* RecentCaption() {
    return l10n::Pick(L"每次应用都有对应的 .reg 备份，可以逐步撤销。",
                      L"Every batch has its .reg backup and can be undone step by step.");
}
inline const wchar_t* RecentEmpty() {
    return l10n::Pick(L"还没有改动。在左侧选择一个场景开始。", L"No changes yet. Pick a scene on the left to start.");
}
inline const wchar_t* UndonePrefix() { return l10n::Pick(L"已撤销 · ", L"Undone · "); }
inline const wchar_t* OverviewSideHint() {
    return l10n::Pick(L"在左侧选择一个场景，这里会显示它的右键菜单，并实时反映你的改动。",
                      L"Pick a scene on the left to see its menu here, live with your changes.");
}
inline const wchar_t* OverviewSideNote() {
    return l10n::Pick(L"概览页的数据来自 Pulse 实际右键时的测量，不需要额外运行任何东西。",
                      L"The overview uses what Pulse measured on real right-clicks; nothing else needs to run.");
}
inline std::wstring Ms(uint32_t ms) { return std::to_wstring(ms) + L" ms"; }
} // namespace sysctx_text

inline constexpr float kSysCtxRowDip = 52.0f;

inline bool SysCtxContains(const D2D1_RECT_F& r, float x, float y) {
    return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

inline bool SysCtxSwitchable(const SysCtxRowView& row) { return !row.locked && !row.read_only; }

inline float SysCtxTextWidth(const fluent::Painter* painter, std::wstring_view text, float s) {
    // Badge metrics: caption-sized text plus a little padding.
    return painter ? painter->MeasureBadgeWidth(text) : static_cast<float>(text.size()) * 8.0f * s + 14.0f * s;
}

inline float SysCtxWrappedHeight(const fluent::Painter* painter, std::wstring_view text, float width, float s) {
    if (text.empty()) return 18 * s;
    return (std::max)(18 * s, painter ? painter->MeasureWrappedCaptionHeight(text, width) : 36 * s);
}

// Details of `row` laid out in [left, right] from `top`; returns the bottom.
inline float LayoutSysCtxDetail(SysCtxDetailLayout& d, const SysCtxRowView& row, float left, float right, float top,
                                const fluent::Painter* painter, float s, bool inline_head = false) {
    d = {};
    float y = top;
    // Under its row the name and icon are already visible: keep only the kind line.
    d.head = D2D1::RectF(left, y, right, y + (inline_head ? 24 : 48) * s);
    y = d.head.bottom + 6 * s;
    const float label_w = 64 * s, value_left = left + label_w + 10 * s, value_w = (std::max)(40 * s, right - value_left);
    const std::wstring values[4] = {row.registry, row.file, row.publisher, sysctx_text::Permission(row)};
    for (size_t i = 0; i < 4; ++i) {
        const float h = SysCtxWrappedHeight(painter, values[i].empty() ? L"—" : values[i], value_w, s);
        d.kv[i] = D2D1::RectF(left, y, right, y + h);
        y += h + 6 * s;
    }
    y += 6 * s;
    d.switch_row = D2D1::RectF(left, y, right, y + 44 * s);
    if (SysCtxSwitchable(row)) {
        const float mid = (d.switch_row.top + d.switch_row.bottom) * 0.5f;
        d.toggle = D2D1::RectF(right - 44 * s, mid - 11 * s, right, mid + 11 * s);
    }
    y = d.switch_row.bottom + 4 * s;
    // What the Windows switch writes stays right under it.
    const float method_h = SysCtxWrappedHeight(painter, row.method, right - left - 20 * s, s) + 16 * s;
    d.method = D2D1::RectF(left, y, right, y + method_h);
    y = d.method.bottom + 4 * s;
    if (row.pulse != 0) {
        // 只在 Pulse 隐藏: ContextMenuPrefs, whatever Windows does with the entry.
        d.pulse_row = D2D1::RectF(left, y, right, y + 44 * s);
        const float mid = (d.pulse_row.top + d.pulse_row.bottom) * 0.5f;
        d.pulse_toggle = D2D1::RectF(right - 44 * s, mid - 11 * s, right, mid + 11 * s);
        y = d.pulse_row.bottom;
    }
    y += 6 * s;
    const float bh = 28 * s;
    const float cw = painter ? painter->MeasureButtonWidth(sysctx_text::CopyKey(), L"\xE8C8") : 120 * s;
    const float fw = painter ? painter->MeasureButtonWidth(sysctx_text::OpenFolder(), L"\xE838") : 120 * s;
    if (!row.registry.empty()) d.copy = D2D1::RectF(left, y, (std::min)(right, left + cw), y + bh);
    const float fx = d.copy.right > d.copy.left ? d.copy.right + 8 * s : left;
    if (!row.file.empty() && !row.missing && fx + fw <= right + 1)
        d.folder = D2D1::RectF(fx, y, fx + fw, y + bh);
    y += bh;
    d.bounds = D2D1::RectF(left, top, right, y);
    return y;
}

// Live preview geometry. With a live drag every landing zone the item may
// take is tried and the one whose slot is nearest the floating item wins; the
// other items are laid out as that drop would leave them (as on the Pulse
// menu page). `with_drag` false: the resting layout (hit testing, grab offset).
inline SysCtxPreviewLayout LayoutSysCtxPreview(const SysCtxView& v, const D2D1_RECT_F& body, float s,
                                               bool with_drag = true) {
    const int n = static_cast<int>(v.preview.size());
    const float item_h = 28 * s, label_h = 20 * s, pad = 4 * s;
    const float left = body.left, right = (std::min)(body.right, body.left + 290 * s);
    const int dragged = with_drag && v.drag_live && v.drag_item >= 0 && v.drag_item < n ? v.drag_item : -1;
    const bool zones = dragged >= 0 && v.preview[static_cast<size_t>(dragged)].movable;
    const D2D1_RECT_F legend = D2D1::RectF(left, body.bottom - 38 * s, body.right, body.bottom);

    struct Arrangement {
        std::vector<int> menu, shelf;
        std::vector<int> rank;
    };
    auto arrange = [&](int drag_zone) {
        Arrangement a;
        a.rank.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const auto& p = v.preview[static_cast<size_t>(i)];
            bool hidden = p.hidden;
            int rank = p.rank;
            if (i == dragged && drag_zone != kSysCtxZoneNone) {
                hidden = drag_zone == kSysCtxZoneHidden;
                if (!hidden && p.movable) rank = drag_zone == kSysCtxZoneTop ? 0 : drag_zone == kSysCtxZoneBottom ? 3 : 1;
            }
            a.rank[static_cast<size_t>(i)] = rank;
            (hidden ? a.shelf : a.menu).push_back(i);
        }
        std::stable_sort(a.menu.begin(), a.menu.end(), [&](int x, int y) {
            return a.rank[static_cast<size_t>(x)] < a.rank[static_cast<size_t>(y)];
        });
        auto fresh = [&](int i) {
            const auto& p = v.preview[static_cast<size_t>(i)];
            return i == dragged || p.moved != 0 || p.change != 0;
        };
        std::stable_partition(a.shelf.begin(), a.shelf.end(), fresh);
        return a;
    };
    // Lays out one arrangement into `out`; returns the dragged item's slot.
    auto place = [&](const Arrangement& a, SysCtxPreviewLayout& out) {
        out = {};
        out.items.assign(static_cast<size_t>(n), D2D1_RECT_F{});
        out.head = D2D1::RectF(left, body.top, body.right, body.top + 20 * s);
        out.legend = legend;
        D2D1_RECT_F slot{};
        const float top = body.top + 26 * s;
        const float bottom = legend.top - 8 * s;
        const int shelf_rows_min = a.shelf.empty() ? 1 : (std::min)(static_cast<int>(a.shelf.size()), 3);
        const float shelf_min = 22 * s + static_cast<float>(shelf_rows_min) * item_h + 2 * pad;
        const int labels = zones ? 3 : 0;
        const float menu_budget = (std::max)(item_h + 2 * pad, bottom - top - 12 * s - shelf_min);
        int menu_cap = static_cast<int>((menu_budget - 2 * pad - static_cast<float>(labels) * label_h) / item_h);
        menu_cap = (std::max)(1, menu_cap);
        const int menu_n = static_cast<int>(a.menu.size());
        const bool menu_more = menu_n > menu_cap;
        // Too many rows: fold default-position rows into one "… 还有 N 项" row,
        // keeping the held item, Top/Bottom rows and pending changes in sight.
        std::vector<char> shown(static_cast<size_t>(menu_n), menu_more ? 0 : 1);
        int menu_hidden = 0;
        if (menu_more) {
            int budget = (std::max)(0, menu_cap - 1);
            auto take = [&](auto&& want) {
                for (int k = 0; k < menu_n && budget > 0; ++k) {
                    const int i = a.menu[static_cast<size_t>(k)];
                    if (!shown[static_cast<size_t>(k)] && want(i)) {
                        shown[static_cast<size_t>(k)] = 1;
                        --budget;
                    }
                }
            };
            take([&](int i) { return i == dragged; });
            take([&](int i) {
                const int rank = a.rank[static_cast<size_t>(i)];
                const auto& p = v.preview[static_cast<size_t>(i)];
                return rank == 0 || rank == 3 || p.moved != 0 || p.change != 0;
            });
            take([](int) { return true; });
            for (const char f : shown) menu_hidden += f ? 0 : 1;
        }
        float y = top + pad;
        int zone_now = -1;
        auto zone_of_rank = [](int rank) { return rank == 0 ? 0 : rank == 3 ? 2 : 1; };
        auto emit_labels_until = [&](int zone) {
            while (zones && zone_now < zone) {
                ++zone_now;
                out.zone_label[static_cast<size_t>(zone_now)] = D2D1::RectF(left + pad, y, right - pad, y + label_h);
                y += label_h;
            }
        };
        for (int k = 0; k < menu_n; ++k) {
            const int i = a.menu[static_cast<size_t>(k)];
            if (!shown[static_cast<size_t>(k)]) {
                if (out.more_menu_count == 0) {  // one fold row where the first folded item sits
                    emit_labels_until(zone_of_rank(a.rank[static_cast<size_t>(i)]));
                    out.more_menu = D2D1::RectF(left + pad, y, right - pad, y + item_h);
                    out.more_menu_count = menu_hidden;
                    y += item_h;
                }
                continue;
            }
            emit_labels_until(zone_of_rank(a.rank[static_cast<size_t>(i)]));
            const auto r = D2D1::RectF(left + pad, y, right - pad, y + item_h);
            if (i == dragged) slot = r;
            else out.items[static_cast<size_t>(i)] = r;
            y += item_h;
        }
        emit_labels_until(2);
        if (menu_n == 0 && !zones) y += item_h;  // an empty menu still shows its surface
        out.menu = D2D1::RectF(left, top, right, y + pad);

        y = out.menu.bottom + 12 * s;
        out.shelf_head = D2D1::RectF(left, y, right, y + 22 * s);
        y = out.shelf_head.bottom;
        const float shelf_top = y;
        const int shelf_n = static_cast<int>(a.shelf.size());
        int shelf_cap = (std::max)(1, static_cast<int>((bottom - y - 2 * pad) / item_h));
        const bool shelf_more = shelf_n > shelf_cap;
        const int shelf_shown = shelf_more ? (std::max)(0, shelf_cap - 1) : shelf_n;
        y += pad;
        for (int k = 0; k < shelf_shown; ++k) {
            const int i = a.shelf[static_cast<size_t>(k)];
            const auto r = D2D1::RectF(left + pad, y, right - pad, y + item_h);
            if (i == dragged) slot = r;
            else out.items[static_cast<size_t>(i)] = r;
            y += item_h;
        }
        if (shelf_more) {
            out.more_shelf = D2D1::RectF(left + pad, y, right - pad, y + item_h);
            out.more_shelf_count = shelf_n - shelf_shown;
            y += item_h;
        }
        if (shelf_n == 0) y += item_h;  // placeholder: 把条目拖到这里即可隐藏
        out.shelf = D2D1::RectF(left, shelf_top, right, y + pad);
        return slot;
    };

    SysCtxPreviewLayout best_layout;
    place(arrange(kSysCtxZoneNone), best_layout);
    if (dragged < 0) return best_layout;
    const auto& held = v.preview[static_cast<size_t>(dragged)];
    const float ghost_top = std::clamp(v.drag_y - v.drag_grab, body.top, (std::max)(body.top, body.bottom - item_h));
    std::vector<int> candidates;
    if (held.movable) candidates = {kSysCtxZoneTop, kSysCtxZoneDefault, kSysCtxZoneBottom, kSysCtxZoneHidden};
    else candidates = {kSysCtxZoneDefault, kSysCtxZoneHidden};
    float best = 1e30f;
    for (const int zone : candidates) {
        SysCtxPreviewLayout trial;
        const D2D1_RECT_F slot = place(arrange(zone), trial);
        if (slot.bottom <= slot.top) continue;  // would land past the visible rows
        const float d = std::fabs(slot.top - ghost_top);
        if (d >= best) continue;
        best = d;
        trial.dragging = true;
        trial.drop_zone = zone;
        trial.gap = slot;
        trial.ghost = D2D1::RectF(slot.left, ghost_top, slot.right, ghost_top + item_h);
        best_layout = std::move(trial);
    }
    return best_layout;
}

// 概览 cards in [left, right] from `top` (the prototype's .ov grid): the
// stats, 慢扩展排行, then 禁用标记 | 最近的改动 side by side when there is room.
// Returns the bottom.
inline float LayoutSysCtxOverview(SysCtxOverviewLayout& ov, const SysCtxView& v, float left, float right, float top,
                                  const fluent::Painter* painter, float s) {
    const SysCtxOverview& data = v.overview;
    const float gap = 12 * s, pad = 14 * s, title_h = 22 * s;
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text, {}) : 72 * s; };
    float y = top;

    // Stats: four tiles in one wide card.
    {
        const float tile_h = 62 * s, tile_gap = 10 * s;
        const float tile_w = (right - left - 2 * pad - 3 * tile_gap) / 4;
        for (size_t i = 0; i < ov.stats.size(); ++i) {
            const float x = left + pad + static_cast<float>(i) * (tile_w + tile_gap);
            ov.stats[i] = D2D1::RectF(x, y + pad, x + tile_w, y + pad + tile_h);
        }
        ov.stats_card = D2D1::RectF(left, y, right, y + pad + tile_h + pad);
        y = ov.stats_card.bottom + gap;
    }

    // 慢扩展排行: icon, name, bar, time, buttons.
    {
        float cy = y + pad + title_h;
        cy += SysCtxWrappedHeight(painter, sysctx_text::SlowCaption(), right - left - 2 * pad, s) + 10 * s;
        if (data.slow_rows.empty()) {
            ov.slow_empty = D2D1::RectF(left + pad, cy, right - pad, cy + 40 * s);
            cy = ov.slow_empty.bottom;
        }
        const float rh = 42 * s, bh = 26 * s;
        const float skip_w = button_w(sysctx_text::PulseSkip()), off_w = button_w(sysctx_text::TurnOff());
        for (const auto& row : data.slow_rows) {
            const auto r = D2D1::RectF(left + pad, cy, right - pad, cy + rh);
            const float mid = (r.top + r.bottom) * 0.5f;
            float bx = r.right;
            D2D1_RECT_F toggle{}, skip{};
            if (row.on && row.switchable) {
                toggle = D2D1::RectF(bx - off_w, mid - bh * 0.5f, bx, mid + bh * 0.5f);
                bx -= off_w + 6 * s;
            }
            if (row.on && !row.pulse_skipped) skip = D2D1::RectF(bx - skip_w, mid - bh * 0.5f, bx, mid + bh * 0.5f);
            ov.slow_rows.push_back(r);
            ov.slow_toggle.push_back(toggle);
            ov.slow_skip.push_back(skip);
            cy += rh;
        }
        ov.slow_card = D2D1::RectF(left, y, right, cy + pad);
        y = ov.slow_card.bottom + gap;
    }

    // 禁用标记 | 最近的改动.
    const bool two = (right - left) >= 640 * s;
    const float col_w = two ? (right - left - gap) * 0.5f : right - left;
    auto mini_card = [&](D2D1_RECT_F& card, std::vector<D2D1_RECT_F>& rows, D2D1_RECT_F& empty, float cl, float cr,
                         float ct, const wchar_t* caption, size_t count, float rh) {
        float cy = ct + pad + title_h;
        cy += SysCtxWrappedHeight(painter, caption, cr - cl - 2 * pad, s) + 10 * s;
        if (count == 0) {
            empty = D2D1::RectF(cl + pad, cy, cr - pad, cy + 20 * s);
            cy = empty.bottom;
        }
        for (size_t i = 0; i < count; ++i) {
            rows.push_back(D2D1::RectF(cl + pad, cy, cr - pad, cy + rh));
            cy += rh;
        }
        card = D2D1::RectF(cl, ct, cr, cy + pad);
    };
    mini_card(ov.marks_card, ov.mark_rows, ov.marks_empty, left, left + col_w, y, sysctx_text::MarksCaption(),
              data.mark_rows.size(), 46 * s);
    const float restore_w = button_w(sysctx_text::Restore());
    for (const auto& r : ov.mark_rows) {
        const float mid = (r.top + r.bottom) * 0.5f;
        ov.mark_restore.push_back(D2D1::RectF(r.right - restore_w, mid - 13 * s, r.right, mid + 13 * s));
    }
    const float rl = two ? left + col_w + gap : left, rt = two ? y : ov.marks_card.bottom + gap;
    mini_card(ov.recent_card, ov.recent_rows, ov.recent_empty, rl, rl + col_w, rt, sysctx_text::RecentCaption(),
              data.recent.size(), 40 * s);
    if (two) {
        // Equal heights, like a grid row.
        const float bottom = (std::max)(ov.marks_card.bottom, ov.recent_card.bottom);
        ov.marks_card.bottom = ov.recent_card.bottom = bottom;
    }
    return (std::max)(ov.marks_card.bottom, ov.recent_card.bottom);
}

inline float LayoutSettingsSysCtx(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                                  const fluent::Painter* painter) {
    SysCtxLayout& o = l.sysctx;
    o = {};
    o.scale = s;
    const SysCtxView* v = vm.settings_sysctx.get();
    const float left = l.content.left + 20 * s, right = l.content.right - 20 * s;
    auto button_w = [&](std::wstring_view text, std::wstring_view glyph = {}) {
        return painter ? painter->MeasureButtonWidth(text, glyph) : 96 * s;
    };
    const float bh = 32 * s;

    // Intro on the left, page commands on the right.
    float x = right;
    auto place = [&](D2D1_RECT_F& r, std::wstring_view text, std::wstring_view glyph) {
        const float w = button_w(text, glyph);
        r = D2D1::RectF(x - w, y, x, y + bh);
        x -= w + 8 * s;
    };
    place(o.restore, sysctx_text::RestoreAll(), L"\xE777");
    place(o.log, sysctx_text::Log(), L"\xE81C");
    place(o.undo, sysctx_text::Undo(), L"\xE7A7");
    const float intro_w = (std::max)(80 * s, x - 8 * s - left);
    const float intro_h = SysCtxWrappedHeight(painter, sysctx_text::Intro(), intro_w, s);
    o.intro = D2D1::RectF(left, y, left + intro_w, y + intro_h);
    y += (std::max)(intro_h, bh) + 14 * s;

    // Layer cards.
    const float gap = 10 * s, card_w = (right - left - gap) * 0.5f;
    o.local_card = D2D1::RectF(left, y, left + card_w, y + 58 * s);
    o.system_card = D2D1::RectF(left + card_w + gap, y, right, y + 58 * s);
    y += 58 * s + 12 * s;
    if (!v) return y;

    if (v->pending == 0 && !v->message.empty() && v->state == SysCtxView::State::Ready) {
        o.notice = D2D1::RectF(left, y, right, y + 40 * s);
        y = o.notice.bottom + 10 * s;
    }

    // Columns.
    o.wide = (right - left) >= 960 * s;
    const float scenes_w = (o.wide ? 200 : 176) * s, side_w = 310 * s, col_gap = 12 * s;
    const float list_left = left + scenes_w + col_gap;
    const float list_right = o.wide ? right - side_w - col_gap : right;
    const float work_top = y;
    const float viewport_top = l.content.top + 12 * s, viewport_bottom = l.content.bottom - 12 * s;

    // Scene list: 概览 first, then the scenes grouped like the prototype.
    float sy = 8 * s;  // relative to the card top; offset once the sticky top is known
    const auto overview_rel = D2D1::RectF(6 * s, sy, scenes_w - 6 * s, sy + 32 * s);
    sy += 32 * s + 4 * s;
    std::vector<D2D1_RECT_F> scene_rel;
    std::array<D2D1_RECT_F, 3> group_rel{};
    int last_group = -1;
    for (const auto& scene : v->scenes) {
        if (scene.group != last_group && scene.group >= 0 && scene.group < 3) {
            last_group = scene.group;
            group_rel[static_cast<size_t>(scene.group)] = D2D1::RectF(6 * s, sy, scenes_w - 6 * s, sy + 26 * s);
            sy += 26 * s;
        }
        scene_rel.push_back(D2D1::RectF(6 * s, sy, scenes_w - 6 * s, sy + 32 * s));
        sy += 32 * s + 2 * s;
    }
    const float scenes_h = sy + 6 * s;

    // Entry list.
    float ly = work_top;
    o.list_head = D2D1::RectF(list_left, ly, list_right, ly + 48 * s);
    o.refresh = D2D1::RectF(list_right - 40 * s, ly + 9 * s, list_right - 10 * s, ly + 39 * s);
    ly = o.list_head.bottom + 8 * s;
    float cx = list_left + 12 * s;
    const float ch = 26 * s;
    for (int f = 0; f < kSysCtxFilterCount; ++f) {
        const float w = SysCtxTextWidth(painter, sysctx_text::Chip(*v, f), s) + 10 * s;
        if (cx + w > list_right - 12 * s && cx > list_left + 12 * s) {
            cx = list_left + 12 * s;
            ly += ch + 6 * s;
        }
        o.filters.push_back(D2D1::RectF(cx, ly, cx + w, ly + ch));
        cx += w + 6 * s;
    }
    ly += ch + 8 * s;
    if (!v->subs.empty()) {
        cx = list_left + 12 * s + SysCtxTextWidth(painter, sysctx_text::SubLabel(v->scene == 11), s);
        for (const auto& sub : v->subs) {
            const float w = SysCtxTextWidth(painter, sub, s) + 6 * s;
            if (cx + w > list_right - 12 * s && cx > list_left + 80 * s) {
                cx = list_left + 12 * s;
                ly += ch + 6 * s;
            }
            o.subs.push_back(D2D1::RectF(cx, ly, cx + w, ly + ch));
            cx += w + 6 * s;
        }
        ly += ch + 8 * s;
    }
    o.rows_top = D2D1::RectF(list_left, ly, list_right, ly + 1);
    ly += 4 * s;
    const float rh = kSysCtxRowDip * s;
    if (v->rows.empty()) {
        o.empty = D2D1::RectF(list_left, ly, list_right, ly + 96 * s);
        ly = o.empty.bottom;
    } else {
        for (size_t i = 0; i < v->rows.size(); ++i) {
            const float row_left = list_left + 6 * s, row_right = list_right - 6 * s;
            float bottom = ly + rh;
            if (!o.wide && static_cast<int>(i) == v->selected)
                bottom = LayoutSysCtxDetail(o.detail, v->rows[i], row_left + 46 * s, row_right - 12 * s, ly + rh, painter, s, true) +
                         12 * s;
            const auto r = D2D1::RectF(row_left, ly, row_right, bottom);
            const float mid = ly + rh * 0.5f;
            o.rows.push_back(r);
            o.more.push_back(D2D1::RectF(row_right - 36 * s, mid - 14 * s, row_right - 8 * s, mid + 14 * s));
            o.switches.push_back(D2D1::RectF(row_right - 88 * s, mid - 11 * s, row_right - 44 * s, mid + 11 * s));
            ly = bottom + 2 * s;
        }
    }
    ly += 8 * s;
    o.list_card = D2D1::RectF(list_left, work_top, list_right, ly);
    if (v->overview.shown) {
        // 概览 replaces the filters and the rows: cards right under the head.
        o.filters.clear();
        o.subs.clear();
        o.rows.clear();
        o.switches.clear();
        o.more.clear();
        o.empty = {};
        o.detail = {};
        ly = o.list_head.bottom;
        o.rows_top = D2D1::RectF(list_left, ly, list_right, ly + 1);
        ly = LayoutSysCtxOverview(o.overview, *v, list_left + 12 * s, list_right - 12 * s, ly + 12 * s, painter, s) +
             12 * s;
        o.list_card = D2D1::RectF(list_left, work_top, list_right, ly);
    }

    // Side panel: preview / details.
    float work_bottom = (std::max)(o.list_card.bottom, work_top + scenes_h);
    if (o.wide) {
        const float side_left = right - side_w;
        float need = 52 * s;
        SysCtxDetailLayout probe;
        if (v->side_tab == 1 && v->selected >= 0 && v->selected < static_cast<int>(v->rows.size()))
            need += LayoutSysCtxDetail(probe, v->rows[static_cast<size_t>(v->selected)], side_left + 16 * s,
                                       right - 16 * s, 0, painter, s) + 32 * s;
        else
            need += (static_cast<float>(v->preview.size()) * 28 + 220) * s;  // menu, shelf, legend
        const float viewport_h = (std::max)(240 * s, viewport_bottom - viewport_top);
        // Never taller than what is left below its (sticky) top, so the shelf
        // and the legend stay on screen before the page scrolls.
        const float room = viewport_bottom - (std::max)(work_top, viewport_top) - 8 * s;
        const float side_h = std::clamp(need, 320 * s, (std::max)(320 * s, (std::min)(viewport_h, room)));
        work_bottom = (std::max)(work_bottom, work_top + side_h);
        // Sticky: stays in view while the entry list scrolls by.
        float side_top = work_top;
        if (side_h <= viewport_h) side_top = std::clamp(viewport_top, work_top, work_bottom - side_h);
        o.side_card = D2D1::RectF(side_left, side_top, right, side_top + side_h);
        const float tw = (side_w - 16 * s - 4 * s) * 0.5f;
        o.tab_preview = D2D1::RectF(side_left + 8 * s, side_top + 8 * s, side_left + 8 * s + tw, side_top + 36 * s);
        o.tab_detail = D2D1::RectF(o.tab_preview.right + 4 * s, side_top + 8 * s, right - 8 * s, side_top + 36 * s);
        o.side_body = D2D1::RectF(side_left + 16 * s, side_top + 52 * s, right - 16 * s, o.side_card.bottom - 12 * s);
        if (v->side_tab == 1 && v->selected >= 0 && v->selected < static_cast<int>(v->rows.size()))
            LayoutSysCtxDetail(o.detail, v->rows[static_cast<size_t>(v->selected)], o.side_body.left, o.side_body.right,
                               o.side_body.top, painter, s);
    }

    // Scene card, sticky the same way when it fits.
    {
        float top = work_top;
        if (scenes_h <= viewport_bottom - viewport_top) top = std::clamp(viewport_top, work_top, work_bottom - scenes_h);
        o.scenes_card = D2D1::RectF(left, top, left + scenes_w, top + scenes_h);
        o.overview_row = D2D1::RectF(left + overview_rel.left, top + overview_rel.top, left + overview_rel.right,
                                     top + overview_rel.bottom);
        for (const auto& r : scene_rel) o.scenes.push_back(D2D1::RectF(left + r.left, top + r.top, left + r.right, top + r.bottom));
        for (size_t g = 0; g < 3; ++g) {
            const auto& r = group_rel[g];
            if (r.bottom > r.top) o.groups[g] = D2D1::RectF(left + r.left, top + r.top, left + r.right, top + r.bottom);
        }
    }

    // Floating bar over the bottom of the viewport.
    if (v->pending > 0 || v->state == SysCtxView::State::Applying) {
        const float w = (std::min)(600 * s, right - left);
        const float cx2 = (l.content.left + l.content.right) * 0.5f;
        o.pending_bar = D2D1::RectF(cx2 - w * 0.5f, l.content.bottom - 72 * s, cx2 + w * 0.5f, l.content.bottom - 16 * s);
        if (v->pending > 0 && v->state == SysCtxView::State::Ready) {
            const float mid = (o.pending_bar.top + o.pending_bar.bottom) * 0.5f;
            float bx = o.pending_bar.right - 12 * s;
            const float aw = button_w(sysctx_text::Apply(), L"\xEA18");
            o.apply = D2D1::RectF(bx - aw, mid - bh * 0.5f, bx, mid + bh * 0.5f);
            bx -= aw + 8 * s;
            const float dw = button_w(sysctx_text::Discard());
            o.discard = D2D1::RectF(bx - dw, mid - bh * 0.5f, bx, mid + bh * 0.5f);
        }
    }
    // Room for the floating bar under the last row.
    return work_bottom + (o.pending_bar.bottom > o.pending_bar.top ? 88 : 24) * s;
}

inline int HitTestSettingsSysCtx(const SettingsLayout& l, const WindowViewModel& vm, float x, float y) {
    const SysCtxView* v = vm.settings_sysctx.get();
    const SysCtxLayout& o = l.sysctx;
    if (SysCtxContains(o.local_card, x, y)) return kSysCtxLocalLayer;
    if (!v) return -1;
    if (SysCtxContains(o.pending_bar, x, y)) {
        if (SysCtxContains(o.apply, x, y)) return kSysCtxApply;
        if (SysCtxContains(o.discard, x, y)) return kSysCtxDiscard;
        return -1;  // the bar covers whatever scrolls beneath it
    }
    if (SysCtxContains(o.undo, x, y)) return v->undo_steps > 0 ? kSysCtxUndo : -1;
    if (SysCtxContains(o.log, x, y)) return kSysCtxLog;
    if (SysCtxContains(o.restore, x, y)) return v->undo_steps > 0 ? kSysCtxRestoreAll : -1;
    for (size_t i = 0; i < o.scenes.size(); ++i)
        if (SysCtxContains(o.scenes[i], x, y)) return kSysCtxSceneBase + static_cast<int>(i);
    if (SysCtxContains(o.overview_row, x, y)) return kSysCtxOverview;
    auto detail_hit = [&]() -> int {
        if (SysCtxContains(o.detail.toggle, x, y)) return kSysCtxDetailSwitch;
        if (SysCtxContains(o.detail.pulse_toggle, x, y)) return kSysCtxDetailPulse;
        if (SysCtxContains(o.detail.copy, x, y)) return kSysCtxDetailCopy;
        if (SysCtxContains(o.detail.folder, x, y)) return kSysCtxDetailFolder;
        return -1;
    };
    if (SysCtxContains(o.side_card, x, y)) {
        if (SysCtxContains(o.tab_preview, x, y)) return kSysCtxTabPreview;
        if (SysCtxContains(o.tab_detail, x, y)) return kSysCtxTabDetail;
        if (v->side_tab == 1) return detail_hit();
        if (v->overview.shown) return -1;  // the 概览 side panel only explains
        // Preview items: press to drag (zones / 已隐藏 shelf) or click to select.
        const auto pl = LayoutSysCtxPreview(*v, o.side_body, o.scale, false);
        for (size_t i = 0; i < pl.items.size(); ++i)
            if (SysCtxContains(pl.items[i], x, y)) return kSysCtxPreviewBase + static_cast<int>(i);
        return -1;
    }
    if (SysCtxContains(o.refresh, x, y)) return kSysCtxRefresh;
    if (v->overview.shown) {
        const SysCtxOverviewLayout& ov = o.overview;
        for (size_t i = 0; i < ov.slow_skip.size() && i < ov.slow_toggle.size(); ++i) {
            if (SysCtxContains(ov.slow_skip[i], x, y)) return kSysCtxSlowSkipBase + static_cast<int>(i);
            if (SysCtxContains(ov.slow_toggle[i], x, y)) return kSysCtxSlowToggleBase + static_cast<int>(i);
        }
        for (size_t i = 0; i < ov.mark_restore.size(); ++i)
            if (SysCtxContains(ov.mark_restore[i], x, y)) return kSysCtxMarkRestoreBase + static_cast<int>(i);
        return -1;
    }
    for (size_t i = 0; i < o.filters.size(); ++i)
        if (SysCtxContains(o.filters[i], x, y)) return kSysCtxFilterBase + static_cast<int>(i);
    for (size_t i = 0; i < o.subs.size(); ++i)
        if (SysCtxContains(o.subs[i], x, y)) return kSysCtxSubBase + static_cast<int>(i);
    const float s = o.scale;
    for (size_t i = 0; i < o.rows.size() && i < v->rows.size(); ++i) {
        if (!SysCtxContains(o.rows[i], x, y)) continue;
        if (SysCtxContains(o.more[i], x, y)) return kSysCtxMoreBase + static_cast<int>(i);
        if (SysCtxContains(o.switches[i], x, y) && SysCtxSwitchable(v->rows[i])) return kSysCtxRowBase + static_cast<int>(i);
        if (y < o.rows[i].top + kSysCtxRowDip * s) return kSysCtxSelectBase + static_cast<int>(i);
        return !o.wide && static_cast<int>(i) == v->selected ? detail_hit() : -1;
    }
    return -1;
}
