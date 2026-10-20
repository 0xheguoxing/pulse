// tag_view.cpp — condition bar and combination clicks (see the header).
//
// Every change builds the new query and navigates to its pulse:tags: path, so
// Back steps through the refinements.
#include "tag_view.h"

#include "app_commands.h"
#include "app_navigation.h"
#include "saved_search.h"
#include "tag_query.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"

namespace pulse {
namespace {

bool ActiveQuery(AppState& s, app::TagQuery* query) {
    const app::Tab* tab = ActiveTab(s);
    return tab && app::TagQueryFromPath(s.places, tab->current_path, query);
}

void Open(AppState& s, const app::TagQuery& query) {
    NavigateTo(s, app::MakeTagQueryPath(query));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace

bool CombineSidebarTagClick(AppState& s, const std::wstring& tag_path) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!ctrl && !shift) return false;
    app::TagQuery query;
    if (!ActiveQuery(s, &query)) return false;
    std::wstring kind, rest;
    app::ParsePulsePath(tag_path, &kind, &rest);
    const app::TagId id = s.places.ResolveTagRef(rest);
    if (id.empty()) return false;
    app::TagTermKind current{};
    const bool present = query.KindOf(id, &current);
    if (shift) {
        // Shift: 不含, or back out of 不含 when it already is.
        if (present && current == app::TagTermKind::Not) query.Remove(id);
        else query.Set(id, app::TagTermKind::Not);
    } else if (present) {
        query.Remove(id);
    } else {
        query.Set(id, app::TagTermKind::All);
    }
    Open(s, query);
    return true;
}

void HandleTagQueryChip(AppState& s, int index, POINT screen_pt) {
    app::TagQuery query;
    if (index < 0 || !ActiveQuery(s, &query) || !EnsureMenu(s)) return;
    const std::vector<app::TagQueryTerm> terms = app::TagQueryTerms(query);
    const int term_count = static_cast<int>(terms.size());
    const bool has_location = !query.in.empty();
    constexpr int kKindBase = 34000;   // + TagTermKind
    constexpr int kRemove = 34010;
    constexpr int kOpenFolder = 34011;
    constexpr int kAddBase = 34100;    // + tag index
    std::vector<ui::FluentMenuItem> items;
    if (index < term_count) {
        static constexpr l10n::StringId kLabels[] = {
            l10n::StringId::TagTermAll, l10n::StringId::TagTermAny, l10n::StringId::TagTermNot };
        for (int k = 0; k < 3; ++k) {
            ui::FluentMenuItem item;
            item.command = kKindBase + k;
            item.text = l10n::Get(kLabels[k]);
            item.radio_group = true;
            item.radio = static_cast<int>(terms[static_cast<size_t>(index)].kind) == k;
            items.push_back(std::move(item));
        }
        items.back().separator_after = true;
        ui::FluentMenuItem remove;
        remove.command = kRemove;
        remove.text = l10n::Get(l10n::StringId::TagTermRemove);
        remove.glyph = L"\xE711";
        items.push_back(std::move(remove));
    } else if (has_location && index == term_count) {
        ui::FluentMenuItem open;
        open.command = kOpenFolder;
        open.text = l10n::Get(l10n::StringId::TagLocationOpen);
        open.glyph = L"\xE8B7";
        items.push_back(std::move(open));
        ui::FluentMenuItem remove;
        remove.command = kRemove;
        remove.text = l10n::Get(l10n::StringId::TagTermRemove);
        remove.glyph = L"\xE711";
        items.push_back(std::move(remove));
    } else if (index == static_cast<int>(app::TagQueryTerms(query).size()) + (has_location ? 2 : 1)) {
        // 保存视图: name it, then it lists under the saved searches.
        const std::wstring name = PromptSmartViewName(s, screen_pt, app::TagQueryTitle(s.places, query));
        if (name.empty()) return;
        app::SavedSearch saved;
        saved.name = name;
        saved.mode = app::SavedSearchMode::Tags;
        saved.root = query.in.empty() ? std::wstring(L"pulse:tags") : query.in;
        saved.query = app::MakeTagQueryPath(query);
        if (s.savedSearches.Add(std::move(saved))) {
            s.savedSearches.Save();
            SyncSavedSearchSidebar(s);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        }
        return;
    } else {
        // ＋ 条件: the tags not in the query yet.
        for (int i = 0; i < static_cast<int>(s.places.tags.size()); ++i) {
            const auto& tag = s.places.tags[static_cast<size_t>(i)];
            if (query.Contains(tag.id)) continue;
            ui::FluentMenuItem item;
            item.command = kAddBase + i;
            item.text = tag.name;
            item.has_swatch = true;
            item.swatch_color = ui::HexColor(tag.rgb);
            items.push_back(std::move(item));
        }
        if (items.empty()) return;
    }
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command >= kKindBase && command < kKindBase + 3 && index < term_count) {
        query.Set(terms[static_cast<size_t>(index)].id, static_cast<app::TagTermKind>(command - kKindBase));
    } else if (command == kRemove) {
        if (index < term_count) query.Remove(terms[static_cast<size_t>(index)].id);
        else query.in.clear();
    } else if (command == kOpenFolder) {
        NavigateTo(s, query.in);
        return;
    } else if (command >= kAddBase && command < kAddBase + static_cast<int>(s.places.tags.size())) {
        query.Set(s.places.tags[static_cast<size_t>(command - kAddBase)].id, app::TagTermKind::All);
    } else {
        return;
    }
    Open(s, query);
}

void HandleTagQueryChipRemove(AppState& s, int index) {
    app::TagQuery query;
    if (index < 0 || !ActiveQuery(s, &query)) return;
    const std::vector<app::TagQueryTerm> terms = app::TagQueryTerms(query);
    if (index < static_cast<int>(terms.size())) query.Remove(terms[static_cast<size_t>(index)].id);
    else if (!query.in.empty() && index == static_cast<int>(terms.size())) query.in.clear();
    else return;
    Open(s, query);
}

void HandleTagQueryRefine(AppState& s, int index, bool exclude) {
    app::TagQuery query;
    const app::Tab* tab = ActiveTab(s);
    if (index < 0 || !tab || !ActiveQuery(s, &query)) return;
    // Same order as the 细分 row: the tags still in the catalog, then the folders.
    std::vector<app::TagId> tags;
    for (const auto& entry : tab->tag_refine) {
        if (s.places.FindTag(entry.first)) tags.push_back(entry.first);
    }
    if (index < static_cast<int>(tags.size())) {
        query.Set(tags[static_cast<size_t>(index)], exclude ? app::TagTermKind::Not : app::TagTermKind::All);
    } else {
        const size_t folder = static_cast<size_t>(index) - tags.size();
        if (folder >= tab->tag_locations.size()) return;
        query.in = tab->tag_locations[folder].first;
    }
    Open(s, query);
}

bool TagViewDropTags(const AppState& s, const std::wstring& view_path, std::vector<std::wstring>* tag_ids) {
    app::TagQuery query;
    if (!app::TagQueryFromPath(s.places, view_path, &query) || query.all.empty()) return false;
    if (tag_ids) *tag_ids = query.all;
    return true;
}

} // namespace pulse
