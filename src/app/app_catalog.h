// app_catalog.h - the programs on the Start menu, for quick search.
//
// The list is the shell's Apps folder (what Start's "All apps" shows):
// desktop programs and Store apps alike, each with its shown name and a
// shell path ("shell:AppsFolder\<app id>") that both the icon cache and
// ShellExecuteEx understand. Enumerating it can take a while on a cold
// shell, so it runs on a worker and the popup works from the last list.
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulse::app {
struct AppEntry {
    std::wstring name;   // as Start shows it
    std::wstring path;   // AppsFolder parsing path
};

// "shell:AppsFolder\" + id
std::wstring AppPath(const std::wstring& id);
bool IsAppPath(const std::wstring& path);
// False for entries that are not programs to start: uninstallers, help
// files and web links put on the Start menu next to a program.
bool KeepApp(const std::wstring& name, const std::wstring& id);

class AppCatalog {
public:
    AppCatalog();
    // Starts a background enumeration when the list is older than
    // `max_age_ms` (or was never loaded); `message` is posted to `notify`
    // when a new list is in.
    void RefreshIfStale(HWND notify, UINT message, uint64_t max_age_ms = 10 * 60 * 1000);
    std::vector<AppEntry> Snapshot() const;
private:
    struct State {
        mutable std::mutex mu;
        std::vector<AppEntry> apps;
        uint64_t loaded_at = 0;
        bool loading = false, loaded = false;
    };
    std::shared_ptr<State> state_;
};

// Blocking enumeration (COM must be initialized on the calling thread).
std::vector<AppEntry> EnumerateApps();
}
