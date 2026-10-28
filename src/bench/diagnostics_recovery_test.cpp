#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"
#include "../common/diagnostics_cleanup_io.h"
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <cstdio>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace {
int failures = 0;
void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); if (!ok) ++failures; }
void Write(const fs::path& path) { std::ofstream(path) << "{}"; }
bool Wait(auto predicate) {
    const auto end = GetTickCount64() + 10000;
    do { if (predicate()) return true; Sleep(20); } while (GetTickCount64() < end);
    return predicate();
}
size_t Groups(const fs::path& dir) {
    std::set<fs::path> stems;
    for (const auto& item : fs::directory_iterator(dir)) {
        const auto ext = item.path().extension();
        if (ext == L".json" || ext == L".log" || ext == L".dmp") stems.insert(item.path().stem());
    }
    return stems.size();
}
void Old(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    FILETIME time{1, 0};
    if (file != INVALID_HANDLE_VALUE) { SetFileTime(file, nullptr, nullptr, &time); CloseHandle(file); }
}
std::wstring Self() { wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768); return path; }
PROCESS_INFORMATION Child(const wchar_t* mode, const fs::path& root) {
    std::wstring command = L"\"" + Self() + L"\" " + mode + L" \"" + root.wstring() + L"\"";
    STARTUPINFOW start{sizeof(start)}; PROCESS_INFORMATION process{};
    Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &start, &process) != FALSE,
        "private subprocess starts");
    if (process.hThread) CloseHandle(process.hThread);
    return process;
}
bool Finish(PROCESS_INFORMATION process) {
    if (!process.hProcess) return false;
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 5000); }
    DWORD code = 99; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && code == 0;
}
DWORD WINAPI Denied(LPCWSTR) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_FILE_ATTRIBUTES; }
HANDLE WINAPI DeniedFirst(LPCWSTR, LPWIN32_FIND_DATAW) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
BOOL WINAPI BrokenNext(HANDLE, LPWIN32_FIND_DATAW) { SetLastError(ERROR_READ_FAULT); return FALSE; }
HANDLE WINAPI ArtifactFirst(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    HANDLE find = FindFirstFileW(path, data);
    if (find != INVALID_HANDLE_VALUE) {
        while ((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && FindNextFileW(find, data)) {}
    }
    return find;
}
void ClearTests(const fs::path& root) {
    using namespace pulse::diagnostics;
    std::wstring error = L"stale";
    Check(ClearCrashReports(root.wstring(), &error) && error.empty(), "missing directory is successful empty cleanup");
    auto dir = root / L"Diagnostics" / L"Crashes"; fs::create_directories(dir);
    Check(ClearCrashReports(root.wstring(), &error) && error.empty(), "existing empty directory is successful EOF");
    Write(dir / L"one.json"); Write(dir / L"two.dmp"); Write(dir / L"keep.txt");
    Check(ClearCrashReports(root.wstring(), &error) && !fs::exists(dir / L"one.json") &&
        !fs::exists(dir / L"two.dmp") && fs::exists(dir / L"keep.txt"), "real cleanup removes only diagnostic artifacts");
    CleanupIo io; io.attributes = Denied;
    Check(!ClearCrashReportsWithIo(root.wstring(), &error, io) && error.find(L"5") != std::wstring::npos,
        "attribute access denied is failure with diagnostic");
    io = {}; io.first = DeniedFirst;
    Check(!ClearCrashReportsWithIo(root.wstring(), &error, io) && !error.empty(), "FindFirst denial is failure");
    Write(dir / L"partial.json"); fs::remove(dir / L"keep.txt");
    io = {}; io.first = ArtifactFirst; io.next = BrokenNext;
    Check(!ClearCrashReportsWithIo(root.wstring(), &error, io) && error.find(L"30") != std::wstring::npos &&
        !fs::exists(dir / L"partial.json"), "partial deletion plus FindNext read error cannot report complete");
    Write(dir / L"locked.json"); Write(dir / L"free.json");
    HANDLE lock = CreateFileW((dir / L"locked.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "private locked artifact fixture opens");
    Check(!ClearCrashReports(root.wstring(), &error) && !error.empty() && fs::exists(dir / L"locked.json") &&
        !fs::exists(dir / L"free.json"), "locked deletion preserves error and other successful work");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    Check(ClearCrashReports(root.wstring(), &error) && error.empty() && !fs::exists(dir / L"locked.json"), "unlocked retry succeeds");
    fs::remove(dir); Write(dir);
    Check(!ClearCrashReports(root.wstring(), &error) && !error.empty(), "file substituted for directory is failure");
}
void Retention(const fs::path& root) {
    Check(pulse::crash::Initialize({pulse::crash::ProcessRole::Test, false, root.wstring()}), "initialize private diagnostics");
    const auto dir = root / L"Diagnostics" / L"Crashes";
    for (int i = 0; i < 13; ++i) { pulse::crash::ReportRecoverable(nullptr, "living-host"); Sleep(2); }
    Check(Wait([&] { return Groups(dir) == 10; }), "living host prunes repeated recoverable reports without restart");
    pulse::crash::ReportRecoverable(nullptr, "retention-latest");
    Check(Wait([&] { return Groups(dir) == 10; }), "subsequent report schedules another maintenance pass");
    bool latest = false;
    for (const auto& item : fs::directory_iterator(dir)) if (item.path().extension() == L".json") {
        std::ifstream input(item.path()); std::string text((std::istreambuf_iterator<char>(input)), {});
        latest |= text.find("retention-latest") != std::string::npos;
    }
    Check(latest, "latest completed report remains available");
    HANDLE oversized = CreateFileW((dir / L"oversized.dmp").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, 0, nullptr);
    bool sized = false;
    if (oversized != INVALID_HANDLE_VALUE) {
        DWORD ignored = 0;
        DeviceIoControl(oversized, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr);
        LARGE_INTEGER bytes{}; bytes.QuadPart = 201ll * 1024 * 1024;
        sized = SetFilePointerEx(oversized, bytes, nullptr, FILE_BEGIN) && SetEndOfFile(oversized);
        CloseHandle(oversized); Old(dir / L"oversized.dmp");
    }
    Check(sized, "private oversized dump fixture has exact logical size");
    pulse::crash::ReportRecoverable(nullptr, "byte-budget");
    Check(Wait([&] { return !fs::exists(dir / L"oversized.dmp") && Groups(dir) <= 10; }),
        "runtime maintenance enforces aggregate byte budget");
    Write(dir / L"locked.json"); Old(dir / L"locked.json");
    HANDLE lock = CreateFileW((dir / L"locked.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "retention locked oldest fixture");
    pulse::crash::ReportRecoverable(nullptr, "locked-oldest");
    Check(Wait([&] { return Groups(dir) == 10; }) && fs::exists(dir / L"locked.json"), "failed delete is not counted as reclaimed event");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    pulse::crash::ReportRecoverable(nullptr, "unlocked-oldest");
    Check(Wait([&] { return !fs::exists(dir / L"locked.json") && Groups(dir) == 10; }), "unlocked oldest can be reclaimed later");
    auto writer = Child(L"--lease", root);
    Check(Wait([&] { return fs::exists(root / L"ready"); }), "other process owns live report lease");
    pulse::crash::ReportRecoverable(nullptr, "writer-active");
    Check(Wait([&] { return Groups(dir) == 10; }) && fs::exists(dir / L"live.json"), "retention skips active cross-process writer");
    Write(root / L"release"); Check(Finish(writer), "writer exits without closing lease explicitly");
    Check(!fs::exists(dir / L"live.writing"), "OS removes lease after abrupt writer exit");
    pulse::crash::ReportRecoverable(nullptr, "writer-exited");
    Check(Wait([&] { return !fs::exists(dir / L"live.json") && Groups(dir) == 10; }), "crashed writer cannot pin report forever");
    // Idle means idle: the sweep used to re-create retention.lock every second.
    Sleep(300);  // let the last sweep close its lock
    HANDLE change = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
    const bool idle = change != INVALID_HANDLE_VALUE && WaitForSingleObject(change, 2500) == WAIT_TIMEOUT;
    if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);
    Check(idle, "an idle process creates and deletes nothing in the reports folder");
    const auto before = GetTickCount64(); pulse::crash::Shutdown();
    Check(GetTickCount64() - before < 1000, "shutdown signals maintenance without joining filesystem I/O");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--lease") {
        fs::path root(argv[2]); auto dir = root / L"Diagnostics" / L"Crashes";
        HANDLE lease = CreateFileW((dir / L"live.writing").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lease == INVALID_HANDLE_VALUE) return 3;
        Write(dir / L"live.json"); Old(dir / L"live.json"); Write(root / L"ready");
        if (!Wait([&] { return fs::exists(root / L"release"); })) return 4;
        ExitProcess(0);
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--retention") {
        Retention(argv[2]); fflush(stdout); return failures ? 1 : 0;
    }
    fs::path root = fs::absolute(L"bench_data") / (L"diagnostics-recovery-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root.parent_path());
    if (!fs::create_directory(root)) return 2;
    ClearTests(root / L"clear");
    Check(Finish(Child(L"--retention", root / L"reports")), "recoverable lifetime subprocess completes successfully");
    // Every child has exited; only this uniquely created fixture is removed.
    fs::remove_all(root);
    printf("Failures: %d\n", failures); return failures ? 1 : 0;
}
