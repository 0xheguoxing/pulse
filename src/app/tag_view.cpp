// tag_view.cpp — condition bar and combination clicks (see the header).
//
// Every change builds the new query and navigates to its pulse:tags: path, so
// Back steps through the refinements.
#include "tag_view.h"

#include "app_commands.h"
#include "app_navigation.h"
#include "app_hosted_edit.h"
#include "saved_search.h"
#include "tag_query.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"
#include <algorithm>
#include <unordered_set>

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
        // Say why when it cannot be kept: the name is taken, or the list is full.
        const auto& existing = s.savedSearches.items();
        const bool taken = std::any_of(existing.begin(), existing.end(), [&](const app::SavedSearch& item) {
            return _wcsicmp(item.name.c_str(), name.c_str()) == 0;
        });
        if (!taken && s.savedSearches.Add(std::move(saved))) {
            s.savedSearches.Save();
            SyncSavedSearchSidebar(s);
            InvalidateRect(s.hwnd, nullptr, FALSE);
        } else if (s.hwnd) {
            wchar_t text[256]{};
            swprintf_s(text, l10n::Get(taken ? l10n::StringId::TagSmartViewNameTaken
                                             : l10n::StringId::TagSmartViewNotSaved).c_str(), name.c_str());
            s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, text, std::wstring{}, {});
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

namespace {
// What the last tag drop added, per tag, for the snack's undo.
struct TagDropUndo {
    const AppState* owner = nullptr;
    bool removed = false;   // the snack took them off (undo puts them back)
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> added;
};
TagDropUndo g_tag_drop;
} // namespace

void ApplyDropTags(AppState& s, const std::vector<std::wstring>& tag_ids, const std::vector<std::wstring>& paths) {
    TagDropUndo undo;
    undo.owner = &s;
    std::wstring names;
    std::unordered_set<std::wstring> touched;
    std::vector<app::TagAdsUpdate> ads_updates;
    for (const auto& id : tag_ids) {
        const int index = s.places.FindTagIndex(id);
        if (index < 0) continue;
        std::vector<std::wstring> fresh;
        for (const auto& path : paths)
            if (!s.places.PathHasTag(path, index)) fresh.push_back(path);
        if (fresh.empty()) continue;
        if (!names.empty()) names += L"\u300D\u300C";   // 」「
        names += s.places.tags[static_cast<size_t>(index)].name;
        s.places.SetTagsBatch(id, fresh, true, &ads_updates);
        touched.insert(fresh.begin(), fresh.end());
        undo.added.emplace_back(id, std::move(fresh));
    }
    QueueTagAds(s, std::move(ads_updates));
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (undo.added.empty() || !s.hwnd) return;   // everything had them already
    g_tag_drop = std::move(undo);
    wchar_t text[256]{};
    swprintf_s(text, l10n::Get(l10n::StringId::TagDropDoneFormat).c_str(), static_cast<int>(touched.size()),
               names.c_str());
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, text, std::wstring{},
                                   {ui::SnackAction{l10n::Get(l10n::StringId::SnackUndo), WM_TAG_DROP_UNDO}});
}

namespace {
// What has (or lacks, `has` false) the tag among paths.
std::vector<std::wstring> PathsWhereTag(const AppState& s, int index, const std::vector<std::wstring>& paths, bool has) {
    std::vector<std::wstring> out;
    for (const auto& path : paths)
        if (s.places.PathHasTag(path, index) == has) out.push_back(path);
    return out;
}

void ShowTagUndo(AppState& s, TagDropUndo undo, const std::wstring& text) {
    if (undo.added.empty() || !s.hwnd) return;
    g_tag_drop = std::move(undo);
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, text, std::wstring{},
                                   {ui::SnackAction{l10n::Get(l10n::StringId::SnackUndo), WM_TAG_DROP_UNDO}});
}
} // namespace

void ToggleTagWithUndo(AppState& s, const std::wstring& tag_id, const std::vector<std::wstring>& paths) {
    const int index = s.places.FindTagIndex(tag_id);
    if (index < 0) return;
    const bool add = s.places.GetSelectionState(tag_id, paths) != app::TagSelectionState::All;
    std::vector<std::wstring> touched = PathsWhereTag(s, index, paths, !add);
    const std::wstring name = s.places.tags[static_cast<size_t>(index)].name;
    if (touched.empty() || !ToggleTagForSelection(s, tag_id, paths)) return;
    TagDropUndo undo;
    undo.owner = &s;
    undo.removed = !add;
    wchar_t text[256]{};
    swprintf_s(text, l10n::Get(add ? l10n::StringId::TagDropDoneFormat : l10n::StringId::TagRemovedFormat).c_str(),
               static_cast<int>(touched.size()), name.c_str());
    undo.added.emplace_back(tag_id, std::move(touched));
    ShowTagUndo(s, std::move(undo), text);
}

void ClearTagsWithUndo(AppState& s, const std::vector<std::wstring>& paths) {
    TagDropUndo undo;
    undo.owner = &s;
    undo.removed = true;
    std::unordered_set<std::wstring> touched;
    for (size_t i = 0; i < s.places.tags.size(); ++i) {
        auto had = PathsWhereTag(s, static_cast<int>(i), paths, true);
        if (had.empty()) continue;
        touched.insert(had.begin(), had.end());
        undo.added.emplace_back(s.places.tags[i].id, std::move(had));
    }
    if (undo.added.empty() || !ClearTagsForSelection(s, paths)) return;
    wchar_t text[256]{};
    swprintf_s(text, l10n::Get(l10n::StringId::TagsClearedFormat).c_str(), static_cast<int>(touched.size()));
    ShowTagUndo(s, std::move(undo), text);
}

bool HandleRowTagDotPress(AppState& s, const ui::HitTestResult& hit) {
    if (hit.region != ui::HitTestResult::RowTagDot) return false;
    if (hit.sub_index < 0 || hit.sub_index >= static_cast<int>(s.places.tags.size())) return true;
    const auto panes = LiveLayout(s).VisiblePanes();
    if (hit.pane_index >= 0 && hit.pane_index < static_cast<int>(panes.size()) &&
        panes[static_cast<size_t>(hit.pane_index)] != s.pane)
        FocusPane(s, panes[static_cast<size_t>(hit.pane_index)]);
    if (s.filterEditing) HideFilterEditor(s, true);
    app::Tab* tab = ActiveTab(s);
    if (!tab) return true;
    std::wstring name = s.places.tags[static_cast<size_t>(hit.sub_index)].name;
    if (name.find_first_of(L" \t|") != std::wstring::npos) name = L"\"" + name + L"\"";
    const std::wstring wanted = L"#" + name;
    tab->filter_text = _wcsicmp(tab->filter_text.c_str(), wanted.c_str()) == 0 ? std::wstring{} : wanted;
    tab->scroll_y = 0;
    tab->scroll_x = 0;
    s.scrollTargetY = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

void UndoDropTags(AppState& s) {
    TagDropUndo undo = std::move(g_tag_drop);
    g_tag_drop = TagDropUndo{};
    if (undo.owner != &s) return;
    std::vector<app::TagAdsUpdate> ads_updates;
    for (const auto& [id, paths] : undo.added) s.places.SetTagsBatch(id, paths, undo.removed, &ads_updates);
    QueueTagAds(s, std::move(ads_updates));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void RemoveTagPathsUndoable(AppState& s,
                            const std::vector<std::pair<std::wstring, std::vector<std::wstring>>>& removals) {
    TagDropUndo undo;
    undo.owner = &s;
    undo.removed = true;
    std::unordered_set<std::wstring> touched;
    std::vector<app::TagAdsUpdate> ads_updates;
    for (const auto& [id, paths] : removals) {
        if (paths.empty() || !s.places.FindTag(id)) continue;
        s.places.SetTagsBatch(id, paths, false, &ads_updates);
        touched.insert(paths.begin(), paths.end());
        undo.added.emplace_back(id, paths);
    }
    QueueTagAds(s, std::move(ads_updates));
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (undo.added.empty() || !s.hwnd) return;
    g_tag_drop = std::move(undo);
    wchar_t text[256]{};
    swprintf_s(text, l10n::Get(l10n::StringId::TagMissingCleaned).c_str(), static_cast<int>(touched.size()));
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo, text, std::wstring{},
                                   {ui::SnackAction{l10n::Get(l10n::StringId::SnackUndo), WM_TAG_DROP_UNDO}});
}

bool TagViewDropTags(const AppState& s, const std::wstring& view_path, std::vector<std::wstring>* tag_ids) {
    app::TagQuery query;
    if (!app::TagQueryFromPath(s.places, view_path, &query) || query.all.empty()) return false;
    if (tag_ids) *tag_ids = query.all;
    return true;
}

} // namespace pulse
