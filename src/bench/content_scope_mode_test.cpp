// content_scope_mode_test - unscoped content search scope and legacy
// content-v1.sqlite cleanup (src/app/content_scope_mode.h).
#include "../app/content_scope_mode.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

using namespace pulse::app;

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    fflush(stdout);
    if (!ok) ++failures;
}
void Write(const std::filesystem::path& file, size_t bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (size_t i = 0; i < bytes; ++i) out.put('x');
}

void NormalizeTests() {
    const std::set<std::wstring> present = {
        L"C:\\Users\\a\\Desktop", L"C:\\Users\\a\\Documents", L"C:\\Users\\a\\Downloads",
        L"C:\\Users\\a\\Desktop\\Docs", L"D:\\Data\\Downloads\\", L"D:\\Data", L"c:\\users\\A\\desktop\\"};
    auto exists = [&](const std::wstring& path) { return present.count(path) != 0; };

    auto out = NormalizeContentScopeFolders({L"C:\\Users\\a\\Desktop", L"C:\\Users\\a\\Documents", L"C:\\Users\\a\\Downloads"}, exists);
    Check(out.size() == 3 && out[0] == L"C:\\Users\\a\\Desktop" && out[2] == L"C:\\Users\\a\\Downloads",
          "three separate user folders are kept in order");

    out = NormalizeContentScopeFolders({L"C:\\Users\\a\\Desktop", L"c:\\users\\A\\desktop\\", L"C:\\Missing"}, exists);
    Check(out.size() == 1 && out[0] == L"C:\\Users\\a\\Desktop",
          "case and trailing-slash duplicates collapse; missing folders are dropped");

    out = NormalizeContentScopeFolders({L"C:\\Users\\a\\Desktop", L"C:\\Users\\a\\Desktop\\Docs"}, exists);
    Check(out.size() == 1 && out[0] == L"C:\\Users\\a\\Desktop", "a folder inside a kept folder is dropped");

    out = NormalizeContentScopeFolders({L"D:\\Data\\Downloads\\", L"C:\\Users\\a\\Documents", L"D:\\Data"}, exists);
    Check(out.size() == 2 && out[0] == L"C:\\Users\\a\\Documents" && out[1] == L"D:\\Data",
          "a later parent replaces an earlier nested folder");

    out = NormalizeContentScopeFolders({L"", L"C:\\Missing"}, exists);
    Check(out.empty(), "no existing folder gives an empty list");
}

void ModeTests() {
    Check(DefaultContentSearchRoots(false, ContentScopeMode::UserFolders).empty(),
          "a picked root list is never narrowed");
    Check(DefaultContentSearchRoots(true, ContentScopeMode::AllDrives).empty(),
          "all drives searches the whole shared configuration");
    const auto user = UserContentFolders();
    Check(DefaultContentSearchRoots(true, ContentScopeMode::UserFolders) == user &&
          DefaultContentSearchRoots(true, ContentScopeMode::Unset) == user,
          "user folders (and not chosen yet) narrow the shared configuration");
    printf("user folders:");
    for (const auto& folder : user) printf(" [%ls]", folder.c_str());
    printf("\n");
    Check(!user.empty() && user.size() <= 3, "this machine reports one to three user folders");

    SetContentScopeMode(2);
    Check(CurrentContentScopeMode() == ContentScopeMode::AllDrives && DefaultContentSearchRoots(true).empty(),
          "the process-wide mode reaches the global search window");
    SetContentScopeMode(7);
    Check(CurrentContentScopeMode() == ContentScopeMode::Unset, "unknown stored values read as not chosen");
    SetContentScopeMode(0);
}

void CleanupTests(const std::filesystem::path& base) {
    const auto dir = base / L"ContentIndex";
    std::filesystem::create_directories(dir);
    const auto db = dir / L"content-v1.sqlite";
    const std::wstring path = db.wstring();

    auto missing = DeleteLegacyContentDatabase(path);
    Check(!missing.found && !missing.deleted && missing.bytes == 0, "no database: nothing found, nothing deleted");

    Write(db, 3000);
    Write(path + L"-wal", 500);
    Write(path + L"-shm", 70);
    Write(path + L".config", 11);
    Write(path + L".status", 13);
    {
        HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "fixture opens the database without sharing");
        const auto busy = DeleteLegacyContentDatabase(path);
        CloseHandle(locked);
        Check(busy.found && !busy.deleted && busy.bytes == 0, "a database in use is left for the next launch");
        Check(std::filesystem::exists(db) && std::filesystem::exists(path + L"-wal") && std::filesystem::exists(path + L"-shm"),
              "companions of a database in use are kept");
    }

    const auto done = DeleteLegacyContentDatabase(path);
    Check(done.found && done.deleted && done.bytes == 3570, "database, -wal and -shm are deleted and their sizes summed");
    Check(!std::filesystem::exists(db) && !std::filesystem::exists(path + L"-wal") && !std::filesystem::exists(path + L"-shm"),
          "no legacy database file remains");
    Check(std::filesystem::exists(path + L".config") && std::filesystem::exists(path + L".status"),
          ".config and .status stay for Instant mode");

    const auto again = DeleteLegacyContentDatabase(path);
    Check(!again.found && !again.deleted, "a second run finds nothing");

    std::filesystem::create_directories(dir / L"folder.sqlite");
    const auto folder = DeleteLegacyContentDatabase((dir / L"folder.sqlite").wstring());
    Check(!folder.found && std::filesystem::exists(dir / L"folder.sqlite"), "a directory with the database name is not touched");
    Check(!DeleteLegacyContentDatabase(L"").found, "an empty path is ignored");
}
} // namespace

int main() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto base = std::filesystem::path(temp) / (L"pulse-content-scope-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(base, ignored);
    std::filesystem::create_directories(base);
    NormalizeTests();
    ModeTests();
    CleanupTests(base);
    std::filesystem::remove_all(base, ignored);
    printf("content scope mode failures=%d\n", failures);
    return failures ? 1 : 0;
}
