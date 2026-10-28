// filter_tag_complete.cpp — see filter_tag_complete.h.
#include "filter_tag_complete.h"
#include "../ui/ui_renderer.h"

#include "app_commands.h"
#include "app_internal.h"
#include "../ui/FluentTokens.h"

#include <algorithm>
#include <cwctype>

namespace pulse {
void SyncFilterEditor(AppState& s);

namespace {

constexpr size_t kMaxShown = 8;

struct Completion {
    const AppState* owner = nullptr;
    std::vector<std::wstring> ids;   // the tags shown
    int selected = 0;
    size_t replace_from = 0;         // the name being typed starts here
    size_t replace_to = 0;           // and its token ends here
};
Completion g_list;
bool g_swallow = false;

std::wstring EditText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(edit, text.data(), length + 1)));
    return text;
}

// Index of part in text ignoring case, or -1.
int FindFolded(const std::wstring& text, const std::wstring& part) {
    if (part.empty()) return 0;
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, text.c_str(),
                           static_cast<int>(text.size()), part.c_str(), static_cast<int>(part.size()), nullptr,
                           nullptr, nullptr, 0);
}

D2D1_RECT_F Anchor(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    return s.renderer.FilterEditRect(D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                                 static_cast<float>(s.compositor.Height())),
                                     s.pane ? s.pane->filter_expand : 0.0f, tab && !tab->filter_text.empty());
}

void Accept(AppState& s) {
    const app::ColorTag* tag = g_list.selected >= 0 && g_list.selected < static_cast<int>(g_list.ids.size())
        ? s.places.FindTag(g_list.ids[static_cast<size_t>(g_list.selected)]) : nullptr;
    const Completion list = g_list;
    CloseFilterTagSuggestions(s);
    if (!tag || !s.hwndFilterEdit) return;
    std::wstring text = EditText(s.hwndFilterEdit);
    if (list.replace_from > list.replace_to || list.replace_to > text.size()) return;
    // A name with a space or | goes in quotes (#"two words").
    std::wstring name = tag->name;
    if (name.find_first_of(L" \t|") != std::wstring::npos) name = L"\"" + name + L"\"";
    text.replace(list.replace_from, list.replace_to - list.replace_from, name);
    size_t caret = list.replace_from + name.size();
    if (caret >= text.size() || !std::iswspace(text[caret])) text.insert(caret, L" ");
    ++caret;
    SetWindowTextW(s.hwndFilterEdit, text.c_str());
    SendMessageW(s.hwndFilterEdit, EM_SETSEL, caret, caret);
    SyncFilterEditor(s);
}

} // namespace

void CloseFilterTagSuggestions(AppState& s) {
    const bool open = g_list.owner == &s && !g_list.ids.empty();
    if (g_list.owner == &s || !g_list.owner) g_list = Completion{};
    if (open && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void UpdateFilterTagSuggestions(AppState& s) {
    CloseFilterTagSuggestions(s);
    if (!s.filterEditing || !s.hwndFilterEdit || s.filterSelectMode || s.places.tags.empty()) return;
    const std::wstring text = EditText(s.hwndFilterEdit);
    DWORD sel_end = 0;
    SendMessageW(s.hwndFilterEdit, EM_GETSEL, 0, reinterpret_cast<LPARAM>(&sel_end));
    const size_t caret = (std::min)(static_cast<size_t>(sel_end), text.size());
    // The token at the caret: from the last space outside quotes to the next.
    size_t start = 0;
    bool quoted = false;
    for (size_t i = 0; i < caret; ++i) {
        if (text[i] == L'"') quoted = !quoted;
        else if (!quoted && std::iswspace(text[i])) start = i + 1;
    }
    size_t end = caret;
    for (bool q = quoted; end < text.size() && (q || !std::iswspace(text[end])); ++end)
        if (text[end] == L'"') q = !q;
    const std::wstring token = text.substr(start, caret - start);
    const size_t part = token.starts_with(L"-#") ? 2 : token.starts_with(L"#") ? 1 : 0;
    if (part == 0) return;
    // The alternative being typed: after the last | outside quotes.
    size_t name_at = part;
    bool in_quotes = false;
    for (size_t k = part; k < token.size(); ++k) {
        if (token[k] == L'"') in_quotes = !in_quotes;
        else if (!in_quotes && token[k] == L'|') name_at = k + 1;
    }
    std::wstring typed = token.substr(name_at);
    if (!typed.empty() && typed.front() == L'"') typed.erase(0, 1);
    if (!typed.empty() && typed.back() == L'"') typed.pop_back();
    struct Found {
        int rank;
        size_t count;
        size_t index;
    };
    std::vector<Found> found;
    for (size_t i = 0; i < s.places.tags.size(); ++i) {
        const app::ColorTag& tag = s.places.tags[i];
        const int at = FindFolded(tag.name, typed);
        if (at < 0) continue;
        found.push_back({at == 0 ? 0 : 1, tag.paths.size(), i});
    }
    // Fully typed and the only one: nothing left to offer.
    if (found.empty() || (found.size() == 1 && FindFolded(s.places.tags[found[0].index].name, typed) == 0 &&
                          s.places.tags[found[0].index].name.size() == typed.size()))
        return;
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return a.rank != b.rank ? a.rank < b.rank : a.count > b.count;
    });
    for (size_t i = 0; i < found.size() && i < kMaxShown; ++i)
        g_list.ids.push_back(s.places.tags[found[i].index].id);
    g_list.owner = &s;
    g_list.replace_from = start + name_at;
    g_list.replace_to = end;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool FilterTagSuggestionsKey(AppState& s, WPARAM key) {
    if (g_list.owner != &s || g_list.ids.empty()) return false;
    const int n = static_cast<int>(g_list.ids.size());
    switch (key) {
    case VK_DOWN:
        g_list.selected = (g_list.selected + 1) % n;
        break;
    case VK_UP:
        g_list.selected = (g_list.selected + n - 1) % n;
        break;
    case VK_RETURN:
    case VK_TAB:
        g_swallow = true;
        Accept(s);
        return true;
    case VK_ESCAPE:
        g_swallow = true;
        CloseFilterTagSuggestions(s);
        return true;
    default:
        return false;
    }
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

bool FilterTagSuggestionsSwallowChar(WPARAM ch) {
    if (!g_swallow) return false;
    if (ch != L'\r' && ch != L'\t' && ch != 0x1B) return false;
    g_swallow = false;
    return true;
}

bool ClickFilterTagSuggestion(AppState& s, int x, int y) {
    if (g_list.owner != &s || g_list.ids.empty()) return false;
    const int row = s.renderer.FilterTagSuggestionAt(Anchor(s), static_cast<float>(s.compositor.Width()),
                                                     g_list.ids.size(), static_cast<float>(x),
                                                     static_cast<float>(y));
    if (row == -1) {
        CloseFilterTagSuggestions(s);   // elsewhere: the click goes on
        return false;
    }
    if (row >= 0) {
        g_list.selected = row;
        Accept(s);
    }
    return true;
}

void FillFilterTagSuggestionsVm(AppState& s, ui::WindowViewModel& vm) {
    vm.filter_tag_suggestions.clear();
    if (g_list.owner != &s || g_list.ids.empty() || !s.filterEditing) return;
    for (const auto& id : g_list.ids) {
        const app::ColorTag* tag = s.places.FindTag(id);
        if (!tag) continue;
        vm.filter_tag_suggestions.push_back({tag->name, ui::HexColor(tag->rgb), static_cast<int>(tag->paths.size())});
    }
    vm.filter_tag_selected = g_list.selected;
    vm.filter_tag_anchor = Anchor(s);
}
} // namespace pulse
