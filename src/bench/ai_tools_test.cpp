// ai_tools_test.cpp - pulse_ai_tools_test: the AI tools' file system side
// (ai_tools_fs.h) on fixtures it creates under bench_data/aitools-<pid> and
// removes again. Reads two system files only for their version resource.
#include "../app/ai_tools_fs.h"
#include "../app/zip_compress.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace pulse;
using namespace pulse::app::ai;

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? g_pass : g_fail)++;
}

std::wstring FixtureRoot() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::filesystem::path build = std::filesystem::path(exe).parent_path();
    return (build.parent_path() / L"bench_data" / (L"aitools-" + std::to_wstring(GetCurrentProcessId()))).wstring();
}

bool Write(const std::wstring& path, const std::string& bytes, DWORD attributes = FILE_ATTRIBUTE_NORMAL) {
    HANDLE f = CreateFileW(Win32Path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, attributes, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
    return ok && written == bytes.size();
}

// A 3 x 2 24-bit bitmap.
std::string TinyBmp() {
    const int w = 3, h = 2, row = 12;   // 9 bytes padded to 12
    std::string b(54 + row * h, '\0');
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = static_cast<char>(v >> (8 * i)); };
    b[0] = 'B'; b[1] = 'M';
    put32(2, static_cast<uint32_t>(b.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    b[26] = 1;
    b[28] = 24;
    put32(34, row * h);
    return b;
}

json::Value Obj(std::initializer_list<std::pair<const char*, json::Value>> members) {
    json::Value v = json::Value::Object();
    for (const auto& [k, m] : members) v.set(k, m);
    return v;
}

json::Value Paths(std::initializer_list<std::wstring> paths) {
    json::Value v = json::Value::Array();
    for (const auto& p : paths) v.push(json::Utf8(p));
    return v;
}

std::string U8(const std::wstring& s) { return json::Utf8(s); }

bool Fails(const ToolOutcome& o, const char* code) { return !o.ok && o.error.code == code; }

std::wstring Checked(const std::wstring& raw, bool* ok = nullptr) {
    std::wstring out;
    ToolOutcome error;
    const bool accepted = CheckPathArg(json::Value(json::Utf8(raw)), "path", out, error);
    if (ok) *ok = accepted;
    return accepted ? out : L"<refused:" + json::Wide(error.error.code) + L">";
}

std::vector<std::string> Names(const ToolOutcome& o) {
    std::vector<std::string> names;
    if (const json::Value* items = o.data.find("items"))
        for (const json::Value& item : items->items()) names.push_back(item.get_string("name"));
    return names;
}

size_t CountEntries(const std::wstring& folder) {
    size_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec))
        ++n;
    return n;
}

} // namespace

int wmain() {
    const std::atomic<bool> running{false};
    const std::atomic<bool> stop{true};
    const std::wstring root = FixtureRoot();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    Check(!ec && std::filesystem::is_directory(root), "fixture root created under bench_data");

    // --- paths -------------------------------------------------------------
    Check(Checked(root + L"\\x\\..\\y") == root + L"\\y", "path: .. is resolved");
    {
        std::wstring slashed = root + L"/y/";
        for (auto& c : slashed) if (c == L'\\') c = L'/';
        Check(Checked(slashed) == root + L"\\y", "path: forward slashes and a trailing separator");
    }
    Check(Checked(L"C:\\") == L"C:\\", "path: a drive root keeps its separator");
    Check(Checked(L"\\\\?\\C:\\Temp\\a") == L"C:\\Temp\\a", "path: \\\\?\\C:\\ form accepted as the plain path");
    Check(Checked(L"\\\\?\\UNC\\srv\\share\\a") == L"\\\\srv\\share\\a", "path: \\\\?\\UNC\\ form accepted as UNC");
    Check(Checked(L"\\\\srv\\share\\a\\") == L"\\\\srv\\share\\a", "path: UNC accepted");
    bool ok = true;
    for (const wchar_t* bad : {L"relative\\x", L"C:foo", L"\\rooted", L"\\\\.\\PhysicalDrive0", L"\\\\?\\GLOBALROOT\\x",
                               L"\\\\?\\Volume{0}\\x", L"C:\\a*b", L"C:\\a?b", L"C:\\file.txt:stream", L"\\\\server",
                               L"\\\\server\\", L"C:\\a\"b", L"C:\\a|b"}) {
        bool accepted = false;
        Checked(bad, &accepted);
        if (accepted) {
            ok = false;
            std::printf("  accepted: %s\n", U8(bad).c_str());
        }
    }
    Check(ok, "path: relative, drive-relative, device, GLOBALROOT, wildcard, stream and bare-server forms refused");
    {
        std::wstring out;
        ToolOutcome error;
        Check(!CheckPathArg(json::Value(5), "path", out, error) && error.error.code == "invalid_args" &&
                  !CheckPathArg(json::Value(""), "path", out, error),
              "path: non-strings and empty strings refused");
    }
    Check(ParentFolder(L"C:\\x") == L"C:\\" && ParentFolder(L"C:\\") .empty() && ParentFolder(L"C:\\a\\b") == L"C:\\a" &&
              ParentFolder(L"\\\\s\\sh\\x") == L"\\\\s\\sh" && ParentFolder(L"\\\\s\\sh").empty(),
          "ParentFolder: drives, folders and shares");
    {
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        const std::wstring win = windows;
        Check(IsSystemFolder(win) && IsSystemFolder(win + L"\\System32") && !IsSystemFolder(win + L"2") &&
                  IsSystemFolder(L"D:\\$Recycle.Bin\\S-1") && IsSystemFolder(L"C:\\System Volume Information") &&
                  !IsSystemFolder(root),
              "IsSystemFolder: Windows and below, Recycle Bin, SVI; not a sibling name or the fixture");
    }
    {
        SYSTEMTIME st{2026, 10, 3, 21, 3, 4, 5, 0};
        FILETIME ft{};
        SystemTimeToFileTime(&st, &ft);
        Check(IsoTime(ft) == "2026-10-21T03:04:05Z" && IsoTime(FILETIME{}).empty(), "IsoTime: UTC, empty for zero");
    }

    // --- list_folder ---------------------------------------------------------
    const std::wstring list = root + L"\\list";
    std::filesystem::create_directories(list + L"\\b dir");
    std::filesystem::create_directories(list + L"\\A dir");
    bool made = Write(list + L"\\file10.txt", "10") && Write(list + L"\\file2.txt", "two!") &&
                Write(list + L"\\File1.txt", "1") && Write(list + L"\\h.txt", "h", FILE_ATTRIBUTE_HIDDEN) &&
                Write(list + L"\\s.txt", "s", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    Check(made, "list: fixture written");
    ToolOutcome listed = ListFolder(Obj({{"path", U8(list)}}), running);
    Check(listed.ok && Names(listed) == std::vector<std::string>{"A dir", "b dir", "File1.txt", "file2.txt",
                                                                 "file10.txt", "h.txt"},
          "list: folders first, Explorer name order (numbers by value), protected system files left out");
    Check(listed.ok && listed.data.find("total")->as_number() == 6 && !listed.data.find("next_cursor") &&
              listed.data.get_string("path") == U8(list),
          "list: total and path, no cursor on the last page");
    {
        const json::Value& items = *listed.data.find("items");
        bool fields = items.size() == 6 && items.at(0).get_string("type") == "folder" && !items.at(0).find("size") &&
                      items.at(3).get_string("type") == "file" && items.at(3).find("size")->as_number() == 4 &&
                      items.at(3).get_string("modified").size() == 20 && items.at(5).get_bool("hidden") &&
                      !items.at(3).find("hidden");
        Check(fields, "list: type, size for files, ISO modified time, hidden flag");
    }
    ToolOutcome page = ListFolder(Obj({{"path", U8(list)}, {"limit", 2}}), running);
    Check(page.ok && Names(page) == std::vector<std::string>{"A dir", "b dir"} && page.data.get_string("next_cursor") == "2",
          "list: limit pages and gives next_cursor");
    page = ListFolder(Obj({{"path", U8(list)}, {"limit", 2}, {"cursor", "4"}}), running);
    Check(page.ok && Names(page) == std::vector<std::string>{"file10.txt", "h.txt"} && !page.data.find("next_cursor"),
          "list: the cursor continues where the page ended");
    Check(Fails(ListFolder(Obj({{"path", U8(list)}, {"cursor", "7"}}), running), "invalid_args") &&
              Fails(ListFolder(Obj({{"path", U8(list)}, {"cursor", "x1"}}), running), "invalid_args") &&
              Fails(ListFolder(Obj({{"path", U8(list)}, {"limit", 501}}), running), "invalid_args"),
          "list: a cursor past the end, a malformed cursor and a too large limit are refused");
    Check(Fails(ListFolder(Obj({{"path", U8(list + L"\\file2.txt")}}), running), "invalid_args") &&
              Fails(ListFolder(Obj({{"path", U8(list + L"\\nope")}}), running), "not_found") &&
              Fails(ListFolder(Obj({{"path", "list"}}), running), "invalid_args") &&
              Fails(ListFolder(json::Value::Object(), running), "invalid_args"),
          "list: a file, a missing folder, a relative or no path");
    Check(Fails(ListFolder(Obj({{"path", U8(list)}}), stop), "cancelled"), "list: a cancelled call reports cancelled");
    {
        std::wstring deep = root + L"\\long";
        while (deep.size() < 300) deep += L"\\abcdefghijklmnopqrstuvwxyz0123456789";
        std::filesystem::create_directories(L"\\\\?\\" + deep, ec);
        const bool wrote = !ec && Write(deep + L"\\deep.txt", "deep");
        ToolOutcome long_list = ListFolder(Obj({{"path", U8(deep)}}), running);
        Check(wrote && long_list.ok && Names(long_list) == std::vector<std::string>{"deep.txt"},
              "list: a folder path longer than MAX_PATH");
    }

    // --- get_properties ------------------------------------------------------
    const std::wstring bmp = list + L"\\tiny.bmp";
    Write(bmp, TinyBmp());
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring kernel32 = std::wstring(system) + L"\\kernel32.dll";
    ToolOutcome props = GetProperties(
        Obj({{"paths", Paths({list + L"\\file2.txt", list + L"\\A dir", list + L"\\nope.txt", L"relative.txt", bmp,
                              kernel32, list})}}),
        running);
    Check(props.ok && props.data.find("items")->size() == 7, "props: one entry per path, in order");
    if (props.ok && props.data.find("items")->size() == 7) {
        const json::Value& items = *props.data.find("items");
        const json::Value& file = items.at(0);
        Check(file.get_string("type") == "file" && file.find("size")->as_number() == 4 &&
                  file.get_string("name") == "file2.txt" && file.get_string("modified").size() == 20 &&
                  file.get_string("created").size() == 20 && !file.find("error"),
              "props: a file's size, name and times");
        Check(items.at(1).get_string("type") == "folder" && items.at(1).find("items")->as_number() == 0 &&
                  items.at(6).find("items")->as_number() == 8,
              "props: a folder's direct item count");
        Check(items.at(2).get_string("error").rfind("not_found:", 0) == 0 &&
                  items.at(3).get_string("error").rfind("invalid_args:", 0) == 0 &&
                  items.at(3).get_string("path") == "relative.txt",
              "props: a missing and a refused path fail alone, as entries with error");
        const json::Value* image = items.at(4).find("image");
        Check(image && image->find("width")->as_number() == 3 && image->find("height")->as_number() == 2,
              "props: image dimensions from the shell's property system");
        const json::Value* version = items.at(5).find("version");
        Check(version && version->get_string("file").find('.') != std::string::npos &&
                  !version->get_string("company").empty(),
              "props: a program's version resource");
        bool hidden_attr = false;
        ToolOutcome h = GetProperties(Obj({{"paths", Paths({list + L"\\h.txt"})}}), running);
        if (const json::Value* attrs = h.ok ? h.data.find("items")->at(0).find("attributes") : nullptr)
            for (const json::Value& a : attrs->items()) hidden_attr |= a.as_string() == "hidden";
        Check(hidden_attr, "props: attributes are listed");
    }
    {
        json::Value many = json::Value::Array();
        for (int i = 0; i < 51; ++i) many.push(U8(list));
        Check(Fails(GetProperties(Obj({{"paths", many}}), running), "invalid_args") &&
                  Fails(GetProperties(Obj({{"paths", json::Value::Array()}}), running), "invalid_args") &&
                  Fails(GetProperties(Obj({{"paths", Paths({list})}}), stop), "cancelled"),
              "props: more than 50 or no paths refused; cancel honoured");
    }

    // --- search planning -----------------------------------------------------
    {
        SearchPlan plan;
        ToolOutcome error;
        Check(PlanSearch(Obj({{"query", "report"}}), plan, error) && plan.needle == L"report" && plan.limit == 50 &&
                  !plan.folders_only && plan.folder.empty(),
              "search: a plain query, default limit");
        Check(PlanSearch(Obj({{"query", "q"}, {"type", "image"}, {"modified_after", "2026-01-02"}, {"limit", 7},
                              {"folder", U8(list + L"\\")}}), plan, error) &&
                  plan.needle == L"q type:image dm:2026-01-02..2999-12-31" && plan.limit == 7 && plan.folder == list,
              "search: type, date and folder become Pulse's syntax and the path prefix");
        Check(PlanSearch(Obj({{"query", "q"}, {"type", "folder"}}), plan, error) && plan.folders_only &&
                  plan.needle == L"q" &&
                  PlanSearch(Obj({{"query", "q"}, {"type", "file"}}), plan, error) && plan.needle == L"q file:",
              "search: folder uses folders_only, file the file: filter");
        bool refused = true;
        for (const json::Value& bad : {Obj({{"query", ""}}), Obj({{"query", "a\nb"}}), json::Value::Object(),
                                       Obj({{"query", "q"}, {"type", "exe"}}),
                                       Obj({{"query", "q"}, {"modified_after", "2026-13-01"}}),
                                       Obj({{"query", "q"}, {"modified_after", "yesterday"}}),
                                       Obj({{"query", "q"}, {"limit", 201}}),
                                       Obj({{"query", "q"}, {"folder", "relative"}})})
            refused &= !PlanSearch(bad, plan, error) && error.error.code == "invalid_args";
        Check(refused, "search: empty or multi-line query, unknown type, bad date, big limit, relative folder");
        const json::Value hits = SearchResultJson({{L"C:\\a\\b.txt", L"b.txt", false, 12, 0},
                                                   {L"C:\\a\\d", L"d", true, 0, 0}}, 1);
        Check(hits.find("total")->as_number() == 2 && hits.find("items")->at(0).get_string("path") == "C:\\a\\b.txt" &&
                  hits.find("items")->at(0).find("size")->as_number() == 12 &&
                  !hits.find("items")->at(1).find("size") && hits.find("items")->at(1).get_string("type") == "folder",
              "search: result items; total never below the items returned");
    }

    // --- create_zip ----------------------------------------------------------
    if (!pulse::app::ZipCompressAvailable()) {
        Check(false, "zip: archiveint.dll is available (Windows 10 1803+)");
    } else {
        const std::wstring src = root + L"\\zipsrc";
        std::filesystem::create_directories(src + L"\\sub");
        std::filesystem::create_directories(root + L"\\other");
        Write(src + L"\\a.txt", "aaaa");
        Write(src + L"\\b.txt", "bbbb");
        Write(src + L"\\sub\\c.txt", "cccc");
        Write(root + L"\\other\\d.txt", "dddd");
        const json::Value two = Paths({src + L"\\a.txt", src + L"\\b.txt"});
        ZipPlan plan;
        ToolOutcome error;
        Check(PlanZip(Obj({{"paths", two}}), plan, error) && plan.folder == src &&
                  plan.zip_path == src + L"\\zipsrc.zip",
              "zip: several items of a folder pack into <folder>.zip beside them");
        ToolOutcome zipped = RunZip(plan, running);
        Check(zipped.ok && zipped.changed_files && zipped.data.get_string("path") == U8(src + L"\\zipsrc.zip") &&
                  zipped.data.find("files")->as_number() == 2 && zipped.data.find("size")->as_number() > 0 &&
                  std::filesystem::is_regular_file(src + L"\\zipsrc.zip"),
              "zip: the archive is written; path, size and file count reported");
        Check(PlanZip(Obj({{"paths", two}}), plan, error) && plan.zip_path == src + L"\\zipsrc (2).zip",
              "zip: an existing archive is never replaced");
        Check(PlanZip(Obj({{"paths", Paths({src + L"\\sub"})}, {"name", " report.ZIP "}}), plan, error) &&
                  plan.zip_path == src + L"\\report.zip" && RunZip(plan, running).ok &&
                  PlanZip(Obj({{"paths", Paths({src + L"\\sub"})}, {"name", "report"}}), plan, error) &&
                  plan.zip_path == src + L"\\report (2).zip",
              "zip: a given name gets .zip once, trimmed, and a free number when taken");
        bool names_refused = true;
        for (const char* bad : {"a/b", "a\\b", "CON", "lpt1.zip", "  .", "x:y", "a*"})
            names_refused &= !PlanZip(Obj({{"paths", two}, {"name", bad}}), plan, error) &&
                             error.error.code == "invalid_args";
        Check(names_refused, "zip: names with separators, reserved device names or bad characters refused");
        Check(!PlanZip(Obj({{"paths", Paths({src + L"\\a.txt", root + L"\\other\\d.txt"})}}), plan, error) &&
                  error.error.code == "invalid_args",
              "zip: items from two folders refused");
        Check(!PlanZip(Obj({{"paths", Paths({src + L"\\missing.txt"})}}), plan, error) && error.error.code == "not_found",
              "zip: a missing item -> not_found");
        Check(!PlanZip(Obj({{"paths", Paths({src + L"\\a.txt", src + L"\\A.TXT"})}}), plan, error) &&
                  error.error.code == "invalid_args",
              "zip: the same item twice refused");
        Check(!PlanZip(Obj({{"paths", Paths({L"C:\\"})}}), plan, error) && error.error.code == "invalid_args",
              "zip: a drive root refused");
        Check(PlanZip(Obj({{"paths", two}, {"dest_folder", U8(root + L"\\other")}}), plan, error) &&
                  plan.folder == root + L"\\other" && plan.zip_path == root + L"\\other\\zipsrc.zip",
              "zip: dest_folder places the archive elsewhere, still named after the items' folder");
        Check(PlanZip(Obj({{"paths", Paths({src + L"\\a.txt"})}}), plan, error) && plan.zip_path == src + L"\\a.zip" &&
                  PlanZip(Obj({{"paths", Paths({src + L"\\sub"})}}), plan, error) && plan.zip_path == src + L"\\sub.zip",
              "zip: one item is named after itself, a file without its extension");
        Check(!PlanZip(Obj({{"paths", Paths({src + L"\\sub"})}, {"dest_folder", U8(src + L"\\sub")}}), plan, error) &&
                  error.error.code == "invalid_args",
              "zip: dest_folder inside a folder being packed refused");
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        Check(!PlanZip(Obj({{"paths", two}, {"dest_folder", U8(std::wstring(windows) + L"\\Temp")}}), plan, error) &&
                  error.error.code == "system_folder",
              "zip: a destination under Windows -> system_folder");
        Check(!PlanZip(Obj({{"paths", two}, {"dest_folder", U8(root + L"\\nowhere")}}), plan, error) &&
                  error.error.code == "not_found" &&
                  !PlanZip(Obj({{"paths", two}, {"dest_folder", U8(src + L"\\a.txt")}}), plan, error) &&
                  error.error.code == "invalid_args",
              "zip: a missing destination -> not_found; a file as destination refused");
        Check(ZipTargetFolder(Obj({{"paths", two}})) == src &&
                  ZipTargetFolder(Obj({{"paths", two}, {"dest_folder", U8(root + L"\\other\\")}})) == root + L"\\other" &&
                  ZipTargetFolder(Obj({{"paths", Paths({L"rel"})}})).empty() &&
                  ZipTargetFolder(json::Value::Object()).empty(),
              "zip: the folder a confirmation refers to");
        const size_t before = CountEntries(src);
        Check(PlanZip(Obj({{"paths", two}, {"name", "cancelled"}}), plan, error) &&
                  Fails(RunZip(plan, stop), "cancelled") && CountEntries(src) == before &&
                  !std::filesystem::exists(src + L"\\cancelled.zip"),
              "zip: a cancelled call leaves nothing behind");
    }

    std::filesystem::remove_all(L"\\\\?\\" + root, ec);
    Check(!std::filesystem::exists(root), "fixtures removed");
    std::printf("ai_tools_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
