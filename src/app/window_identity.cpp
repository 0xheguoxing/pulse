#include "window_identity.h"

#include <algorithm>
#include <cwctype>

namespace pulse::app {
namespace {

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

bool IsPlace(const std::wstring& path) {
    return path.empty() || path.starts_with(L"pulse:") || path.starts_with(L"::");
}

// "C:\" and "\\server\share": the top of a path.
bool IsRoot(const std::wstring& p) {
    if (p.size() == 3 && p[1] == L':' && IsSeparator(p[2])) return true;
    if (p.size() == 2 && p[1] == L':') return true;
    if (p.size() > 2 && IsSeparator(p[0]) && IsSeparator(p[1])) {
        const size_t share = p.find_first_of(L"\\/", 2);
        return share != std::wstring::npos && share + 1 < p.size() &&
               p.find_first_of(L"\\/", share + 1) == std::wstring::npos;
    }
    return false;
}

std::wstring Normalize(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    while (p.size() > 1 && p.back() == L'\\' && !IsRoot(p)) p.pop_back();
    return p;
}

std::wstring Parent(const std::wstring& p) {
    if (IsRoot(p)) return {};
    const size_t at = p.find_last_of(L'\\');
    if (at == std::wstring::npos || at == 0) return {};
    std::wstring up = p.substr(0, at);
    if (up.size() == 2 && up[1] == L':') up += L'\\';
    // "\\server" alone is above the share root.
    if (up.size() >= 2 && up[0] == L'\\' && up[1] == L'\\' && up.find(L'\\', 2) == std::wstring::npos) return {};
    return up;
}

bool SameText(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

// `p` is `folder` or inside it.
bool IsUnder(const std::wstring& p, const std::wstring& folder) {
    if (p.size() < folder.size() || !SameText(std::wstring_view(p).substr(0, folder.size()), folder)) return false;
    return p.size() == folder.size() || folder.back() == L'\\' || p[folder.size()] == L'\\';
}

}  // namespace

std::wstring AutoNameFolder(const std::vector<std::wstring>& pane_paths) {
    std::vector<std::wstring> paths;
    for (const std::wstring& raw : pane_paths)
        if (!IsPlace(raw)) paths.push_back(Normalize(raw));
    if (paths.empty()) return {};

    std::wstring common = paths.front();
    while (!common.empty() &&
           !std::all_of(paths.begin(), paths.end(), [&](const std::wstring& p) { return IsUnder(p, common); }))
        common = Parent(common);
    const bool all_same = std::all_of(paths.begin(), paths.end(),
                                      [&](const std::wstring& p) { return SameText(p, paths.front()); });
    if (!common.empty() && (!IsRoot(common) || all_same)) return common;

    std::wstring best;
    size_t best_count = 0;
    for (const std::wstring& path : paths) {
        for (std::wstring folder = path; !folder.empty() && !IsRoot(folder); folder = Parent(folder)) {
            const size_t count = static_cast<size_t>(std::count_if(
                paths.begin(), paths.end(), [&](const std::wstring& p) { return IsUnder(p, folder); }));
            if (count < 2) continue;
            if (count > best_count || (count == best_count && folder.size() > best.size())) {
                best = folder;
                best_count = count;
            }
        }
    }
    return best;
}

int PickWindowColor(const std::vector<int>& taken) {
    int uses[kWindowColorCount] = {};
    for (int color : taken)
        if (ValidWindowColor(color)) ++uses[color];
    return static_cast<int>(std::min_element(std::begin(uses), std::end(uses)) - std::begin(uses));
}

std::wstring NormalizeWindowName(std::wstring_view typed) {
    std::wstring name;
    for (wchar_t c : typed)
        if (c >= 0x20 && c != 0x7F) name += c;
    const auto blank = [](wchar_t c) { return c == L' ' || c == L'\t' || c == 0x3000; };
    while (!name.empty() && blank(name.back())) name.pop_back();
    size_t lead = 0;
    while (lead < name.size() && blank(name[lead])) ++lead;
    name.erase(0, lead);
    if (name.size() > kWindowNameMaxLength) {
        name.resize(kWindowNameMaxLength);
        // Do not leave half of a surrogate pair at the end.
        if (!name.empty() && name.back() >= 0xD800 && name.back() <= 0xDBFF) name.pop_back();
    }
    return name;
}

}  // namespace pulse::app
