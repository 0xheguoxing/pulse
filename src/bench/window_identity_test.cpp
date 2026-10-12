// window_identity_test.cpp - window names and colors (multi-window M7c, plan
// section 8.3, app/window_identity.h). [PASS]/[FAIL] per check.
#include "../app/window_identity.h"

#include <cstdio>

using namespace pulse::app;

namespace {
int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}
}  // namespace

int main() {
    // Automatic name: the folder the panes have in common.
    Check(AutoNameFolder({L"C:\\Work\\Pulse"}) == L"C:\\Work\\Pulse", "name: one pane names the window after its folder");
    Check(AutoNameFolder({L"C:\\Work\\Pulse\\src", L"C:\\Work\\Pulse\\docs"}) == L"C:\\Work\\Pulse",
          "name: two panes name it after their common folder");
    Check(AutoNameFolder({L"C:\\Work\\Pulse", L"c:\\work\\pulse\\SRC\\"}) == L"C:\\Work\\Pulse",
          "name: case and a trailing backslash do not matter");
    Check(AutoNameFolder({L"C:\\Work\\Pulse", L"C:\\Work\\Pulsar"}) == L"C:\\Work",
          "name: a folder is not inside one whose name merely starts the same");
    Check(AutoNameFolder({L"C:\\", L"C:\\"}) == L"C:\\", "name: panes all at a drive root are named after the drive");
    Check(AutoNameFolder({L"D:\\Photos/2026", L"D:\\Photos\\2025"}) == L"D:\\Photos", "name: forward slashes count as separators");

    // Mixed locations: the deepest non-root folder holding the most panes.
    Check(AutoNameFolder({L"C:\\Work\\Pulse\\src", L"C:\\Work\\Pulse\\docs", L"D:\\Downloads"}) == L"C:\\Work\\Pulse",
          "name: one stray tab on another drive does not rename the window");
    Check(AutoNameFolder({L"C:\\A\\x", L"C:\\B\\y", L"C:\\B\\z", L"C:\\A\\w", L"C:\\A\\v"}) == L"C:\\A",
          "name: the folder holding the most panes wins");
    Check(AutoNameFolder({L"C:\\A\\deep\\one", L"C:\\A\\deep\\two", L"C:\\A\\other"}) == L"C:\\A",
          "name: the common folder wins over a deeper folder holding only some panes");
    Check(AutoNameFolder({L"C:\\A\\deep\\one", L"C:\\A\\deep\\two", L"D:\\B\\other"}) == L"C:\\A\\deep",
          "name: among equals the deepest folder wins");
    Check(AutoNameFolder({L"C:\\Users", L"D:\\Games"}).empty(), "name: two lone folders on two drives fall back to the active tab");
    Check(AutoNameFolder({L"C:\\Users", L"C:\\Games"}).empty(), "name: a shared drive root alone does not name the window");

    // Pulse places take no part.
    Check(AutoNameFolder({L"pulse:settings:general", L"C:\\Work"}) == L"C:\\Work", "name: settings tabs are ignored");
    Check(AutoNameFolder({L"", L"pulse:recent"}).empty(), "name: only places falls back to the active tab");
    Check(AutoNameFolder({}).empty(), "name: no panes falls back to the active tab");

    // Network shares: the share is the root.
    Check(AutoNameFolder({L"\\\\nas\\media\\films", L"\\\\nas\\media\\music"}).empty(),
          "name: a share root alone does not name the window, like a drive root");
    Check(AutoNameFolder({L"\\\\nas\\media", L"\\\\NAS\\media\\"}) == L"\\\\nas\\media",
          "name: panes all at a share root are named after the share");
    Check(AutoNameFolder({L"\\\\nas\\media\\films\\a", L"\\\\nas\\media\\films\\b", L"\\\\nas\\backup"}) ==
              L"\\\\nas\\media\\films",
          "name: the share root itself is never picked for mixed panes");

    // Colors: the least used, lowest first.
    Check(PickWindowColor({}) == 0, "color: the first window gets the first color");
    Check(PickWindowColor({0}) == 1, "color: the second window gets the next one");
    Check(PickWindowColor({0, 1, 2, 3, 4, 5}) == 0, "color: after all six the first comes again");
    Check(PickWindowColor({0, 1, 2, 3, 4, 5, 0}) == 1, "color: then the least used one");
    Check(PickWindowColor({1, 3}) == 0, "color: a color freed by a closed window is used again");
    Check(PickWindowColor({-1, 0, 99}) == 1, "color: unset and invalid colors are ignored");

    // Typed names.
    Check(NormalizeWindowName(L"  Project X \t") == L"Project X", "rename: spaces around the name go");
    Check(NormalizeWindowName(L"\u3000\u8D44\u6599\u3000") == L"\u8D44\u6599", "rename: ideographic spaces go too");
    Check(NormalizeWindowName(L"   ").empty(), "rename: a blank name means back to automatic");
    Check(NormalizeWindowName(L"a\r\nb\x7F") == L"ab", "rename: control characters are dropped");
    Check(NormalizeWindowName(std::wstring(80, L'x')).size() == kWindowNameMaxLength, "rename: long names are cut");
    {
        std::wstring name(kWindowNameMaxLength - 1, L'x');
        name += L"\xD83D\xDE00";  // an emoji straddling the limit
        Check(NormalizeWindowName(name) == std::wstring(kWindowNameMaxLength - 1, L'x'),
              "rename: a cut never leaves half a character");
    }

    // Visibility and identity values.
    Check(!ShowsWindowIdentity(1) && ShowsWindowIdentity(2), "show: only with two or more windows");
    Check(ValidWindowColor(0) && ValidWindowColor(5) && !ValidWindowColor(6) && !ValidWindowColor(-1),
          "color: six valid indices");
    Check(WindowIdentity{} == WindowIdentity{L"", -1}, "identity: default is automatic and uncolored");

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
