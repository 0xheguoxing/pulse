// Quick search apps (#58-1): ranking, noise filtering and the AppsFolder path.
// Fixtures are made-up names only. `--live` additionally enumerates the
// Start menu once (read-only) and prints just the count.
#include "../app/app_catalog.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using pulse::app::AppEntry;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

AppEntry App(const wchar_t* name, const wchar_t* id) {
    return {name, std::wstring(pulse::app::kAppsFolderPrefix) + id};
}

std::vector<std::wstring> Names(const std::vector<AppEntry>& apps, std::wstring_view query,
                                bool pinyin = true, size_t limit = 8) {
    std::vector<std::wstring> out;
    for (const size_t i : pulse::app::MatchApps(apps, query, pinyin, limit)) out.push_back(apps[i].name);
    return out;
}

bool First(const std::vector<AppEntry>& apps, std::wstring_view query, const wchar_t* name,
           bool pinyin = true) {
    const auto names = Names(apps, query, pinyin);
    return !names.empty() && names.front() == name;
}

bool Has(const std::vector<std::wstring>& names, const wchar_t* name) {
    for (const auto& item : names)
        if (item == name) return true;
    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::vector<AppEntry> apps = {
        App(L"Visual Studio Code", L"Fake.Code"),
        App(L"Visual Studio 2022", L"Fake.VS"),
        App(L"Code Runner", L"Fake.Runner"),
        App(L"Notepad", L"Fake.Notepad"),
        App(L"Notepad++", L"Fake.NotepadPlusPlus"),
        App(L"PowerToys", L"Fake.PowerToys"),
        App(L"Calculator", L"Fake.Calc!App"),
        App(L"微信", L"Fake.WeChat"),
        App(L"网易云音乐", L"Fake.Music"),
        App(L"Microsoft Teams", L"Fake.Teams"),
    };

    Check(First(apps, L"notepad", L"Notepad"), "whole name ranks first");
    Check(First(apps, L"NOTE", L"Notepad"), "prefix match folds case; shorter name wins the tie");
    Check(First(apps, L"code", L"Code Runner"), "name prefix beats word prefix");
    {
        const auto names = Names(apps, L"code");
        Check(names.size() == 2 && names[1] == L"Visual Studio Code", "word prefix matches inside the name");
    }
    Check(First(apps, L"vsc", L"Visual Studio Code"), "word initials (vsc)");
    Check(First(apps, L"pt", L"PowerToys"), "capitals inside a word count as initials (pt)");
    Check(First(apps, L"calc", L"Calculator"), "short prefix");
    Check(First(apps, L"weixin", L"微信"), "full pinyin");
    Check(First(apps, L"wx", L"微信"), "pinyin initials");
    Check(First(apps, L"yinyue", L"网易云音乐"), "pinyin in the middle of a name");
    Check(Names(apps, L"weixin", false).empty(), "pinyin off: no pinyin matches");
    Check(First(apps, L"lator", L"Calculator"), "substring as the last resort");
    Check(First(apps, L"studio code", L"Visual Studio Code"), "multi-word query, contiguous");
    Check(First(apps, L"code visual", L"Visual Studio Code"), "multi-word query, any order");
    Check(Names(apps, L"code zzz").empty(), "every word must match");
    Check(Names(apps, L"   ").empty(), "blank query matches nothing");
    Check(Names(apps, L"o", true, 3).size() == 3, "limit caps the result");
    {
        const auto names = Names(apps, L"visual studio");
        Check(names.size() == 2 && names[0] == L"Visual Studio 2022" && names[1] == L"Visual Studio Code",
              "equal scores: shorter, then alphabetical");
    }
    Check(!Has(Names(apps, L"team"), L"Notepad"), "unrelated names stay out");

    Check(pulse::app::IsAppNoise(App(L"Uninstall Foo", L"C:\\Fake\\unins000.exe")), "uninstaller is noise");
    Check(pulse::app::IsAppNoise(App(L"卸载 示例", L"C:\\Fake\\u.exe")), "卸载 is noise");
    Check(pulse::app::IsAppNoise(App(L"Foo Website", L"https://example.invalid/")), "website is noise");
    Check(pulse::app::IsAppNoise(App(L"Foo Readme", L"C:\\Fake\\readme.txt")), "readme is noise");
    Check(pulse::app::IsAppNoise(App(L"Foo Help", L"C:\\Fake\\foo.chm")), "help file is noise");
    Check(!pulse::app::IsAppNoise(App(L"Calculator", L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App")),
          "packaged app id is not noise");
    Check(!pulse::app::IsAppNoise(App(L"Foo", L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Foo\\foo.exe")),
          "desktop app is not noise");

    Check(pulse::app::IsAppPath(L"shell:AppsFolder\\Fake.Code"), "AppsFolder path recognized");
    Check(pulse::app::IsAppPath(L"SHELL:appsfolder\\Fake.Code"), "AppsFolder path ignores case");
    Check(!pulse::app::IsAppPath(L"shell:AppsFolder\\"), "bare AppsFolder is not an app");
    Check(!pulse::app::IsAppPath(L"C:\\Fake\\shell:AppsFolder"), "file path is not an app");

    bool live = false;
    for (int i = 1; i < argc; ++i) live = live || std::wcscmp(argv[i], L"--live") == 0;
    if (live) {
        const auto loaded = pulse::app::LoadApps();
        bool shaped = !loaded.empty();
        for (const auto& app : loaded)
            shaped = shaped && pulse::app::IsAppPath(app.path) && !app.name.empty() && !pulse::app::IsAppNoise(app);
        std::printf("live: %zu apps\n", loaded.size());
        Check(shaped, "live Start menu list is non-empty, prefixed and without noise");
    }

    if (failures) std::printf("FAILED: %d\n", failures);
    else std::printf("ALL PASS\n");
    return failures ? 1 : 0;
}
