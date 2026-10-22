#pragma once
// Leading type tags on release-note items (docs/releases/<version>.md):
//   - [新功能] / [New]       a new feature in the app
//   - [新设置] / [Setting]   a new option in Settings
//   - [改进]   / [Improved]  an improvement
//   - [修复]   / [Fixed]     a fix
// Settings > About draws the tag as a coloured badge in front of the item.

#include <cstdint>
#include <string>
#include <string_view>

namespace pulse::ui {

enum class ReleaseTag : uint8_t { None = 0, New, Setting, Improved, Fixed };
inline constexpr int kReleaseTagCount = 5;

// Strips a known leading "[tag]" and the spaces after it from an item and
// returns its kind; label receives the tag as written. Anything else,
// including a link such as "[New](https://...)", is left alone.
inline ReleaseTag TakeReleaseTag(std::wstring& text, std::wstring& label) {
    label.clear();
    if (text.size() < 3 || text[0] != L'[') return ReleaseTag::None;
    const size_t close = text.find(L']');
    if (close == std::wstring::npos || close < 2 || close > 16) return ReleaseTag::None;
    if (close + 1 < text.size() && text[close + 1] == L'(') return ReleaseTag::None;
    struct Known {
        std::wstring_view name;
        ReleaseTag tag;
    };
    static constexpr Known kKnown[] = {
        {L"\u65B0\u529F\u80FD", ReleaseTag::New},       // 新功能
        {L"\u65B0\u8BBE\u7F6E", ReleaseTag::Setting},   // 新设置
        {L"\u6539\u8FDB", ReleaseTag::Improved},        // 改进
        {L"\u4FEE\u590D", ReleaseTag::Fixed},           // 修复
        {L"New", ReleaseTag::New},
        {L"Setting", ReleaseTag::Setting},
        {L"Improved", ReleaseTag::Improved},
        {L"Fixed", ReleaseTag::Fixed},
    };
    const std::wstring_view name(text.data() + 1, close - 1);
    for (const Known& known : kKnown) {
        if (name != known.name) continue;
        label.assign(name);
        size_t start = close + 1;
        while (start < text.size() && (text[start] == L' ' || text[start] == L'\t' || text[start] == 0x3000))
            ++start;
        text.erase(0, start);
        return known.tag;
    }
    return ReleaseTag::None;
}

} // namespace pulse::ui
