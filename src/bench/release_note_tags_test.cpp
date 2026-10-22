// Release-note item tags: which leading brackets become badges (pure).
#include "../ui/release_note_tags.h"
#include <cstdio>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    const auto take = [](std::wstring text, ReleaseTag want, const wchar_t* rest, const wchar_t* label) {
        std::wstring got_label;
        const ReleaseTag tag = TakeReleaseTag(text, got_label);
        return tag == want && text == rest && got_label == label;
    };
    check(take(L"\u65B0\u529F\u80FD \u7F51\u7EDC", ReleaseTag::None, L"\u65B0\u529F\u80FD \u7F51\u7EDC", L""),
          "no bracket: untouched");
    check(take(L"[\u65B0\u529F\u80FD] \u7F51\u7EDC\u4F4D\u7F6E", ReleaseTag::New, L"\u7F51\u7EDC\u4F4D\u7F6E",
               L"\u65B0\u529F\u80FD"), "[新功能] becomes New and the text loses it");
    check(take(L"[\u65B0\u8BBE\u7F6E]\u5355\u51FB\u6253\u5F00", ReleaseTag::Setting, L"\u5355\u51FB\u6253\u5F00",
               L"\u65B0\u8BBE\u7F6E"), "[新设置] without a space");
    check(take(L"[\u6539\u8FDB]\u3000\u66F4\u5FEB", ReleaseTag::Improved, L"\u66F4\u5FEB", L"\u6539\u8FDB"),
          "[改进] followed by an ideographic space");
    check(take(L"[\u4FEE\u590D] \u767D\u5C4F", ReleaseTag::Fixed, L"\u767D\u5C4F", L"\u4FEE\u590D"), "[修复]");
    check(take(L"[New] Network locations", ReleaseTag::New, L"Network locations", L"New") &&
              take(L"[Setting] Single-click", ReleaseTag::Setting, L"Single-click", L"Setting") &&
              take(L"[Improved] Faster", ReleaseTag::Improved, L"Faster", L"Improved") &&
              take(L"[Fixed] Blank window", ReleaseTag::Fixed, L"Blank window", L"Fixed"),
          "English tags");
    check(take(L"[New](https://example.com) page", ReleaseTag::None, L"[New](https://example.com) page", L""),
          "a link is not a tag");
    check(take(L"[Beta] thing", ReleaseTag::None, L"[Beta] thing", L"") &&
              take(L"[new] lower case", ReleaseTag::None, L"[new] lower case", L""),
          "unknown or differently cased brackets stay text");
    check(take(L"[\u65B0\u529F\u80FD]", ReleaseTag::New, L"", L"\u65B0\u529F\u80FD"), "a bare tag leaves empty text");
    check(take(L"[] x", ReleaseTag::None, L"[] x", L"") && take(L"[", ReleaseTag::None, L"[", L""),
          "empty and unclosed brackets");
    return failures ? 1 : 0;
}
