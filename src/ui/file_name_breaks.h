#pragma once

// File-name line breaking shared by the icon grids (names under icons) and
// the wrapped rename field, so a name breaks at the same places whether it
// is shown or edited.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

inline bool IsCjkNameChar(wchar_t c) {
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF);
}

// Closing punctuation never starts a line.
inline bool NoNameLineStart(wchar_t c) {
    return c != 0 && std::wcschr(L")]}\uFF09\u300D\u300F\u3011\u3015\u3009\u300B\uFF0C\u3002\u3001\uFF1B\uFF1A\uFF1F\uFF01,;:?!%\u2026", c) != nullptr;
}

// The extension's dot, when the name has a short extension (up to 8 chars).
inline size_t FileNameExtensionDot(const std::wstring& s) {
    const size_t dot = s.find_last_of(L'.');
    return dot != std::wstring::npos && dot > 0 && s.size() - dot <= 9 ? dot : std::wstring::npos;
}

// Last position a line may end at: the extension stays on one line with the
// two characters in front of it ("…v3.hdr", never "…v3." + "hdr").
inline size_t FileNameBreakLimit(const std::wstring& s, size_t ext_dot) {
    if (ext_dot == std::wstring::npos) return s.size();
    return ext_dot >= 2 ? ext_dot - 2 : 0;
}

// A natural place to wrap, between s[i - 1] and s[i]: after a space,
// underscore, hyphen, period, closing bracket or list punctuation, before an
// opening bracket, or between CJK characters - never in front of closing
// punctuation and never inside the bound extension.
inline bool GoodFileNameBreak(const std::wstring& s, size_t i, size_t ext_dot) {
    if (i == 0 || i >= s.size() || NoNameLineStart(s[i])) return false;
    if (i > FileNameBreakLimit(s, ext_dot)) return false;
    const wchar_t a = s[i - 1], b = s[i];
    if (a == L' ' || a == L'\t' || a == L'_' || a == L'-' || a == L'.' || a == L')' ||
        a == L']' || a == L',' || a == 0x00B7 || a == 0xFF09 || a == 0x3011 ||
        a == 0x300D || a == 0x3001 || a == 0xFF0C) return true;
    if (b == L'(' || b == L'[' || b == 0xFF08 || b == 0x3010 || b == 0x300C) return true;
    return IsCjkNameChar(a) || IsCjkNameChar(b);
}

struct FileNameLine { size_t start, length; };

// Greedy wrap with the file-name rules: the longest prefix that fits, pulled
// back to a natural break in its last 45% when there is one. Stops once
// `limit` lines are exceeded (callers then only need to know it overflows).
inline std::vector<FileNameLine> WrapFileName(const std::function<float(const std::wstring&)>& measure,
                                              const std::wstring& s, float width, size_t limit) {
    std::vector<FileNameLine> lines;
    const size_t ext_dot = FileNameExtensionDot(s);
    const size_t bind = FileNameBreakLimit(s, ext_dot);
    size_t at = 0;
    while (at < s.size() && lines.size() <= limit) {
        if (!lines.empty()) while (at < s.size() && s[at] == L' ') ++at;
        if (at >= s.size()) break;
        if (measure(s.substr(at)) <= width) { lines.push_back({at, s.size() - at}); break; }
        size_t lo = 1, hi = s.size() - at;
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (measure(s.substr(at, mid)) <= width) lo = mid;
            else hi = mid - 1;
        }
        size_t n = lo, natural = 0;
        for (size_t k = n; k > 0 && k * 100 >= n * 55; --k)
            if (GoodFileNameBreak(s, at + k, ext_dot)) { natural = k; break; }
        if (natural) {
            n = natural;
        } else {
            // No natural break: cut by character, but keep the extension
            // with its two characters when that unit fits on a line.
            if (at < bind && at + n > bind && measure(s.substr(bind)) <= width) n = bind - at;
            while (n > 1 && NoNameLineStart(s[at + n])) --n;
            if (n > 1 && IS_LOW_SURROGATE(s[at + n])) --n;
        }
        lines.push_back({at, n});
        at += n;
    }
    return lines;
}

// A wrapped name as a layout string: U+2028 in front of every line after
// the first, so DirectWrite (laid out without its own wrapping) breaks
// exactly there. Maps caret positions between the name and the layout.
struct BrokenFileName {
    std::wstring text;               // the layout string
    std::vector<UINT32> breaks;      // name positions where a line starts

    UINT32 ToLayout(UINT32 i) const {
        UINT32 shift = 0;
        for (const UINT32 b : breaks) { if (b <= i) ++shift; else break; }
        return i + shift;
    }
    // A layout position on a separator maps to the line start after it.
    UINT32 ToName(UINT32 j) const {
        UINT32 shift = 0;
        for (size_t k = 0; k < breaks.size(); ++k) {
            if (breaks[k] + static_cast<UINT32>(k) < j) ++shift; else break;
        }
        return j - shift;
    }
};

inline BrokenFileName BreakFileName(const std::function<float(const std::wstring&)>& measure,
                                    const std::wstring& s, float width) {
    BrokenFileName out;
    const auto lines = WrapFileName(measure, s, width, static_cast<size_t>(-2));
    size_t from = 0;
    for (size_t k = 1; k < lines.size(); ++k) {
        const size_t b = lines[k].start;
        out.text.append(s, from, b - from);
        out.text.push_back(L'\u2028');
        out.breaks.push_back(static_cast<UINT32>(b));
        from = b;
    }
    out.text.append(s, from, std::wstring::npos);
    return out;
}

}  // namespace pulse::ui
