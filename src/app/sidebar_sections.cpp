// sidebar_sections.cpp — Section menu for the empty part of the sidebar.
//
// File Explorer puts a context menu on the navigation-pane background that decides
// which sections the pane shows. Pulse keeps its sections in a fixed order, so the
// same idea becomes one checked entry per section plus a bulk expand/collapse pair.
#include <algorithm>
#include "app_internal.h"
#include "app_commands.h"
#include "quick_access.h"
#include "tag_manager.h"
#include "tag_library.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"

namespace pulse {
namespace {

using I = l10n::StringId;

constexpr const wchar_t* kIconCheck = L"\xE73E"; // Segoe Fluent Icons: CheckMark

// Menu commands: one toggle per section, then the bulk pair.
constexpr int kToggleBase = 32000;
constexpr int kExpandAll = 32100;
constexpr int kCollapseAll = 32101;
// Section menu (opened on a section header or its empty space).
constexpr int kBuiltinBase = 32200; // + BuiltinQuickAccess
constexpr int kEditQuickAccess = 32250;
constexpr int kManageTags = 32260;
constexpr int kNewTagGroup = 32261;
constexpr int kImportTags = 32262;
constexpr int kExportTags = 32263;
constexpr int kTagModeBase = 32270; // + TagSidebarMode
constexpr int kCollapseSection = 32300;
constexpr int kExpandSection = 32301;
constexpr int kHideSection = 32302;

constexpr int kSectionCount = app::kSidebarSectionCount;

// Titles are listed in SidebarSectionId order; the cloud section shows the
// product name, exactly as its sidebar row does.
constexpr I kSectionTitles[] = {
    I::SidebarWorkspaces,
    I::SidebarQuickAccess,
    I::SidebarSavedSearches,
    I::SidebarDrives,
    I::SidebarTags,
    I::SidebarNetworkLocations,
};
// Cloud and Starred are named individually in SectionTitle().
static_assert(std::size(kSectionTitles) ==
                  static_cast<size_t>(app::SidebarSectionId::Cloud),
              "kSectionTitles must cover every header section in id order");

std::wstring SectionTitle(int section) {
    if (section == static_cast<int>(app::SidebarSectionId::Cloud)) return L"OneDrive";
    if (section == static_cast<int>(app::SidebarSectionId::Starred))
        return l10n::Get(I::StarredItems);
    if (section < 0 || section >= static_cast<int>(std::size(kSectionTitles))) return {};
    return l10n::Get(kSectionTitles[section]);
}

bool IsQuickAccessSection(int section) {
    return section == static_cast<int>(app::SidebarSectionId::QuickAccess);
}

} // namespace

void ToggleSidebarSection(AppState& s, int group) {
    if (group < 0 || group >= kSectionCount) return;
    s.sidebarHiddenMask ^= 1u << group;
    // Bringing Quick access back with every built-in link switched off would
    // leave nothing to right-click, so unhiding it also restores the links.
    if (group == static_cast<int>(app::SidebarSectionId::QuickAccess) &&
        ((s.sidebarHiddenMask >> group) & 1u) == 0) {
        s.sidebarQuickAccessHiddenMask = app::kDefaultQuickAccessHiddenMask;
    }
}

void SetEverySidebarSectionCollapsed(AppState& s, bool collapsed) {
    s.sidebarCollapsedMask = collapsed ? (1u << kSectionCount) - 1u : 0u;
}

void ShowSidebarSectionsMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const uint32_t hidden = s.sidebarHiddenMask;
    std::vector<ui::FluentMenuItem> items;
    items.reserve(kSectionCount + 2);
    // Listed in display order, so the menu matches the sidebar after a reorder.
    for (int i : app::NormalizeSidebarOrder(s.sidebarOrder)) {
        ui::FluentMenuItem item;
        item.command = kToggleBase + i;
        item.text = SectionTitle(i);
        item.checked = ((hidden >> i) & 1u) == 0;
        // TrackPopup only paints a checked row when the check arrives as a glyph,
        // which is what TrackDropdown does for its own checked entries.
        if (item.checked) item.glyph = kIconCheck;
        items.push_back(std::move(item));
    }
    items.back().separator_after = true;
    ui::FluentMenuItem expand;
    expand.command = kExpandAll;
    expand.text = l10n::Get(I::SidebarExpandAll);
    items.push_back(std::move(expand));
    ui::FluentMenuItem collapse;
    collapse.command = kCollapseAll;
    collapse.text = l10n::Get(I::SidebarCollapseAll);
    items.push_back(std::move(collapse));

    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command >= kToggleBase && command < kToggleBase + kSectionCount) {
        ToggleSidebarSection(s, command - kToggleBase);
    } else if (command == kExpandAll) {
        SetEverySidebarSectionCollapsed(s, false);
    } else if (command == kCollapseAll) {
        SetEverySidebarSectionCollapsed(s, true);
    } else {
        return;
    }
    // The sidebar scroll range shrinks with the content, so the layout clamps the
    // offset the same way a group header click does.
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowSidebarSectionMenu(AppState& s, int section, POINT screen_pt) {
    if (section < 0 || section >= kSectionCount) return;
    if (!EnsureMenu(s)) return;
    std::vector<ui::FluentMenuItem> items;
    if (IsQuickAccessSection(section)) {
        ui::FluentMenuItem edit;
        edit.command = kEditQuickAccess;
        edit.text = l10n::Get(I::QuickAccessEdit);
        edit.glyph = L"\xE70F";
        edit.separator_after = true;
        items.push_back(std::move(edit));
        // One entry per built-in link; each toggles just that row. Labels come
        // from the model and the order from the user's layout, so the menu and
        // the sidebar always read the same.
        const auto keys = app::QuickAccessDisplayKeys(
            s.sidebar.quick_access, &s.places, s.sidebarQuickAccessHiddenMask, true);
        for (const auto& key : keys) {
            const int builtin = app::BuiltinQuickAccessFromKey(key);
            if (builtin < 0) continue;
            const auto entry = std::find_if(s.sidebar.quick_access.begin(), s.sidebar.quick_access.end(),
                [&](const app::SidebarEntry& e) { return e.builtin == builtin; });
            if (entry == s.sidebar.quick_access.end()) continue;
            ui::FluentMenuItem item;
            item.command = kBuiltinBase + builtin;
            item.text = entry->label;
            item.checked = ((s.sidebarQuickAccessHiddenMask >> builtin) & 1u) == 0;
            if (item.checked) item.glyph = kIconCheck;
            items.push_back(std::move(item));
        }
        if (!items.empty()) items.back().separator_after = true;
    }
    if (section == static_cast<int>(app::SidebarSectionId::Tags)) {
        ui::FluentMenuItem manage;
        manage.command = kManageTags;
        manage.text = l10n::Get(I::TagManage);
        manage.glyph = L"\xE713";
        items.push_back(std::move(manage));
        ui::FluentMenuItem new_group;
        new_group.command = kNewTagGroup;
        new_group.text = l10n::Get(I::TagNewGroup);
        new_group.glyph = L"\xE8B7";
        items.push_back(std::move(new_group));
        // 标签库 ▸ 导入… / 导出… (another PC gets the groups and favorites too).
        ui::FluentMenuItem import_tags;
        import_tags.command = kImportTags;
        import_tags.text = l10n::Get(I::TagLibraryImport);
        import_tags.glyph = L"\xE8B5";
        items.push_back(std::move(import_tags));
        ui::FluentMenuItem export_tags;
        export_tags.command = kExportTags;
        export_tags.text = l10n::Get(I::TagLibraryExport);
        export_tags.glyph = L"\xEDE1";
        export_tags.enabled = !s.places.tags.empty();
        items.push_back(std::move(export_tags));
        ui::FluentMenuItem mode;
        mode.text = l10n::Get(I::TagSidebarMode);
        static constexpr I kModes[] = { I::TagModeFavoritesGroups, I::TagModeAllGroups, I::TagModeFavorites };
        for (int i = 0; i < 3; ++i) {
            ui::FluentMenuItem item;
            item.command = kTagModeBase + i;
            item.text = l10n::Get(kModes[i]);
            item.radio_group = true;
            item.radio = static_cast<int>(s.places.tag_sidebar_mode) == i;
            mode.children.push_back(std::move(item));
        }
        mode.separator_after = true;
        items.push_back(std::move(mode));
    }
    // A header-less section has nothing to fold: its rows are the section.
    if (!app::IsHeaderlessSection(static_cast<app::SidebarSectionId>(section))) {
        const bool collapsed = ((s.sidebarCollapsedMask >> section) & 1u) != 0;
        ui::FluentMenuItem fold;
        fold.command = collapsed ? kExpandSection : kCollapseSection;
        fold.text = l10n::Get(collapsed ? I::SidebarExpandSection : I::SidebarCollapseSection);
        items.push_back(std::move(fold));
    }
    ui::FluentMenuItem hide;
    hide.command = kHideSection;
    hide.text = l10n::Get(I::SidebarHideSection);
    items.push_back(std::move(hide));
    // #80: hidden sections come back from any section menu too, not only from
    // the empty space below the sections, which a full sidebar may not have.
    ui::FluentMenuItem restore;
    for (int id : app::NormalizeSidebarOrder(s.sidebarOrder)) {
        if (((s.sidebarHiddenMask >> id) & 1u) == 0) continue;
        ui::FluentMenuItem item;
        item.command = kToggleBase + id;
        item.text = SectionTitle(id);
        restore.children.push_back(std::move(item));
    }
    if (!restore.children.empty()) {
        items.back().separator_after = true;
        restore.text = l10n::Get(I::SidebarShowHidden);
        items.push_back(std::move(restore));
    }

    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kEditQuickAccess) {
        ShowQuickAccessEditor(s, screen_pt);
        return;
    }
    if (command == kManageTags) {
        ShowTagManager(s, screen_pt);
        return;
    }
    if (command == kNewTagGroup) {
        CreateTagGroupInteractive(s, screen_pt);
        return;
    }
    if (command == kImportTags) {
        ImportTagLibrary(s);
        return;
    }
    if (command == kExportTags) {
        ExportTagLibrary(s);
        return;
    }
    if (command >= kTagModeBase && command < kTagModeBase + 3) {
        s.places.SetTagSidebarMode(static_cast<app::TagSidebarMode>(command - kTagModeBase));
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (command >= kBuiltinBase &&
        command < kBuiltinBase + static_cast<int>(app::BuiltinQuickAccess::Count)) {
        s.sidebarQuickAccessHiddenMask ^= 1u << (command - kBuiltinBase);
    } else if (command == kCollapseSection) {
        s.sidebarCollapsedMask |= 1u << section;
    } else if (command == kExpandSection) {
        s.sidebarCollapsedMask &= ~(1u << section);
    } else if (command == kHideSection) {
        s.sidebarHiddenMask |= 1u << section;
    } else if (command >= kToggleBase && command < kToggleBase + kSectionCount) {
        if (((s.sidebarHiddenMask >> (command - kToggleBase)) & 1u) != 0)
            ToggleSidebarSection(s, command - kToggleBase);
    } else {
        return;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
