#include "shell_registry_watch.h"

#include "shell_registry_debounce.h"

namespace pulse::app {

namespace {

// Keys every item of a kind reads its verbs and extensions from. A file's own
// ProgID (AutoCAD.Drawing.24\shell ...) is not among them: those are spread
// over the whole Classes root and re-read on use instead.
constexpr const wchar_t* kSceneKeys[] = {
    L"*", L"AllFilesystemObjects", L"Directory", L"Folder", L"Drive",
    L"DesktopBackground", L"SystemFileAssociations", L"lnkfile", L"Unknown",
};

} // namespace

std::vector<RegistryWatchKey> ShellMenuWatchKeys(HKEY user_root, const std::wstring& user_software,
                                                 HKEY machine_root,
                                                 const std::wstring& machine_software) {
    std::vector<RegistryWatchKey> keys;
    auto add_root = [&](HKEY root, const std::wstring& software) {
        const std::wstring classes = software + L"\\Classes";
        // Not a subtree: a type or ProgID added or removed, without the
        // Local Settings writes below it.
        keys.push_back({ root, classes, false });
        for (const wchar_t* scene : kSceneKeys)
            keys.push_back({ root, classes + L"\\" + scene, true });
        const std::wstring extensions =
            software + L"\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions";
        keys.push_back({ root, extensions, false });              // Blocked appears
        keys.push_back({ root, extensions + L"\\Blocked", false }); // a handler blocked
    };
    add_root(user_root, user_software);
    add_root(machine_root, machine_software);
    // Default apps and Open With lists.
    keys.push_back({ user_root,
                     user_software + L"\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts",
                     true });
    return keys;
}

ShellRegistryWatcher::~ShellRegistryWatcher() { Stop(); }

bool ShellRegistryWatcher::Start(std::vector<RegistryWatchKey> keys,
                                 std::function<void()> changed) {
    if (thread_) return false;
    // One wait slot is the stop event.
    if (keys.empty() || keys.size() >= MAXIMUM_WAIT_OBJECTS) return false;
    keys_ = std::move(keys);
    changed_ = std::move(changed);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_) return false;
    thread_ = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
    if (!thread_) {
        CloseHandle(stop_);
        stop_ = nullptr;
        return false;
    }
    return true;
}

void ShellRegistryWatcher::Stop() {
    if (stop_) SetEvent(stop_);
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (stop_) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
}

DWORD WINAPI ShellRegistryWatcher::ThreadMain(LPVOID self) {
    static_cast<ShellRegistryWatcher*>(self)->Run();
    return 0;
}

void ShellRegistryWatcher::Run() {
    struct Watch {
        HKEY key = nullptr;
        HANDLE event = nullptr;
    };
    std::vector<Watch> watches(keys_.size());
    constexpr DWORD kFilter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET;
    auto close_key = [](Watch& w) {
        if (w.key) RegCloseKey(w.key);
        w.key = nullptr;
    };
    // Opens the key if needed and arms it; false while it does not exist (or
    // was deleted: re-arming a deleted key fails).
    auto arm = [&](size_t i) {
        Watch& w = watches[i];
        if (!w.event) w.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!w.event) return false;
        if (!w.key && RegOpenKeyExW(keys_[i].root, keys_[i].path.c_str(), 0, KEY_NOTIFY,
                                    &w.key) != ERROR_SUCCESS) {
            w.key = nullptr;
            return false;
        }
        if (RegNotifyChangeKeyValue(w.key, keys_[i].subtree ? TRUE : FALSE, kFilter, w.event,
                                    TRUE) != ERROR_SUCCESS) {
            close_key(w);
            return false;
        }
        return true;
    };
    for (size_t i = 0; i < watches.size(); ++i) arm(i);

    // A registry cleaner fires these thousands of times in a row; flush once
    // per burst instead of re-seeding the verb cache on every change (#127).
    ShellRegistryDebounce debounce;
    std::vector<HANDLE> wait;
    std::vector<size_t> owner;
    for (;;) {
        wait.assign(1, stop_);
        owner.assign(1, SIZE_MAX);
        for (size_t i = 0; i < watches.size(); ++i) {
            if (!watches[i].key) continue;
            wait.push_back(watches[i].event);
            owner.push_back(i);
        }
        const DWORD count = static_cast<DWORD>(wait.size());
        const DWORD r = WaitForMultipleObjects(count, wait.data(), FALSE,
                                               debounce.WaitMs(GetTickCount64()));
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
        if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count) {
            // Re-arm only the key that fired: the others are still armed, and
            // re-registering them would stack notifications on the key.
            arm(owner[r - WAIT_OBJECT_0]);
            debounce.Note(GetTickCount64());
        }
        // A key that appeared since (its parent's watch woke us): watch it, and
        // whatever was written into it before it was armed is a change too.
        for (size_t i = 0; i < watches.size(); ++i)
            if (!watches[i].key && arm(i)) debounce.Note(GetTickCount64());
        // Checked after every wake: during a steady stream the events never
        // let the wait time out, and the max delay must still flush.
        if (debounce.TakeDue(GetTickCount64()) && changed_) changed_();
    }
    for (auto& w : watches) {
        close_key(w);
        if (w.event) CloseHandle(w.event);
    }
}

} // namespace pulse::app
