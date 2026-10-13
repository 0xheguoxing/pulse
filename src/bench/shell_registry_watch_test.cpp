// shell_registry_watch_test.cpp - which registry writes flush the right-click
// menu caches (ShellRegistryWatcher over ShellMenuWatchKeys). Runs against a
// private fixture under HKCU\Software\PulseTest, never the real Classes keys.
#include "../app/shell_registry_watch.h"
#include "../app/shell_registry_debounce.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

std::wstring g_base;
std::atomic<int> g_changes{0};
HANDLE g_changed = nullptr;

bool Write(const std::wstring& sub, const wchar_t* name = L"v", const wchar_t* value = L"1") {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (g_base + L"\\" + sub).c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                                       static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

// RegDeleteTree with a subkey removes that key too.
bool Delete(const std::wstring& sub) {
    const std::wstring path = g_base + L"\\" + sub;
    HKEY key = nullptr;
    const bool deleted = RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS;
    const bool gone = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) ==
                      ERROR_FILE_NOT_FOUND;
    if (key) RegCloseKey(key);
    return deleted && gone;
}

// Flushes seen after the burst settles: the debounce waits kQuietMs of
// silence, so give it that plus slack.
int Settle() {
    constexpr DWORD kSlackMs = 1200;
    const int before = g_changes.load();
    const DWORD wait = static_cast<DWORD>(pulse::app::ShellRegistryDebounce::kQuietMs) + kSlackMs;
    const ULONGLONG until = GetTickCount64() + wait;
    while (GetTickCount64() < until)
        WaitForSingleObject(g_changed, static_cast<DWORD>(until - GetTickCount64()));
    return g_changes.load() - before;
}

} // namespace

int wmain() {
    using namespace pulse::app;
    g_base = L"Software\\PulseTest\\RegWatch-" + std::to_wstring(GetCurrentProcessId());
    RegDeleteTreeW(HKEY_CURRENT_USER, g_base.c_str());
    g_changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const std::wstring ext = L"Microsoft\\Windows\\CurrentVersion\\Shell Extensions";
    // What a profile has before the watch starts. user\Classes\Drive and both
    // Blocked keys are missing on purpose.
    const bool fixture =
        Write(L"user\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Shell") &&
        Write(L"user\\Classes\\*\\shell") && Write(L"user\\Classes\\Directory\\shell") &&
        Write(L"user\\Classes\\PulseTest.File\\shell\\open\\command") &&
        Write(L"user\\" + ext) &&
        Write(L"user\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.dwg\\UserChoice") &&
        Write(L"machine\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers") &&
        Write(L"machine\\" + ext);
    Check(fixture, "fixture keys created");

    const auto keys = ShellMenuWatchKeys(HKEY_CURRENT_USER, g_base + L"\\user", HKEY_CURRENT_USER,
                                         g_base + L"\\machine");
    Check(keys.size() < MAXIMUM_WAIT_OBJECTS, "every key fits one wait");
    ShellRegistryWatcher watcher;
    Check(watcher.Start(keys, [] {
        ++g_changes;
        SetEvent(g_changed);
    }), "watcher starts");
    Sleep(100);

    // Explorer saving folder views: the writes that used to flush every cache.
    for (int i = 0; i < 20; ++i)
        Write(L"user\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\Bags\\" +
              std::to_wstring(i % 3 + 1) + L"\\Shell", L"Mode", std::to_wstring(i).c_str());
    Write(L"user\\Classes\\Local Settings\\MrtCache\\x", L"k", L"v");
    Check(Settle() == 0, "folder-view writes under Local Settings flush nothing");

    Write(L"user\\Classes\\*\\shell\\PulseVerb\\command");
    Check(Settle() == 1, "a verb added for every file flushes once");

    Write(L"machine\\Classes\\Directory\\Background\\shellex\\ContextMenuHandlers\\Pulse",
          nullptr, L"{00000000-0000-0000-0000-000000000001}");
    Check(Settle() == 1, "a machine-wide background handler flushes");

    Write(L"user\\Classes\\Drive\\shell\\PulseDrive");
    Check(Settle() == 1, "a scene key that did not exist flushes when it appears");
    Write(L"user\\Classes\\Drive\\shell\\PulseDrive", L"MUIVerb", L"Drive verb");
    Check(Settle() == 1, "the key that appeared is watched from then on");

    Write(L"user\\" + ext + L"\\Blocked", L"{00000000-0000-0000-0000-000000000002}", L"");
    Check(Settle() == 1, "the first blocked handler flushes");
    Write(L"user\\" + ext + L"\\Blocked", L"{00000000-0000-0000-0000-000000000003}", L"");
    Check(Settle() == 1, "another blocked handler flushes");

    Write(L"user\\Classes\\.pulsetest", nullptr, L"PulseTest.File");
    Check(Settle() == 1, "a new file type flushes");

    Write(L"user\\Classes\\PulseTest.File\\shell\\open\\command", nullptr, L"x.exe %1");
    Check(Settle() == 0, "a ProgID's own verbs are left to the re-read on use");

    Write(L"user\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.dwg\\UserChoice",
          L"ProgId", L"PulseTest.File");
    Check(Settle() == 1, "a changed default app flushes");

    Check(Delete(L"user\\Classes\\Drive"), "scene key deleted");
    Check(Settle() >= 1, "deleting a watched key flushes");
    Write(L"user\\Classes\\Drive\\shell\\Again");
    Check(Settle() == 1, "a deleted key is watched again once it is back");
    Write(L"user\\Classes\\Drive\\shell\\Again", L"MUIVerb", L"again");
    Check(Settle() == 1, "...including writes below it");

    for (int i = 0; i < 200; ++i)
        Write(L"user\\Classes\\*\\shell\\Burst" + std::to_wstring(i % 10));
    Check(Settle() == 1, "a burst of writes flushes once");

    const ULONGLONG stop_started = GetTickCount64();
    watcher.Stop();
    Check(!watcher.running() && GetTickCount64() - stop_started < 1000, "watcher stops promptly");
    Write(L"user\\Classes\\*\\shell\\AfterStop");
    Check(Settle() == 0, "nothing flushes after stop");

    RegDeleteTreeW(HKEY_CURRENT_USER, g_base.c_str());
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
