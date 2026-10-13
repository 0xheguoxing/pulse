#pragma once

// shell_registry_watch.h - the registry keys whose changes alter right-click
// menus, watched on one thread.
//
// Explorer saves folder views under HKCU\Software\Classes\Local Settings
// (Bags, BagMRU, MrtCache) dozens of times a minute. A recursive watch on all
// of Classes turned each of those writes into a flush of every menu cache, so
// only the keys menus are built from are watched here: the scene keys every
// item of a kind shares (*, Directory, Drive, SystemFileAssociations ...) as
// subtrees, the Classes roots themselves for new or removed types, blocked
// extensions and the per-user file associations. Verbs a type's own ProgID
// defines are re-read when the menu is used (RequestStaticRevalidate).

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

struct RegistryWatchKey {
    HKEY root = nullptr;
    std::wstring path;
    bool subtree = false;
};

// `user_software` / `machine_software` are the "Software" keys under each
// root; tests pass a private fixture instead.
std::vector<RegistryWatchKey> ShellMenuWatchKeys(
    HKEY user_root = HKEY_CURRENT_USER, const std::wstring& user_software = L"Software",
    HKEY machine_root = HKEY_LOCAL_MACHINE, const std::wstring& machine_software = L"Software");

// One thread waits on every key and calls `changed` (on that thread) once per
// burst of changes (ShellRegistryDebounce). A key that does not exist yet is
// opened as soon as it appears; its parent's watch is what wakes the thread.
class ShellRegistryWatcher {
public:
    ShellRegistryWatcher() = default;
    ~ShellRegistryWatcher();
    ShellRegistryWatcher(const ShellRegistryWatcher&) = delete;
    ShellRegistryWatcher& operator=(const ShellRegistryWatcher&) = delete;

    bool Start(std::vector<RegistryWatchKey> keys, std::function<void()> changed);
    void Stop();
    bool running() const { return thread_ != nullptr; }

private:
    static DWORD WINAPI ThreadMain(LPVOID self);
    void Run();

    std::vector<RegistryWatchKey> keys_;
    std::function<void()> changed_;
    HANDLE stop_ = nullptr;
    HANDLE thread_ = nullptr;
};

} // namespace pulse::app
