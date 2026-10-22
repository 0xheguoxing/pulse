#include <windows.h>
#include <atomic>
#include <thread>
#include <cstdio>
#include <filesystem>
static HANDLE entered, release_provider;
static std::atomic<int> mode{0}, fallback_calls{0};
static HANDLE WINAPI TestFindFirst(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
    FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    ++fallback_calls;
    if (mode == 3) { SetEvent(entered); WaitForSingleObject(release_provider, INFINITE); }
    return FindFirstFileExW(path, level, data, search, filter, flags);
}
#define FindFirstFileExW TestFindFirst
#include "../fs/fs_enum.cpp"
#undef FindFirstFileExW
using namespace pulse::fs;
static NTSTATUS NTAPI TestCreate(PHANDLE h, ACCESS_MASK, NtObjectAttributes*, NtIoStatusBlock*,
    PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG) {
    if (mode == 3) return static_cast<NTSTATUS>(0xC0000001L);
    if (mode == 1) { SetEvent(entered); WaitForSingleObject(release_provider, INFINITE); }
    *h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return STATUS_SUCCESS;
}
static std::thread completion;
static NTSTATUS NTAPI TestQuery(HANDLE, HANDLE event, NtPioApcRoutine, PVOID,
    NtIoStatusBlock* status, PVOID buffer, ULONG, NtFileInformationClass, BOOLEAN, NtUnicodeString*, BOOLEAN) {
    if (mode != 2) return STATUS_NO_MORE_FILES;
    completion = std::thread([=] {
        SetEvent(entered);
        WaitForSingleObject(release_provider, INFINITE);
        // Touch both pieces of provider-owned storage after the caller returns.
        static_cast<BYTE*>(buffer)[0] = 0;
        status->Information = 0;
        status->Status = static_cast<NTSTATUS>(0xC0000120L);
        SetEvent(event);
    });
    return STATUS_PENDING;
}
static int failures = 0;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static bool Drain() {
    for (int i = 0; i != 500 && (active_network_enumerations || active_local_enumerations); ++i) Sleep(2);
    return !active_network_enumerations && !active_local_enumerations;
}
int main() {
    InitNtApi();
    const auto original_create = g_NtCreateFile;
    const auto original_query = g_NtQueryDirectoryFile;
    g_NtCreateFile = TestCreate;
    g_NtQueryDirectoryFile = TestQuery;
    entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_provider = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto path = std::filesystem::absolute(L"bench_data").wstring();
    for (int stage = 1; stage <= 3; ++stage) {
        for (bool timeout : {false, true}) {
            mode = stage;
            ResetEvent(entered); ResetEvent(release_provider); fallback_calls = 0;
            std::atomic_bool cancelled{false}, returned{false};
            bool rejected = false;
            std::thread caller([&] {
                try {
                    RunBoundedEnumeration<std::vector<DirEntry>>(true, [path](const std::atomic_bool& token) {
                        std::vector<DirEntry> entries;
                        EnumerateDirectory(path, entries, &token);
                        return entries;
                    }, [&] { return cancelled.load(); }, std::chrono::milliseconds(timeout ? 120 : 5000));
                } catch (const std::exception&) { rejected = true; }
                returned = true;
            });
            Check(WaitForSingleObject(entered, 2000) == WAIT_OBJECT_0, "provider blocked at open / pending query / fallback");
            const auto started = GetTickCount64();
            if (!timeout) cancelled = true;
            while (!returned && GetTickCount64() - started < 1000) Sleep(2);
            Check(returned, timeout ? "deadline returns within one second" : "cancel returns within one second");
            Check(active_network_enumerations == 1, "unfinished provider state remains owned");
            SetEvent(release_provider);
            caller.join();
            Check(Drain(), "late completion releases provider slot");
            if (completion.joinable()) completion.join();
            Check(rejected, "no late or partial listing published");
            if (stage == 2) Check(fallback_calls == 0, "cancelled NT query does not start fallback");
        }
    }
    mode = 0;
    ResetEvent(release_provider);
    for (int i = 0; i < 4; ++i) {
        try {
            RunBoundedEnumeration<int>(true, [](const std::atomic_bool&) {
                WaitForSingleObject(release_provider, INFINITE); return 1;
            }, [] { return false; }, std::chrono::milliseconds(30));
        } catch (...) {}
    }
    Check(active_network_enumerations == 4, "retired network work capped at four");
    bool refused = false;
    const auto waited_from = GetTickCount64();
    try {
        RunBoundedEnumeration<int>(true, [](const std::atomic_bool&) { return 1; }, [] { return false; },
                                   std::chrono::milliseconds(150));
    } catch (...) { refused = true; }
    const auto waited = GetTickCount64() - waited_from;
    Check(refused && active_network_enumerations == 4, "exhausted network quota still fails at its deadline");
    Check(waited >= 100 && waited < 1000, "exhausted network quota waits for a slot within the budget");
    bool cancelled_wait = false;
    std::atomic_bool stop_waiting{false};
    std::thread canceller([&] { Sleep(60); stop_waiting = true; });
    try {
        RunBoundedEnumeration<int>(true, [](const std::atomic_bool&) { return 1; },
                                   [&] { return stop_waiting.load(); }, std::chrono::seconds(10));
    } catch (const EnumerationCancelled&) { cancelled_wait = true; } catch (...) {}
    canceller.join();
    Check(cancelled_wait && active_network_enumerations == 4, "a request waiting for a slot can be cancelled");
    Check(RunBoundedEnumeration<int>(false, [](const std::atomic_bool&) { return 7; }, [] { return false; }) == 7,
        "local navigation retains independent capacity");
    // A slot that frees up while a request waits lets that request run (#226:
    // a share must not look offline only because other listings were stuck).
    int late = 0;
    std::thread releaser([] { Sleep(80); SetEvent(release_provider); });
    try {
        late = RunBoundedEnumeration<int>(true, [](const std::atomic_bool&) { return 9; }, [] { return false; },
                                          std::chrono::seconds(5));
    } catch (...) {}
    releaser.join();
    Check(late == 9, "a request waiting for a slot runs once one frees up");
    Check(Drain(), "all retired jobs reclaimed after release");
    bool ran = false;
    try { RunBoundedEnumeration<int>(false, [&](const std::atomic_bool&) { ran = true; return 1; }, [] { return true; }); }
    catch (const EnumerationCancelled&) {}
    Check(!ran, "already cancelled request never starts provider");
    g_NtCreateFile = original_create;
    g_NtQueryDirectoryFile = original_query;
    auto entries = RunBoundedEnumeration<std::vector<DirEntry>>(false, [path](const std::atomic_bool& token) {
        std::vector<DirEntry> result; EnumerateDirectory(path, result, &token); return result;
    }, [] { return false; });
    Check(!entries.empty(), "real local directory enumeration succeeds");
    CloseHandle(entered); CloseHandle(release_provider);
    return failures ? 1 : 0;
}
