// A context-menu handler that never returns from QueryContextMenu (#65): the
// menu still settles, the handler is reported as timed out, and pulse_shell
// recycles itself once idle instead of leaking the stuck thread for good.
// Drives the real pulse_shell.exe next to this test through ShellClient.
#include "../ipc/shell_client.h"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kStuckHandler[] = L"{50554C53-4500-4D41-4E47-000000000065}";
constexpr wchar_t kStuckVariable[] = L"PULSE_SHELL_TEST_STUCK_HANDLER";

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_passed : ++g_failed;
}

struct Session {
    bool partial = false;
    bool final = false;
    size_t items = 0;
    std::vector<std::wstring> slow;
    std::vector<pulse::ipc::CtxHandlerStatus> handlers;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::map<uint32_t, Session> g_sessions; // by query id

template <class Pred>
bool WaitFor(uint32_t id, Pred pred, int ms) {
    std::unique_lock<std::mutex> lock(g_mutex);
    return g_cv.wait_for(lock, std::chrono::milliseconds(ms),
                         [&] { return pred(g_sessions[id]); });
}

Session Snapshot(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sessions[id];
}

// The pulse_shell.exe this process started (ShellClient spawns it lazily).
HANDLE HostProcess() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    PROCESSENTRY32W entry{ sizeof(entry) };
    HANDLE found = nullptr;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !found;
         more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == GetCurrentProcessId() &&
            _wcsicmp(entry.szExeFile, L"pulse_shell.exe") == 0)
            found = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                entry.th32ProcessID);
    }
    CloseHandle(snapshot);
    return found;
}

} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"pulse_shell_recycle_test_" +
                             std::to_wstring(GetCurrentProcessId());
    // Menus for hang.txt get a handler that never returns; menu.txt is the control.
    const std::wstring hang = dir + L"\\hang.txt";
    const std::wstring file = dir + L"\\menu.txt";
    CreateDirectoryW(dir.c_str(), nullptr);
    for (const auto& path : { hang, file }) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    SetEnvironmentVariableW(kStuckVariable, L"hang.txt"); // inherited by every host

    pulse::ipc::ShellClient::Callbacks callbacks;
    callbacks.ctx_items = [](uint32_t id, std::vector<pulse::ipc::CtxMenuItem> items, bool partial,
                             pulse::ipc::CtxItemsExtra extra) {
        std::lock_guard<std::mutex> lock(g_mutex);
        Session& s = g_sessions[id];
        s.items = items.size();
        if (partial) s.partial = true;
        else {
            s.final = true;
            s.slow = std::move(extra.slow_clsids);
            s.handlers = std::move(extra.handlers);
        }
        g_cv.notify_all();
    };
    callbacks.done = [](uint32_t, uint32_t, bool, std::wstring) {};
    auto& client = pulse::ipc::ShellClient::Instance();
    client.Start(std::move(callbacks));

    // --- a handler hangs ----------------------------------------------------
    const uint32_t first = client.QueryContextMenu({ hang }, 0, false, false);
    Check(first != 0, "the context menu query is sent");
    Check(WaitFor(first, [](const Session& s) { return s.partial || s.final; }, 5000),
          "the menu shows the answering handlers while one hangs");
    HANDLE host = HostProcess();
    Check(host != nullptr, "the host process is found");
    const auto settle_start = std::chrono::steady_clock::now();
    Check(WaitFor(first, [](const Session& s) { return s.final; }, 9000),
          "the menu settles without the hung handler");
    const auto settle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - settle_start).count();
    const Session settled = Snapshot(first);
    std::printf("  settled after %lld ms, %zu items\n", static_cast<long long>(settle_ms),
                settled.items);
    bool reported = false;
    for (const auto& clsid : settled.slow) reported |= clsid == kStuckHandler;
    Check(reported, "the hung handler is reported as timed out");
    bool stuck_unfinished = false;
    size_t listed_rows = 0;
    for (const auto& h : settled.handlers) {
        if (h.clsid == kStuckHandler)
            stuck_unfinished = !(h.flags & pulse::ipc::CTX_HANDLER_DONE) && h.elapsed_ms >= 4000;
        listed_rows += h.rows;
    }
    std::printf("  %zu handler states, %zu rows listed\n", settled.handlers.size(), listed_rows);
    Check(stuck_unfinished && listed_rows == settled.items,
          "the answer names the hung handler as unfinished and accounts for every row");
    Check(host && WaitForSingleObject(host, 0) == WAIT_TIMEOUT,
          "the host keeps serving while the menu is open");

    client.CloseContextMenu(first);
    Check(host && WaitForSingleObject(host, 15000) == WAIT_OBJECT_0,
          "the host recycles itself once the menu closes");
    if (host) CloseHandle(host);

    // --- the next right-click gets a fresh host ----------------------------
    const uint32_t second = client.QueryContextMenu({ file }, 0, false, false);
    Check(second != 0 && WaitFor(second, [](const Session& s) { return s.final; }, 20000),
          "the next menu is served by a fresh host");
    Check(Snapshot(second).items > 0, "the fresh host lists the menu items");
    HANDLE fresh = HostProcess();
    client.CloseContextMenu(second);
    Sleep(3000);
    DWORD fresh_exit = 0;
    if (fresh) GetExitCodeProcess(fresh, &fresh_exit);
    std::printf("  fresh host %s, exit code %lu\n", fresh ? "found" : "not found",
                static_cast<unsigned long>(fresh_exit));
    Check(fresh && WaitForSingleObject(fresh, 0) == WAIT_TIMEOUT,
          "a host without hung handlers is not recycled");
    if (fresh) CloseHandle(fresh);

    client.Stop();
    DeleteFileW(hang.c_str());
    DeleteFileW(file.c_str());
    RemoveDirectoryW(dir.c_str());
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}