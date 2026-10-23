#include "jump_match.h"
#include "../index/filename_pinyin.h"
#include <algorithm>
#include <cwctype>
#include <windows.h>

namespace pulse::app {
namespace {
std::wstring Fold(std::wstring_view text) {
    std::wstring out(text);
    if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}
std::wstring_view Leaf(std::wstring_view path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring_view::npos || slash + 1 >= path.size() ? path : path.substr(slash + 1);
}
bool Separator(wchar_t c) {
    return c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'(' || c == L')' || c == L'[' || c == L']';
}
bool WordStart(std::wstring_view name, size_t i) {
    if (i == 0) return true;
    const wchar_t prev = name[i - 1], c = name[i];
    if (Separator(prev)) return !Separator(c);
    // camelCase and letter/digit boundaries: "MyProject2024"
    if (iswupper(c) && iswlower(prev)) return true;
    return iswdigit(c) && !iswdigit(prev);
}
bool AsciiWord(std::wstring_view word) {
    return std::all_of(word.begin(), word.end(), [](wchar_t c) { return c < 128 && iswalnum(c); });
}
int WordRank(std::wstring_view name, std::wstring_view folded_name, std::wstring_view word, bool pinyin) {
    if (word.empty()) return 0;
    if (folded_name == word) return 0;
    const auto at = folded_name.find(word);
    if (at == 0) return 1;
    if (at != std::wstring_view::npos) {
        for (size_t i = at; i != std::wstring_view::npos; i = folded_name.find(word, i + 1))
            if (WordStart(name, i)) return 2;
        return 3;
    }
    if (pinyin) {
        if (const auto match = index::FindPinyinMatch(name, word)) return match.start == 0 ? 4 : 5;
    }
    if (AsciiWord(word) && word.size() >= 2) {
        std::wstring initials;
        for (size_t i = 0; i < name.size(); ++i)
            if (WordStart(name, i) && name[i] < 128 && iswalnum(name[i])) initials.push_back(static_cast<wchar_t>(towlower(name[i])));
        size_t k = 0;
        for (wchar_t c : initials) if (k < word.size() && c == word[k]) ++k;
        if (k == word.size()) return 6;
    }
    return -1;
}
// "项目\jg": the head must appear in the parent path (as text or pinyin of
// one of its folders).
bool HeadMatches(std::wstring_view path, std::wstring_view head, bool pinyin) {
    const auto leaf = Leaf(path);
    const std::wstring_view parent = path.substr(0, path.size() - leaf.size());
    const std::wstring folded_parent = Fold(parent);
    size_t begin = 0;
    while (begin < head.size()) {
        size_t end = head.find_first_of(L"\\/", begin);
        if (end == std::wstring_view::npos) end = head.size();
        const auto part = head.substr(begin, end - begin);
        begin = end + 1;
        if (part.empty()) continue;
        if (folded_parent.find(part) != std::wstring::npos) continue;
        bool found = false;
        if (pinyin) {
            size_t s = 0;
            while (!found && s < parent.size()) {
                size_t e = parent.find_first_of(L"\\/", s);
                if (e == std::wstring_view::npos) e = parent.size();
                if (e > s && index::FindPinyinMatch(parent.substr(s, e - s), part)) found = true;
                s = e + 1;
            }
        }
        if (!found) return false;
    }
    return true;
}

int NameRank(std::wstring_view path, std::wstring_view name, std::wstring_view query, bool pinyin) {
    if (name.empty()) return -1;
    const std::wstring folded_name = Fold(name);
    const std::wstring folded_query = Fold(query);
    int worst = -1;
    bool any = false;
    size_t begin = 0;
    while (begin < folded_query.size()) {
        size_t end = folded_query.find_first_of(L" \t", begin);
        if (end == std::wstring::npos) end = folded_query.size();
        std::wstring_view word(folded_query.data() + begin, end - begin);
        begin = end + 1;
        if (word.empty()) continue;
        const auto slash = word.find_last_of(L"\\/");
        if (slash != std::wstring_view::npos) {
            if (!HeadMatches(path, word.substr(0, slash), pinyin)) return -1;
            word = word.substr(slash + 1);
            if (word.empty()) continue;  // "项目\" alone lists children; nothing to rank here
        }
        const int rank = WordRank(name, folded_name, word, pinyin);
        if (rank < 0) return -1;
        worst = (std::max)(worst, rank);
        any = true;
    }
    return any ? worst : -1;
}
}

int JumpMatchRank(std::wstring_view path, std::wstring_view query, bool pinyin, std::wstring_view label) {
    const auto leaf = Leaf(path);
    const int by_leaf = NameRank(path, leaf, query, pinyin);
    if (label.empty() || label == leaf) return by_leaf;
    const int by_label = NameRank(path, label, query, pinyin);
    if (by_leaf < 0) return by_label;
    return by_label < 0 ? by_leaf : (std::min)(by_leaf, by_label);
}

std::vector<size_t> RankJumpFolders(const std::vector<JumpCandidate>& folders, std::wstring_view query,
                                    bool pinyin, size_t limit) {
    std::vector<std::pair<int, size_t>> hits;
    for (size_t i = 0; i < folders.size(); ++i) {
        const int rank = JumpMatchRank(folders[i].path, query, pinyin, folders[i].label);
        if (rank >= 0) hits.push_back({rank, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<size_t> out;
    for (const auto& hit : hits) {
        if (out.size() >= limit) break;
        out.push_back(hit.second);
    }
    return out;
}
}
