#include "app_internal.h"
#include "search_query.h"
#include "../common/localization.h"
#include "../ui/address_search_layout.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cwctype>

namespace pulse {
namespace {
void SaveHistory(AppState& s) {
    if (!s.searchHistory.persist) return;
    s.searchHistoryWriter.Submit(s.searchHistory);
}

bool Contains(const std::wstring& text, const std::wstring& query) {
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
        [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }) != text.end();
}

// "PDF · This week · Greater than 10 MB" for the smart-filter suggestion row.
std::wstring SmartFilterLabels(const app::AdvancedSearchSpec& spec) {
    using I = l10n::StringId;
    std::vector<std::wstring> parts;
    switch (spec.kind) {
    case index::SearchKind::Folder: parts.push_back(l10n::Get(I::KindFolder)); break;
    case index::SearchKind::Document: parts.push_back(l10n::Get(I::KindDocument)); break;
    case index::SearchKind::Image: parts.push_back(l10n::Get(I::KindImage)); break;
    case index::SearchKind::Video: parts.push_back(l10n::Get(I::KindVideo)); break;
    case index::SearchKind::Audio: parts.push_back(l10n::Get(I::KindAudio)); break;
    case index::SearchKind::Archive: parts.push_back(l10n::Get(I::KindArchive)); break;
    case index::SearchKind::Code: parts.push_back(l10n::Get(I::KindCode)); break;
    case index::SearchKind::Custom: {
        std::wstring exts = L".";
        for (wchar_t c : spec.custom_exts) {
            if (c == L';') exts += L" .";
            else exts.push_back(static_cast<wchar_t>(towupper(c)));
        }
        parts.push_back(std::move(exts));
        break;
    }
    default: break;
    }
    switch (spec.date) {
    case app::DatePreset::Today: parts.push_back(l10n::Get(I::DateToday)); break;
    case app::DatePreset::Yesterday: parts.push_back(l10n::Get(I::DateYesterday)); break;
    case app::DatePreset::ThisWeek: parts.push_back(l10n::Get(I::DateThisWeek)); break;
    case app::DatePreset::ThisMonth: parts.push_back(l10n::Get(I::DateThisMonth)); break;
    case app::DatePreset::ThisYear: parts.push_back(l10n::Get(I::DateThisYear)); break;
    default: break;
    }
    switch (spec.size) {
    case app::SizePreset::Lt1MB: parts.push_back(l10n::Get(I::SizeLt1MB)); break;
    case app::SizePreset::Gt10MB: parts.push_back(l10n::Get(I::SizeGt10MB)); break;
    case app::SizePreset::Gt100MB: parts.push_back(l10n::Get(I::SizeGt100MB)); break;
    case app::SizePreset::Gt1GB: parts.push_back(l10n::Get(I::SizeGt1GB)); break;
    case app::SizePreset::Custom:
        parts.push_back(spec.size_custom.starts_with(L"size:") ? spec.size_custom.substr(5) : spec.size_custom);
        break;
    default: break;
    }
    std::wstring out;
    for (const auto& part : parts) {
        if (!out.empty()) out += L" \u00b7 ";
        out += part;
    }
    return out;
}

std::wstring ScopeLabel(const app::SearchHistoryEntry& entry) {
    std::wstring raw;
    app::ParsePulsePath(entry.path, nullptr, &raw);
    const auto spec = app::ParseSearchQuery(raw);
    if (spec.location == app::LocationScope::CustomFolder) return spec.custom_folder;
    if (spec.location == app::LocationScope::CurrentFolder) return spec.current_folder;
    return l10n::Get(l10n::StringId::SearchScopeAll);
}
}

void RecordSearchHistory(AppState& s, const std::wstring& path) {
    std::wstring kind, raw;
    if (!app::ParsePulsePath(path, &kind, &raw) || kind != L"search") return;
    auto query = app::SearchDisplayNeedle(raw);
    if (query.empty()) query = raw;
    if (s.searchHistory.Record(query, path)) SaveHistory(s);
}

void ShowAddressSearchHistory(AppState& s) {
    if (!s.addressSearching || s.searchHistoryOpen || !s.hwndAddressEdit || !EnsureMenu(s)) return;
    if (s.menu->IsOpen()) return;
    auto* tab = ActiveTab(s);
    auto* pane = s.pane;
    if (!tab) return;
    const int length = GetWindowTextLengthW(s.hwndAddressEdit);
    std::wstring draft(static_cast<size_t>(length) + 1, L'\0');
    draft.resize(GetWindowTextW(s.hwndAddressEdit, draft.data(), length + 1));
    // Nothing typed and nothing remembered: an empty popup would only cover the toolbar.
    if (draft.empty() && s.searchHistory.entries.empty()) return;
    s.searchHistoryOpen = true;
    const bool previous_ignore = s.addressIgnoreKillFocus;
    s.addressIgnoreKillFocus = true;
    bool repeat = true;
    bool show_all = true;
    while (repeat && s.addressSearching && ActiveTab(s) == tab && s.pane == pane) {
        repeat = false;
        const auto entries = s.searchHistory.entries;
        auto items_for = [&](const std::wstring& filter) {
            std::vector<ui::FluentMenuItem> items;
            auto header = [&](l10n::StringId id) {
                ui::FluentMenuItem item;
                item.text = l10n::Get(id);
                item.enabled = false;
                items.push_back(std::move(item));
            };
            const auto first = filter.find_first_not_of(L" \t");
            const std::wstring query = first == std::wstring::npos ? std::wstring{}
                : filter.substr(first, filter.find_last_not_of(L" \t") - first + 1);
            if (!query.empty()) {
                // Say what Enter will do, and offer the other ways to run this query.
                const auto scope = s.addressSearchCurrent && !s.addressSearchRoot.empty()
                    ? app::TabTitle(s.addressSearchRoot) : l10n::Get(l10n::StringId::SearchScopeAll);
                auto suggestion = [&](int command, l10n::StringId format, const wchar_t* glyph,
                                      const wchar_t* shortcut, bool with_scope) {
                    std::wstring text(query.size() + scope.size() + 128, L'\0');
                    const int written = with_scope
                        ? swprintf_s(text.data(), text.size(), l10n::Get(format).c_str(), scope.c_str(), query.c_str())
                        : swprintf_s(text.data(), text.size(), l10n::Get(format).c_str(), query.c_str());
                    text.resize(written > 0 ? static_cast<size_t>(written) : 0);
                    ui::FluentMenuItem item;
                    item.command = command;
                    item.text = std::move(text);
                    item.glyph = glyph;
                    item.shortcut = shortcut;
                    items.push_back(std::move(item));
                };
                header(l10n::StringId::SearchSuggestions);
                const bool content = s.addressSearchContent;
                suggestion(content ? 11 : 10, content ? l10n::StringId::SearchSuggestContent : l10n::StringId::SearchSuggestName,
                           content ? L"\xE8A5" : L"\xE721", L"Enter", true);
                suggestion(content ? 10 : 11, content ? l10n::StringId::SearchSuggestName : l10n::StringId::SearchSuggestContent,
                           content ? L"\xE721" : L"\xE8A5", content ? L"" : L"Alt+Enter", true);
                if (s.addressSearchCurrent && !s.addressSearchRoot.empty())
                    suggestion(12, l10n::StringId::SearchSuggestAll, L"\xE774", L"Shift+Enter", false);
                // "report pdf 本周" -> offer to turn the filter words into real filters.
                app::AdvancedSearchSpec smart;
                std::wstring rest;
                if (!content && app::ExtractSmartFilters(query, smart, rest)) {
                    const auto filters = SmartFilterLabels(smart);
                    std::wstring text(rest.size() + filters.size() + 128, L'\0');
                    const int written = rest.empty()
                        ? swprintf_s(text.data(), text.size(), l10n::Get(l10n::StringId::SearchSuggestSmartOnly).c_str(), filters.c_str())
                        : swprintf_s(text.data(), text.size(), l10n::Get(l10n::StringId::SearchSuggestSmart).c_str(), rest.c_str(), filters.c_str());
                    text.resize(written > 0 ? static_cast<size_t>(written) : 0);
                    ui::FluentMenuItem item;
                    item.command = 13;
                    item.text = std::move(text);
                    item.glyph = L"\xE71C";
                    // Right after the header: the most specific suggestion leads.
                    items.insert(items.end() - (s.addressSearchCurrent && !s.addressSearchRoot.empty() ? 3 : 2), std::move(item));
                }
            }
            std::vector<ui::FluentMenuItem> history;
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto scope = ScopeLabel(entries[i]);
                if (!filter.empty() && !Contains(entries[i].query, filter) && !Contains(scope, filter)) continue;
                ui::FluentMenuItem item;
                item.command = 100 + static_cast<int>(i);
                item.trailing_command = 1000 + static_cast<int>(i);
                item.text = entries[i].query;
                item.glyph = L"\xE81C";
                item.shortcut = scope;
                // A full folder path must not squeeze the search term out of a narrow row.
                if (const auto slash = scope.find_last_of(L"\\/"); slash != std::wstring::npos && slash + 1 < scope.size())
                    item.shortcut = scope.substr(slash + 1);
                if (item.shortcut.size() > 14) item.shortcut = item.shortcut.substr(0, 13) + L"\u2026";
                std::wstring raw;
                app::ParsePulsePath(entries[i].path, nullptr, &raw);
                const auto& mode = l10n::Get(app::SplitSearchQueryText(raw).content.present()
                    ? l10n::StringId::SearchModeContent : l10n::StringId::SearchModeName);
                item.shortcut += L" · " + mode;
                item.tooltip = entries[i].query + L"\n" + scope + L" · " + mode;
                history.push_back(std::move(item));
            }
            if (!history.empty()) {
                header(query.empty() ? l10n::StringId::SearchHistory : l10n::StringId::SearchRecent);
                for (auto& item : history) items.push_back(std::move(item));
            } else if (query.empty()) {
                // Only an empty box reports missing history; while typing it is just noise.
                header(l10n::StringId::SearchHistory);
                ui::FluentMenuItem empty;
                empty.text = l10n::Get(l10n::StringId::SearchHistoryEmpty);
                empty.enabled = false;
                items.push_back(std::move(empty));
            }
            if (!entries.empty() && filter.empty()) {
                items.back().separator_after = true;
                ui::FluentMenuItem clear;
                clear.command = 2;
                clear.secondary = true;
                clear.text = l10n::Get(l10n::StringId::SearchHistoryClear);
                clear.glyph = L"\xE74D";
                items.push_back(std::move(clear));
            }
            return items;
        };

        const auto field = ui::LayoutAddressSearch(s.renderer.SearchBarRect(
            static_cast<float>(s.compositor.Width())), s.scale).input;
        RECT anchor{static_cast<LONG>(field.left), static_cast<LONG>(field.top),
                    static_cast<LONG>(field.right), static_cast<LONG>(field.bottom)};
        MapWindowPoints(s.hwnd, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
        s.menu->SetAnchorRect(anchor);
        s.menu->SetExternalFilterEdit(s.hwndAddressEdit);
        // At least 360 dip so the scope/mode column ("All · Contents") is not cut off.
        s.menu->SetFilterMinWidth((std::max)((field.right - field.left) / s.scale, 360.0f));
        s.menu->SetMaxVisibleRows(9);
        s.menu->SetInitialFilterText(draft);
        s.menu->SetFilterPlaceholder(l10n::Get(l10n::StringId::SearchHistory));
        s.menu->SetHoverFirstOnOpen(false);
        s.menu->SetSelectAllOnOpen(false);
        wchar_t debug_path[32768]{};
        if (s.shot.active && s.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_HISTORY_SHOT", debug_path, ARRAYSIZE(debug_path))) {
            s.menu->SetTheme(s.darkMode, s.accentColor);
            s.menu->SaveDebugSnapshot(debug_path, items_for(L""));
            break;
        }
        const int command = s.menu->TrackPopup({anchor.left, anchor.top},
            items_for(show_all ? L"" : draft), items_for);
        draft = s.menu->LastFilterQuery();
        if (!s.addressSearching || ActiveTab(s) != tab || s.pane != pane) break;
        if (command == 2) {
            if (s.searchHistory.Clear()) SaveHistory(s);
            repeat = true;
        } else if (command >= 1000 && command < 1000 + static_cast<int>(entries.size())) {
            if (s.searchHistory.Remove(entries[static_cast<size_t>(command - 1000)].path)) SaveHistory(s);
            repeat = true;
        } else if (command == 13) {
            app::AdvancedSearchSpec spec;
            std::wstring rest;
            app::ExtractSmartFilters(draft, spec, rest);
            const auto filters = spec;
            spec = app::ParseSearchQuery(app::CompileNameQueryInput(rest,
                s.addressSearchCurrent ? s.addressSearchRoot : L""));
            if (filters.kind != index::SearchKind::Any) {
                spec.kind = filters.kind;
                spec.custom_exts = filters.custom_exts;
            }
            if (filters.date != app::DatePreset::Any) spec.date = filters.date;
            if (filters.size != app::SizePreset::Any) {
                spec.size = filters.size;
                spec.size_custom = filters.size_custom;
            }
            const auto path = app::MakeSearchPath(app::CompileSearchQuery(spec));
            s.addressLiveDue = s.addressHistoryDue = 0;
            HideAddressEditor(s, false);
            NavigateTo(s, path);
            RecordSearchHistory(s, path);
            ShowAddressSearch(s);
            break;
        } else if (command >= 10 && command <= 12) {
            if (command == 12) s.addressSearchCurrent = false;
            else if (s.addressSearchContent != (command == 11)) {
                s.addressSearchContent = command == 11;
                const auto& cue = l10n::Get(s.addressSearchContent ? l10n::StringId::SearchContentHint : l10n::StringId::SearchNameHint);
                SendMessageW(s.hwndAddressEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
            }
            SaveAddressSearchDraft(s);
            SubmitAddressSearch(s);
            FocusSearchResults(s, false);
            break;
        } else if (command >= 100 && command < 100 + static_cast<int>(entries.size())) {
            const auto path = entries[static_cast<size_t>(command - 100)].path;
            s.addressLiveDue = s.addressHistoryDue = 0;
            HideAddressEditor(s, false);
            NavigateTo(s, path);
            ShowAddressSearch(s);
            break;
        } else {
            SaveAddressSearchDraft(s);
            if (s.menu->LastFilterCommitted()) {
                // Enter variants mirror the suggestion rows: Shift = everywhere, Alt = contents.
                if (GetKeyState(VK_SHIFT) & 0x8000) s.addressSearchCurrent = false;
                if ((GetKeyState(VK_MENU) & 0x8000) && !s.addressSearchContent) {
                    s.addressSearchContent = true;
                    const auto& cue = l10n::Get(l10n::StringId::SearchContentHint);
                    SendMessageW(s.hwndAddressEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
                }
                SubmitAddressSearch(s);
                FocusSearchResults(s, false);
                break;
            }
        }
        show_all = false;
    }
    s.addressIgnoreKillFocus = previous_ignore;
    s.searchHistoryOpen = false;
    if (s.searchScopePending) {
        s.searchScopePending = false;
        if (s.addressSearching && ActiveTab(s) == tab && s.pane == pane)
            ShowAddressSearchScope(s);
    }
}
}
