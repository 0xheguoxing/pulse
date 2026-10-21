#include "json_value.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pulse::json {

bool Value::get_int(int64_t& out, int64_t minimum, int64_t maximum) const noexcept {
    if (!is_number() || !std::isfinite(number_) || std::floor(number_) != number_) return false;
    if (number_ < static_cast<double>(minimum) || number_ > static_cast<double>(maximum)) return false;
    out = static_cast<int64_t>(number_);
    return true;
}

const std::string& Value::as_string() const noexcept {
    static const std::string kEmpty;
    return is_string() ? string_ : kEmpty;
}

const Value& Value::at(size_t i) const noexcept {
    static const Value kNull;
    return i < items_.size() ? items_[i] : kNull;
}

const Value* Value::find(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

std::string Value::get_string(std::string_view key, std::string_view fallback) const {
    const Value* v = find(key);
    return v && v->is_string() ? v->string_ : std::string(fallback);
}

bool Value::get_bool(std::string_view key, bool fallback) const noexcept {
    const Value* v = find(key);
    return v && v->is_bool() ? v->bool_ : fallback;
}

Value& Value::push(Value v) {
    if (is_null()) type_ = Type::Array;
    items_.push_back(std::move(v));
    return items_.back();
}

Value& Value::set(std::string_view key, Value v) {
    if (is_null()) type_ = Type::Object;
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) { items_[i] = std::move(v); return items_[i]; }
    }
    keys_.emplace_back(key);
    items_.push_back(std::move(v));
    return items_.back();
}

// ---- parsing ----

class Parser {
public:
    Parser(std::string_view text, const ParseLimits& limits) : s_(text), limits_(limits) {}

    bool Run(Value& out, std::string* error) {
        bool ok = s_.size() <= limits_.max_bytes;
        if (!ok) Fail("input too large");
        if (ok) { SkipSpace(); ok = ParseValue(out, 0); }
        if (ok) { SkipSpace(); if (pos_ != s_.size()) ok = Fail("trailing characters"); }
        if (!ok && error) *error = error_ + " at " + std::to_string(pos_);
        return ok;
    }

private:
    bool Fail(const char* what) {
        if (error_.empty()) error_ = what;
        return false;
    }
    void SkipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool Literal(const char* word) {
        const size_t n = std::strlen(word);
        if (s_.substr(pos_, n) != word) return Fail("invalid literal");
        pos_ += n;
        return true;
    }

    bool ParseValue(Value& out, size_t depth) {
        if (pos_ >= s_.size()) return Fail("unexpected end");
        switch (s_[pos_]) {
        case '{': return ParseObject(out, depth + 1);
        case '[': return ParseArray(out, depth + 1);
        case '"': out.type_ = Type::String; return ParseString(out.string_);
        case 't': out = Value(true); return Literal("true");
        case 'f': out = Value(false); return Literal("false");
        case 'n': out = Value(); return Literal("null");
        default: return ParseNumber(out);
        }
    }

    bool ParseObject(Value& out, size_t depth) {
        if (depth > limits_.max_depth) return Fail("nested too deeply");
        out = Value::Object();
        ++pos_;
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        for (;;) {
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"') return Fail("expected a key");
            std::string key;
            if (!ParseString(key)) return false;
            for (const auto& k : out.keys_)
                if (k == key) return Fail("duplicate key");
            SkipSpace();
            if (pos_ >= s_.size() || s_[pos_] != ':') return Fail("expected ':'");
            ++pos_;
            SkipSpace();
            Value v;
            if (!ParseValue(v, depth)) return false;
            out.keys_.push_back(std::move(key));
            out.items_.push_back(std::move(v));
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value& out, size_t depth) {
        if (depth > limits_.max_depth) return Fail("nested too deeply");
        out = Value::Array();
        ++pos_;
        SkipSpace();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        for (;;) {
            SkipSpace();
            Value v;
            if (!ParseValue(v, depth)) return false;
            out.items_.push_back(std::move(v));
            SkipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            return Fail("expected ',' or ']'");
        }
    }

    static int Hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool Hex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) return Fail("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = Hex(s_[pos_ + i]);
            if (h < 0) return Fail("bad \\u escape");
            out = (out << 4) | static_cast<uint32_t>(h);
        }
        pos_ += 4;
        return true;
    }
    static void Append(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    // One UTF-8 sequence starting at pos_, validated (no overlongs, no surrogates).
    bool CopyUtf8(std::string& out) {
        const auto b0 = static_cast<unsigned char>(s_[pos_]);
        size_t n = 0;
        uint32_t cp = 0, minimum = 0;
        if (b0 >= 0xC2 && b0 <= 0xDF) { n = 1; cp = b0 & 0x1F; minimum = 0x80; }
        else if (b0 >= 0xE0 && b0 <= 0xEF) { n = 2; cp = b0 & 0x0F; minimum = 0x800; }
        else if (b0 >= 0xF0 && b0 <= 0xF4) { n = 3; cp = b0 & 0x07; minimum = 0x10000; }
        else return Fail("invalid UTF-8");
        if (pos_ + n >= s_.size()) return Fail("truncated UTF-8");
        for (size_t i = 1; i <= n; ++i) {
            const auto b = static_cast<unsigned char>(s_[pos_ + i]);
            if ((b & 0xC0) != 0x80) return Fail("invalid UTF-8");
            cp = (cp << 6) | (b & 0x3F);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return Fail("invalid UTF-8");
        out.append(s_.data() + pos_, n + 1);
        pos_ += n + 1;
        return true;
    }

    bool ParseString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < s_.size()) {
            const auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') { ++pos_; return true; }
            if (c < 0x20) return Fail("control character in string");
            if (c >= 0x80) { if (!CopyUtf8(out)) return false; continue; }
            if (c != '\\') { out += static_cast<char>(c); ++pos_; continue; }
            if (++pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(cp)) return false;
                if (cp >= 0xDC00 && cp <= 0xDFFF) return Fail("unpaired surrogate");
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low = 0;
                    if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') return Fail("unpaired surrogate");
                    pos_ += 2;
                    if (!Hex4(low)) return false;
                    if (low < 0xDC00 || low > 0xDFFF) return Fail("unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                Append(out, cp);
                break;
            }
            default: return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }

    bool ParseNumber(Value& out) {
        const size_t start = pos_;
        auto digit = [&](size_t i) { return i < s_.size() && s_[i] >= '0' && s_[i] <= '9'; };
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (!digit(pos_)) return Fail("invalid value");
        if (s_[pos_] == '0') ++pos_;
        else while (digit(pos_)) ++pos_;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digit(pos_)) return Fail("invalid number");
            while (digit(pos_)) ++pos_;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!digit(pos_)) return Fail("invalid number");
            while (digit(pos_)) ++pos_;
        }
        if (pos_ - start > 64) return Fail("number too long");
        char buffer[72];
        std::memcpy(buffer, s_.data() + start, pos_ - start);
        buffer[pos_ - start] = 0;
        const double n = std::strtod(buffer, nullptr);
        if (!std::isfinite(n)) return Fail("number out of range");
        out = Value(n);
        return true;
    }

    std::string_view s_;
    ParseLimits limits_;
    size_t pos_ = 0;
    std::string error_;
};

bool Parse(std::string_view utf8, Value& out, std::string* error, const ParseLimits& limits) {
    Value result;
    if (!Parser(utf8, limits).Run(result, error)) return false;
    out = std::move(result);
    return true;
}

// ---- serialization ----

namespace {
void WriteString(const std::string& s, std::string& out) {
    static const char kHex[] = "0123456789abcdef";
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) { out += "\\u00"; out += kHex[c >> 4]; out += kHex[c & 15]; }
            else out += ch;
        }
    }
    out += '"';
}

void WriteNumber(double n, std::string& out) {
    if (!std::isfinite(n)) { out += "null"; return; }
    char buffer[40];
    if (std::floor(n) == n && std::fabs(n) <= 9007199254740992.0)
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(n));
    else
        std::snprintf(buffer, sizeof(buffer), "%.17g", n);
    out += buffer;
}
} // namespace

void SerializeTo(const Value& v, std::string& out) {
    switch (v.type()) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Type::Number: WriteNumber(v.as_number(), out); break;
    case Type::String: WriteString(v.as_string(), out); break;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            SerializeTo(v.at(i), out);
        }
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ',';
            WriteString(v.keys()[i], out);
            out += ':';
            SerializeTo(v.at(i), out);
        }
        out += '}';
        break;
    }
}

std::string Serialize(const Value& value) {
    std::string out;
    SerializeTo(value, out);
    return out;
}

std::wstring Wide(std::string_view utf8) {
    if (utf8.empty() || utf8.size() > 0x7FFFFFFF) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string Utf8(std::wstring_view wide) {
    if (wide.empty() || wide.size() > 0x7FFFFFFF) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

} // namespace pulse::json
