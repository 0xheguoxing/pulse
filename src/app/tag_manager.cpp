// tag_manager.cpp — 管理标签 (pulse_tag_redesign §4.6).
//
// The panel (ui::TagManagerPopup) filters, selects and stars tags itself;
// rename, merge, group, color and delete come back here as actions. Each runs
// through the usual menus and confirm dialogs, then the panel reopens in the
// same place with the same filter, selection and scroll.
#include "tag_manager.h"

#include "app_commands.h"
#include "tag_missing.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"
#include "../ui/fluent_menu.h"
#include "../ui/tag_manager_panel.h"

#include <algorithm>

namespace pulse {
namespace {

std::vector<ui::TagManagerRow> ManagerRows(const AppState& s,
                                           const std::vector<std::wstring>& selected) {
    std::vector<ui::TagManagerRow> rows;
    rows.reserve(s.places.tags.size());
    for (const auto& tag : s.places.tags) {
        ui::TagManagerRow row;
        row.id = tag.id;
        row.name = tag.name;
        if (const auto* group = s.places.FindTagGroup(s.places.TagGroupOf(tag))) row.group = group->name;
        row.color = ui::HexColor(tag.rgb);
        row.count = static_cast<int>(tag.paths.size());
        row.missing = static_cast<int>(MissingTaggedPaths(tag).size());
        row.favorite = tag.favorite > 0;
        row.selected = std::find(selected.begin(), selected.end(), tag.id) != selected.end();
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::wstring> SelectedIds(const std::vector<ui::TagManagerRow>& rows) {
    std::vector<std::wstring> ids;
    for (const auto& row : rows) {
        if (row.selected) ids.push_back(row.id);
    }
    return ids;
}

// The star column: removals first, so a swap within nine slots fits.
void ApplyFavorites(AppState& s, const std::vector<ui::TagManagerRow>& rows) {
    bool changed = false;
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& row : rows) {
            const app::ColorTag* tag = s.places.FindTag(row.id);
            if (!tag || (tag->favorite > 0) == row.favorite || row.favorite != (pass == 1)) continue;
            changed = s.places.SetTagFavorite(row.id, row.favorite) || changed;
        }
    }
    if (changed) InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::wstring Format(l10n::StringId id, const std::wstring& name, size_t count = 0) {
    const auto& pattern = l10n::Get(id);
    const int length = _scwprintf(pattern.c_str(), name.c_str(), count);
    if (length < 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    swprintf_s(text.data(), text.size(), pattern.c_str(), name.c_str(), count);
    text.resize(static_cast<size_t>(length));
    return text;
}

bool Confirm(AppState& s, l10n::StringId title, l10n::StringId confirm_text,
             const std::wstring& message, bool danger) {
    ui::ConfirmDialogSpec confirm;
    confirm.title = l10n::Get(title);
    confirm.confirm_text = l10n::Get(confirm_text);
    confirm.cancel_text = l10n::Get(l10n::StringId::Cancel);
    confirm.danger = danger;
    confirm.message = message;
    return ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor);
}

// 合并到 ▸ the survivor: the others' items get it, then the others go.
void MergeTags(AppState& s, const std::vector<std::wstring>& ids, const std::wstring& into) {
    const app::ColorTag* target = s.places.FindTag(into);
    if (!target || ids.size() < 2) return;
    if (!Confirm(s, l10n::StringId::TagMergeTitle, l10n::StringId::TagMergeTitle,
                 Format(l10n::StringId::TagMergeConfirmFormat, target->name, ids.size() - 1), false))
        return;
    for (const auto& id : ids) {
        if (id == into) continue;
        const std::vector<std::wstring> paths = s.places.PathsForTag(id);
        std::vector<app::TagAdsUpdate> ads;
        if (!paths.empty() && s.places.SetTagsBatch(into, paths, true, &ads))
            QueueTagAds(s, std::move(ads));
        DeleteTagAndRefreshViews(s, id);
    }
}

void DeleteTags(AppState& s, const std::vector<std::wstring>& ids) {
    if (ids.empty()) return;
    if (ids.size() == 1) {
        // One tag: the sidebar's own wording (used / unused).
        const app::ColorTag* tag = s.places.FindTag(ids.front());
        if (!tag) return;
        const size_t count = tag->paths.size();
        const std::wstring message = count == 0
            ? Format(l10n::StringId::TagDeleteConfirmFormat, tag->name)
            : Format(l10n::StringId::TagDeleteUsedFormat, tag->name, count);
        if (Confirm(s, l10n::StringId::TagDelete, l10n::StringId::Delete, message, true))
            DeleteTagAndRefreshViews(s, ids.front());
        return;
    }
    if (!Confirm(s, l10n::StringId::TagDelete, l10n::StringId::Delete,
                 Format(l10n::StringId::TagDeleteManyFormat, std::to_wstring(ids.size())), true))
        return;
    for (const auto& id : ids) DeleteTagAndRefreshViews(s, id);
}

constexpr int kMergeBase = 33000;      // + index into the selection
constexpr int kGroupBase = 33200;      // + index into tag_groups
constexpr int kNoGroup = 33190;
constexpr int kNewGroup = 33191;
constexpr int kColorBase = 33400;      // + palette index
constexpr int kDeleteSelected = 33500;
constexpr int kCleanMissing = 33510;

ui::FluentMenuItem GroupSubmenu(const AppState& s, const std::vector<std::wstring>& ids) {
    // The current group gets the dot only when every selected tag shares it.
    std::wstring shared;
    bool same = true;
    for (size_t i = 0; i < ids.size(); ++i) {
        const app::ColorTag* tag = s.places.FindTag(ids[i]);
        const std::wstring group = tag ? s.places.TagGroupOf(*tag) : std::wstring{};
        if (i == 0) shared = group;
        else if (group != shared) same = false;
    }
    ui::FluentMenuItem move;
    move.text = l10n::Get(l10n::StringId::TagMoveToGroup);
    move.glyph = L"\xE8B7";
    for (int i = 0; i < static_cast<int>(s.places.tag_groups.size()) && i < 64; ++i) {
        const auto& group = s.places.tag_groups[static_cast<size_t>(i)];
        ui::FluentMenuItem item;
        item.command = kGroupBase + i;
        item.text = group.name;
        item.radio_group = true;
        item.radio = same && group.id == shared;
        move.children.push_back(std::move(item));
    }
    ui::FluentMenuItem none;
    none.command = kNoGroup;
    none.text = l10n::Get(l10n::StringId::TagNoGroup);
    none.radio_group = true;
    none.radio = same && shared.empty();
    none.separator_after = true;
    move.children.push_back(std::move(none));
    ui::FluentMenuItem create;
    create.command = kNewGroup;
    create.text = l10n::Get(l10n::StringId::TagNewGroup);
    create.glyph = L"\xE710";
    move.children.push_back(std::move(create));
    return move;
}

ui::FluentMenuItem MergeSubmenu(const AppState& s, const std::vector<std::wstring>& ids) {
    ui::FluentMenuItem merge;
    merge.text = l10n::Get(l10n::StringId::TagMergeInto);
    merge.glyph = L"\xE8B8";
    for (int i = 0; i < static_cast<int>(ids.size()) && i < 64; ++i) {
        const app::ColorTag* tag = s.places.FindTag(ids[static_cast<size_t>(i)]);
        if (!tag) continue;
        ui::FluentMenuItem item;
        item.command = kMergeBase + i;
        item.text = tag->name;
        item.has_swatch = true;
        item.swatch_color = ui::HexColor(tag->rgb);
        merge.children.push_back(std::move(item));
    }
    return merge;
}

// Runs a group / merge / color / delete choice for the selected tags.
void ApplyMenuCommand(AppState& s, int command, const std::vector<std::wstring>& ids, POINT pt) {
    if (command >= kMergeBase && command < kMergeBase + static_cast<int>(ids.size())) {
        MergeTags(s, ids, ids[static_cast<size_t>(command - kMergeBase)]);
    } else if (command == kNoGroup || command == kNewGroup ||
               (command >= kGroupBase &&
                command < kGroupBase + static_cast<int>(s.places.tag_groups.size()))) {
        std::wstring group;
        if (command == kNewGroup) {
            group = CreateTagGroupInteractive(s, pt);
            if (group.empty()) return;
        } else if (command != kNoGroup) {
            group = s.places.tag_groups[static_cast<size_t>(command - kGroupBase)].id;
        }
        for (const auto& id : ids) s.places.SetTagGroup(id, group);
    } else if (command >= kColorBase &&
               command < kColorBase + static_cast<int>(TagColorPalette(s).size())) {
        const uint32_t rgb = TagColorPalette(s)[static_cast<size_t>(command - kColorBase)];
        for (const auto& id : ids) {
            const std::vector<std::wstring> affected = s.places.PathsForTag(id);
            if (s.places.SetTagColor(id, rgb)) QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
        }
    } else if (command == kDeleteSelected) {
        DeleteTags(s, ids);
    } else if (command == kCleanMissing) {
        CleanMissingTagged(s, ids);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Right-click with several tags selected: merge, group, color, delete.
void ShowManyTagsMenu(AppState& s, const std::vector<std::wstring>& ids, POINT pt) {
    if (!EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    items.push_back(MergeSubmenu(s, ids));
    items.push_back(GroupSubmenu(s, ids));
    ui::FluentMenuItem color;
    color.text = l10n::Get(l10n::StringId::TagColorMenu);
    color.glyph = L"\xE790";
    const auto& palette = TagColorPalette(s);
    for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
        const uint32_t rgb = palette[static_cast<size_t>(i)];
        ui::FluentMenuItem swatch;
        swatch.command = kColorBase + i;
        wchar_t hex[8]{};
        swprintf_s(hex, L"#%06X", rgb);
        swatch.text = hex;
        swatch.has_swatch = true;
        swatch.swatch_color = ui::HexColor(rgb);
        color.children.push_back(std::move(swatch));
    }
    color.separator_after = true;
    items.push_back(std::move(color));
    // 清理找不到的文件, when any of them has missing items.
    int missing = 0;
    for (const auto& id : ids)
        if (const app::ColorTag* tag = s.places.FindTag(id)) missing += static_cast<int>(MissingTaggedPaths(*tag).size());
    if (missing > 0) {
        ui::FluentMenuItem clean;
        clean.command = kCleanMissing;
        clean.text = l10n::Get(l10n::StringId::TagMissingCleanSelected);
        clean.glyph = L"\xE7BA";
        items.push_back(std::move(clean));
    }
    ui::FluentMenuItem remove;
    remove.command = kDeleteSelected;
    remove.text = l10n::Get(l10n::StringId::Delete);
    remove.glyph = L"\xE74D";
    items.push_back(std::move(remove));
    ApplyMenuCommand(s, s.menu->TrackPopup(pt, std::move(items)), ids, pt);
}

ui::TagManagerText ManagerText() {
    ui::TagManagerText text;
    text.title = l10n::Get(l10n::StringId::TagManagerTitle);
    text.hint = l10n::Get(l10n::StringId::TagManagerHint);
    text.filter_all = l10n::Get(l10n::StringId::TagFilterAll);
    text.filter_favorites = l10n::Get(l10n::StringId::TagFilterFavorites);
    text.filter_unused = l10n::Get(l10n::StringId::TagFilterUnused);
    text.filter_missing = l10n::Get(l10n::StringId::TagFilterMissing);
    text.merge = l10n::Get(l10n::StringId::TagMerge);
    text.group = l10n::Get(l10n::StringId::TagMoveToGroupEllipsis);
    text.remove = l10n::Get(l10n::StringId::Delete);
    text.done = l10n::Get(l10n::StringId::QuickAccessEditDone);
    text.empty = l10n::Get(l10n::StringId::TagManagerEmpty);
    return text;
}

} // namespace

void ShowTagManager(AppState& s, POINT screen_pt) {
    const ui::TagManagerText text = ManagerText();
    ui::TagManagerView view;
    std::vector<std::wstring> selected;
    for (;;) {
        std::vector<ui::TagManagerRow> rows = ManagerRows(s, selected);
        const ui::TagManagerAction action = ui::TagManagerPopup::Edit(
            s.hwnd, &s.compositor, s.scale, screen_pt, s.darkMode, text, rows, view,
            app::kMaxFavoriteTags);
        if (action == ui::TagManagerAction::Cancelled) return;
        ApplyFavorites(s, rows);
        if (action == ui::TagManagerAction::Closed) return;
        selected = SelectedIds(rows);
        const POINT pt = view.action_pt;
        switch (action) {
        case ui::TagManagerAction::Rename:
            RenameTagInteractive(s, view.focus, pt);
            break;
        case ui::TagManagerAction::Merge:
            if (EnsureMenu(s)) {
                std::vector<ui::FluentMenuItem> items = MergeSubmenu(s, selected).children;
                ApplyMenuCommand(s, s.menu->TrackPopup(pt, std::move(items)), selected, pt);
            }
            break;
        case ui::TagManagerAction::Group:
            if (EnsureMenu(s)) {
                std::vector<ui::FluentMenuItem> items = GroupSubmenu(s, selected).children;
                ApplyMenuCommand(s, s.menu->TrackPopup(pt, std::move(items)), selected, pt);
            }
            break;
        case ui::TagManagerAction::Delete:
            DeleteTags(s, selected);
            break;
        case ui::TagManagerAction::RowMenu:
            // One tag: the same menu as its sidebar row.
            if (selected.size() == 1) ShowTagSidebarMenu(s, selected.front(), pt);
            else if (!selected.empty()) ShowManyTagsMenu(s, selected, pt);
            break;
        default:
            break;
        }
        // Deleted and merged-away tags drop out of the selection.
        std::erase_if(selected, [&](const std::wstring& id) { return !s.places.FindTag(id); });
        view.reopen = true;
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

bool SaveTagManagerSnapshot(AppState& s, const wchar_t* png_path, bool dark) {
    struct Sample { const wchar_t* name; const wchar_t* group; uint32_t rgb; int count; bool fav; };
    static const Sample kSamples[] = {
        { L"工作", L"项目", 0xE5484D, 12, true },  { L"个人", L"", 0x3E7BFA, 4, true },
        { L"待办", L"项目", 0xF2994A, 7, false }, { L"照片", L"媒体", 0x27AE60, 31, false },
        { L"重要", L"", 0x9B51E0, 3, true },       { L"归档", L"", 0x8E8E93, 0, false },
    };
    std::vector<ui::TagManagerRow> rows;
    for (const auto& sample : kSamples) {
        ui::TagManagerRow row;
        row.id = sample.name;
        row.name = sample.name;
        row.group = sample.group;
        row.color = ui::HexColor(sample.rgb);
        row.count = sample.count;
        row.favorite = sample.fav;
        rows.push_back(std::move(row));
    }
    rows[2].selected = rows[3].selected = true;
    rows[3].missing = 2;   // 照片: two gone (the red count)
    ui::TagManagerView view;
    return ui::TagManagerPopup::SaveDebugSnapshot(&s.compositor, png_path, dark, s.accentColor,
                                                  s.scale, ManagerText(), rows, view);
}

} // namespace pulse
