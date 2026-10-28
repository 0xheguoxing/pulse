// json_value.h - A small JSON DOM for the AI pack: the tool broker protocol
// (ai_tool_protocol.h), ai.json, the activity log and, in Pulse.AI.exe, MCP's
// JSON-RPC. Shared by pulse.exe and the pack.
//
// The parser is strict RFC 8259 on UTF-8 because its input comes from AI
// clients: no comments, trailing commas, NaN or duplicate keys; strings must
// be valid UTF-8 without unpaired surrogates; nesting depth and input size are
// capped. json_utils.h stays for the app's own small config files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int n) : type_(Type::Number), number_(n) {}
    Value(int64_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(uint64_t n) : type_(Type::Number), number_(static_cast<double>(n)) {}
    Value(double n) : type_(Type::Number), number_(n) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(std::string_view s) : type_(Type::String), string_(s) {}
    Value(const char* s) : type_(Type::String), string_(s ? s : "") {}
    static Value Array() { Value v; v.type_ = Type::Array; return v; }
    static Value Object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const noexcept { return is_bool() ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const noexcept { return is_number() ? number_ : fallback; }
    // An integral number in [minimum, maximum] (both within +-2^53), else false.
    bool get_int(int64_t& out, int64_t minimum = -(int64_t{1} << 53), int64_t maximum = int64_t{1} << 53) const noexcept;
    // The string, or an empty one for any other type.
    const std::string& as_string() const noexcept;

    // Arrays and objects: elements (objects: the values, in key order of insertion).
    size_t size() const noexcept { return items_.size(); }
    const Value& at(size_t i) const noexcept;
    const std::vector<Value>& items() const noexcept { return items_; }
    // Objects: keys parallel to items().
    const std::vector<std::string>& keys() const noexcept { return keys_; }
    const Value* find(std::string_view key) const noexcept;
    // A member's string / number / bool, with a fallback when absent or of another type.
    std::string get_string(std::string_view key, std::string_view fallback = {}) const;
    bool get_bool(std::string_view key, bool fallback = false) const noexcept;

    // Building. push() turns null into an array, set() turns null into an object.
    Value& push(Value v);
    Value& set(std::string_view key, Value v);  // replaces an existing member

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<std::string> keys_;
    friend class Parser;
};

struct ParseLimits {
    size_t max_bytes = 4u << 20;   // the broker's frame limit
    size_t max_depth = 64;
};

// Parses one JSON text (surrounding whitespace allowed). On failure `error`
// gets a short ASCII reason with the byte offset.
bool Parse(std::string_view utf8, Value& out, std::string* error = nullptr, const ParseLimits& limits = {});

// Compact serialization; non-ASCII stays UTF-8, control characters are
// escaped. Non-finite numbers become null.
std::string Serialize(const Value& value);
void SerializeTo(const Value& value, std::string& out);

// UTF-8 <-> UTF-16 for the Win32 side. Invalid input converts to "".
std::wstring Wide(std::string_view utf8);
std::string Utf8(std::wstring_view wide);

} // namespace pulse::json
