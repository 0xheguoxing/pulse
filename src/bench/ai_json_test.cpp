// ai_json_test - json_value: the strict parser AI clients' input goes
// through (RFC 8259, UTF-8, depth / size limits, duplicate keys), the
// serializer, and the UTF-8 / UTF-16 helpers.
#include "../common/json_value.h"
#include <cstdio>
#include <string>

using namespace pulse;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_passed : ++g_failed;
}
bool Parses(const std::string& text) {
    json::Value v;
    return json::Parse(text, v);
}
std::string RoundTrip(const std::string& text) {
    json::Value v;
    return json::Parse(text, v) ? json::Serialize(v) : std::string("<invalid>");
}
} // namespace

int main() {
    // --- valid documents.
    json::Value v;
    Check(json::Parse(R"( {"a":1,"b":[true,false,null],"c":{"d":"e"}} )", v) && v.is_object() && v.size() == 3,
          "object with nested array and object");
    Check(v.find("a") && v.find("a")->as_number() == 1 && v.find("b")->size() == 3 && v.find("b")->at(0).as_bool() &&
          v.find("c")->get_string("d") == "e", "members are reachable");
    Check(RoundTrip(R"({"a":1,"b":[true,false,null],"c":{"d":"e"}})") == R"({"a":1,"b":[true,false,null],"c":{"d":"e"}})",
          "serialize keeps member order and compact form");
    Check(Parses("0") && Parses("-0") && Parses("\"x\"") && Parses("null") && Parses("[]") && Parses("{}"),
          "scalars and empty containers at top level");
    Check(RoundTrip("[1e3,-2.5,0.125,9007199254740992]") == "[1000,-2.5,0.125,9007199254740992]", "numbers");
    int64_t n = 0;
    json::Value number;
    Check(json::Parse("42", number) && number.get_int(n, 1, 100) && n == 42, "get_int in range");
    Check(json::Parse("4.5", number) && !number.get_int(n), "get_int refuses fractions");
    Check(json::Parse("101", number) && !number.get_int(n, 1, 100), "get_int refuses out of range");

    // --- strings and Unicode.
    Check(json::Parse(R"("\u4e2d\u6587 \ud83d\ude00 \"q\" \\ \/ \n")", v) && v.as_string() == "\xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80 \"q\" \\ / \n",
          "escapes and a surrogate pair decode to UTF-8");
    Check(json::Parse("\"\xE4\xB8\xAD\"", v) && v.as_string() == "\xE4\xB8\xAD", "raw UTF-8 is kept");
    Check(!Parses(R"("\ud83d")") && !Parses(R"("\ude00")") && !Parses(R"("\ud83d\u0041")"), "unpaired surrogates are refused");
    Check(!Parses("\"\xC0\xAF\"") && !Parses("\"\xED\xA0\x80\"") && !Parses("\"\xFF\"") && !Parses("\"\xE4\xB8\""),
          "overlong, surrogate, invalid and truncated UTF-8 are refused");
    Check(!Parses("\"a\nb\"") && !Parses(std::string("\"a\0b\"", 5)), "control characters in strings are refused");
    Check(json::Serialize(json::Value(std::string("a\x01" "b\"\\\n"))) == "\"a\\u0001b\\\"\\\\\\n\"", "serializer escapes");

    // --- refused syntax.
    const char* invalid[] = {"", " ", "{", "[1,]", "{\"a\":1,}", "{'a':1}", "{a:1}", "[01]", "[1.]", "[.5]", "[+1]",
                             "NaN", "[Infinity]", "tru", "nul", "[1 2]", "{\"a\" 1}", "// x\n1", "1 2", "[1e]", "\"abc",
                             "[1e999]"};
    bool all_refused = true;
    for (const char* text : invalid) {
        if (Parses(text)) { std::printf("  accepted: %s\n", text); all_refused = false; }
    }
    Check(all_refused, "malformed JSON is refused");
    std::string error;
    Check(!json::Parse("{\"a\":1,\"a\":2}", v, &error) && error.find("duplicate") != std::string::npos,
          "duplicate keys are refused with a reason");

    // --- limits.
    std::string deep(64, '['), deeper(65, '[');
    deep += std::string(64, ']');
    deeper += std::string(65, ']');
    Check(Parses(deep), "64 levels of nesting are accepted");
    Check(!Parses(deeper), "65 levels are refused");
    json::ParseLimits small;
    small.max_bytes = 16;
    Check(!json::Parse("[1,2,3,4,5,6,7,8,9]", v, nullptr, small) && json::Parse("[1,2]", v, nullptr, small),
          "input size limit");
    json::Value kept = json::Value("unchanged");
    Check(!json::Parse("[1,", kept) && kept.as_string() == "unchanged", "a failed parse leaves the output alone");

    // --- building.
    json::Value built;
    built.set("x", 1);
    built.set("y", "two");
    built.set("x", 3);
    json::Value list;
    list.push(true);
    list.push(nullptr);
    built.set("list", std::move(list));
    Check(json::Serialize(built) == R"({"x":3,"y":"two","list":[true,null]})", "set replaces, push appends");
    Check(json::Serialize(json::Value(1.0 / 3.0)) == "0.33333333333333331" && json::Serialize(json::Value(-7)) == "-7",
          "fractions round-trip, integers print plainly");

    // --- UTF-8 / UTF-16.
    const std::wstring wide = L"C:\\\x6587\x4EF6\\\xD83D\xDE00.txt";
    Check(json::Wide(json::Utf8(wide)) == wide, "Utf8 / Wide round-trip Chinese and emoji");
    Check(json::Wide("\xFF\xFE").empty() && json::Utf8(std::wstring(1, static_cast<wchar_t>(0xD800))).empty(),
          "invalid input converts to empty");

    std::printf("ai_json_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
