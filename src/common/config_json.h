#pragma once
#include <string_view>
#include <initializer_list>
#include <string>
#include <vector>
#include <utility>

namespace pulse::json {

// Syntax validation before permissive field extractors touch persisted state.
// Places historically wrote 0xRRGGBB; allow that extension only for that file.
class ConfigSyntax {
public:
    ConfigSyntax(std::wstring_view text, bool legacy_hex) : text_(text), hex_(legacy_hex) {}
    bool Object() { Space(); if (Peek() != L'{') return false; return Value(0) && (Space(), pos_ == text_.size()); }
    bool Array() { Space(); if (Peek() != L'[') return false; return Value(0) && (Space(), pos_ == text_.size()); }
    size_t MemberPosition(std::wstring_view key) {
        if (!Eat(L'{')) return std::wstring_view::npos;
        size_t found = std::wstring_view::npos;
        if (!Eat(L'}')) {
            for (;;) {
                bool match = false;
                if (!Name(key, match) || !Eat(L':')) return std::wstring_view::npos;
                Space();
                if (match && found == std::wstring_view::npos) found = pos_;
                if (!Value(1)) return std::wstring_view::npos;
                if (Eat(L'}')) break;
                if (!Eat(L',')) return std::wstring_view::npos;
            }
        }
        Space();
        return pos_ == text_.size() ? found : std::wstring_view::npos;
    }
    bool DecodeString(size_t& position, std::wstring& output) {
        pos_ = position;
        std::wstring decoded;
        if (!String(&decoded)) return false;
        position = pos_;
        output = std::move(decoded);
        return true;
    }
    bool StringArrayMember(std::wstring_view key, std::vector<std::wstring>& values) {
        if (!Eat(L'{')) return false;
        bool found = false;
        std::vector<std::wstring> parsed;
        if (!Eat(L'}')) {
            for (;;) {
                std::wstring name;
                if (!String(&name) || !Eat(L':')) return false;
                if (name == key) {
                    if (found || !Eat(L'[')) return false;
                    found = true;
                    if (!Eat(L']')) {
                        for (;;) {
                            std::wstring value;
                            if (!String(&value)) return false;
                            parsed.push_back(std::move(value));
                            if (Eat(L']')) break;
                            if (!Eat(L',')) return false;
                        }
                    }
                } else if (!Value(1)) return false;
                if (Eat(L'}')) break;
                if (!Eat(L',')) return false;
            }
        }
        Space();
        if (!found || pos_ != text_.size()) return false;
        values = std::move(parsed);
        return true;
    }
private:
    wchar_t Peek() const { return pos_ < text_.size() ? text_[pos_] : L'\0'; }
    void Space() { while (Peek() == L' ' || Peek() == L'\r' || Peek() == L'\n' || Peek() == L'\t') ++pos_; }
    bool Eat(wchar_t c) { Space(); if (Peek() != c) return false; ++pos_; return true; }
    static bool Digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
    static bool Hex(wchar_t c) { return Digit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'); }
    static bool ValidUtf16(std::wstring_view text) {
        for (size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned>(text[i]);
            if (c >= 0xDC00 && c <= 0xDFFF) return false;
            if (c >= 0xD800 && c <= 0xDBFF) {
                if (++i == text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF) return false;
            }
        }
        return true;
    }
    bool String(std::wstring* decoded = nullptr) {
        if (!Eat(L'"')) return false;
        if (decoded) decoded->clear();
        while (pos_ < text_.size()) {
            wchar_t c = text_[pos_++];
            if (c == L'"') return !decoded || ValidUtf16(*decoded);
            if (c < 0x20) return false;
            if (c != L'\\') {
                if (decoded) decoded->push_back(c);
                continue;
            }
            if (pos_ == text_.size()) return false;
            c = text_[pos_++];
            if (c == L'u') {
                unsigned value = 0;
                for (int i = 0; i < 4; ++i) {
                    if (!Hex(Peek())) return false;
                    const wchar_t digit = text_[pos_++];
                    value = value * 16 + (Digit(digit) ? digit - L'0' :
                        digit >= L'a' ? digit - L'a' + 10 : digit - L'A' + 10);
                }
                if (decoded) decoded->push_back(static_cast<wchar_t>(value));
            } else if (c != L'"' && c != L'\\' && c != L'/' && c != L'b' &&
                       c != L'f' && c != L'n' && c != L'r' && c != L't') return false;
            else if (decoded) {
                decoded->push_back(c == L'b' ? L'\b' : c == L'f' ? L'\f' : c == L'n' ? L'\n' :
                    c == L'r' ? L'\r' : c == L't' ? L'\t' : c);
            }
        }
        return false;
    }
    // String(&name) and name == key, without building name: a member name
    // without escapes is its own decoded text. Startup looks up some 150 keys
    // one by one, and each lookup used to allocate every name in the file.
    bool Name(std::wstring_view key, bool& match) {
        Space();
        const size_t open = pos_;
        if (!String()) return false;
        const std::wstring_view raw = text_.substr(open + 1, pos_ - open - 2);
        if (raw.find(L'\\') == std::wstring_view::npos) {
            match = raw == key;
            return ValidUtf16(raw);
        }
        pos_ = open;
        std::wstring name;
        if (!String(&name)) return false;
        match = name == key;
        return true;
    }
    bool Value(unsigned depth) {
        if (depth > 64) return false;
        Space();
        if (Peek() == L'"') return String();
        if (Peek() == L'{' || Peek() == L'[') {
            const bool object = text_[pos_++] == L'{';
            const wchar_t close = object ? L'}' : L']';
            if (Eat(close)) return true;
            do {
                if (object && (!String() || !Eat(L':'))) return false;
                if (!Value(depth + 1)) return false;
                if (Eat(close)) return true;
            } while (Eat(L','));
            return false;
        }
        for (auto word : {std::wstring_view(L"true"), std::wstring_view(L"false"), std::wstring_view(L"null")}) {
            if (text_.substr(pos_, word.size()) == word) { pos_ += word.size(); return true; }
        }
        if (Peek() == L'-') ++pos_;
        if (!Digit(Peek())) return false;
        if (Peek() == L'0') {
            ++pos_;
            if (hex_ && (Peek() == L'x' || Peek() == L'X')) {
                ++pos_; if (!Hex(Peek())) return false;
                while (Hex(Peek())) ++pos_;
                return true;
            }
        } else while (Digit(Peek())) ++pos_;
        if (Peek() == L'.') { ++pos_; if (!Digit(Peek())) return false; while (Digit(Peek())) ++pos_; }
        if (Peek() == L'e' || Peek() == L'E') {
            ++pos_; if (Peek() == L'+' || Peek() == L'-') ++pos_;
            if (!Digit(Peek())) return false;
            while (Digit(Peek())) ++pos_;
        }
        return true;
    }
    std::wstring_view text_;
    size_t pos_ = 0;
    bool hex_;
};

inline bool ValidConfigObject(std::wstring_view text, bool legacy_hex = false) {
    return ConfigSyntax(text, legacy_hex).Object();
}
inline bool ValidConfigArray(std::wstring_view text) {
    return ConfigSyntax(text, false).Array();
}
} // namespace pulse::json
