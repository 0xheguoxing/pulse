// xlsx_parts.h - package part names and shared formulas for the .xlsx
// readers (table preview and sheet sketch).
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::preview {

// Target of a relationship owned by `owner` ("xl/workbook.xml"; "" for the
// package relationships) as a part name: absolute targets drop the leading
// '/', relative ones resolve against the owner's folder, and "." / ".."
// segments are folded.
inline std::string ResolvePartTarget(std::string_view owner, std::string_view target) {
    std::string joined;
    if (!target.empty() && target.front() == '/') {
        joined.assign(target.substr(1));
    } else {
        const size_t slash = owner.rfind('/');
        if (slash != std::string_view::npos) joined.assign(owner.substr(0, slash + 1));
        joined.append(target);
    }
    std::vector<std::string> segments;
    std::string segment;
    for (size_t i = 0; i <= joined.size(); ++i) {
        if (i < joined.size() && joined[i] != '/' && joined[i] != '\\') {
            segment += joined[i];
            continue;
        }
        if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        segment.clear();
    }
    std::string part;
    for (const std::string& s : segments) {
        if (!part.empty()) part += '/';
        part += s;
    }
    return part;
}

// "xl/workbook.xml" -> "xl/_rels/workbook.xml.rels".
inline std::string RelationshipsPart(std::string_view owner) {
    const size_t slash = owner.rfind('/');
    const size_t name = slash == std::string_view::npos ? 0 : slash + 1;
    return std::string(owner.substr(0, name)) + "_rels/" + std::string(owner.substr(name)) + ".rels";
}

// Relationship types end in the same name in transitional and strict OOXML
// (".../officeDocument/2006/relationships/sharedStrings" and
// "http://purl.oclc.org/ooxml/officeDocument/relationships/sharedStrings").
inline bool RelationshipTypeIs(std::string_view type, std::string_view name) {
    return type.size() > name.size() && type[type.size() - name.size() - 1] == '/' &&
           type.substr(type.size() - name.size()) == name;
}

// A1 references in `formula` moved by rows / cols; "$" parts stay. String
// literals, quoted sheet names and [structured] references are copied as
// they are; a reference pushed off the sheet becomes #REF!.
inline std::wstring ShiftFormula(std::wstring_view formula, long long rows, long long cols) {
    constexpr long long kLastRow = 1048576, kLastCol = 16384;
    const auto upper = [](wchar_t c) { return c >= L'A' && c <= L'Z'; };
    const auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    const auto word = [&](wchar_t c) {
        return upper(c) || digit(c) || (c >= L'a' && c <= L'z') || c == L'_' || c == L'.' ||
               c == L'\\' || c > 0x7F;
    };
    std::wstring out;
    out.reserve(formula.size() + 8);
    size_t i = 0;
    while (i < formula.size()) {
        const wchar_t c = formula[i];
        if (c == L'"' || c == L'\'') {
            const size_t start = i++;
            while (i < formula.size()) {
                if (formula[i] != c) { ++i; continue; }
                if (i + 1 < formula.size() && formula[i + 1] == c) { i += 2; continue; }
                ++i;
                break;
            }
            out.append(formula.substr(start, i - start));
            continue;
        }
        if (c == L'[') {
            const size_t close = formula.find(L']', i);
            const size_t stop = close == std::wstring_view::npos ? formula.size() : close + 1;
            out.append(formula.substr(i, stop - i));
            i = stop;
            continue;
        }
        if ((c == L'$' || upper(c)) && (i == 0 || !word(formula[i - 1]))) {
            size_t p = i;
            const bool col_abs = formula[p] == L'$';
            if (col_abs) ++p;
            const size_t letters = p;
            while (p < formula.size() && upper(formula[p]) && p - letters < 4) ++p;
            const size_t letter_count = p - letters;
            const bool row_abs = p < formula.size() && formula[p] == L'$';
            if (row_abs) ++p;
            const size_t digits = p;
            while (p < formula.size() && digit(formula[p]) && p - digits < 8) ++p;
            const size_t digit_count = p - digits;
            const bool ends = p >= formula.size() ||
                (!word(formula[p]) && formula[p] != L'(' && formula[p] != L'!');
            if (letter_count >= 1 && letter_count <= 3 && digit_count >= 1 && ends) {
                long long column = 0, line = 0;
                for (size_t k = letters; k < letters + letter_count; ++k) column = column * 26 + (formula[k] - L'A' + 1);
                for (size_t k = digits; k < digits + digit_count; ++k) line = line * 10 + (formula[k] - L'0');
                if (column <= kLastCol && line >= 1 && line <= kLastRow) {
                    if (!col_abs) column += cols;
                    if (!row_abs) line += rows;
                    if (column < 1 || column > kLastCol || line < 1 || line > kLastRow) {
                        out += L"#REF!";
                    } else {
                        std::wstring name;
                        for (long long n = column; n > 0; n = (n - 1) / 26)
                            name.insert(name.begin(), static_cast<wchar_t>(L'A' + (n - 1) % 26));
                        if (col_abs) out += L'$';
                        out += name;
                        if (row_abs) out += L'$';
                        out += std::to_wstring(line);
                    }
                    i = p;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// Shared formulas: the anchor cell holds the text, later cells of the group
// only its index; their formula is the anchor's moved by their offset.
class SharedFormulas {
public:
    // Non-empty text: an anchor, remembered. Empty text: a member, filled in
    // from its anchor when the group is known.
    void Resolve(unsigned group, long long row, long long col, std::wstring& text) {
        if (!text.empty()) {
            if (anchors_.size() < kMaxGroups || anchors_.count(group)) anchors_[group] = {row, col, text};
            return;
        }
        const auto it = anchors_.find(group);
        if (it != anchors_.end())
            text = ShiftFormula(it->second.text, row - it->second.row, col - it->second.col);
    }

private:
    struct Anchor { long long row = 0, col = 0; std::wstring text; };
    static constexpr size_t kMaxGroups = 65536;
    std::map<unsigned, Anchor> anchors_;
};

} // namespace pulse::preview
