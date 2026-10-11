// watch_lease_test.cpp - A folder watch must not block renaming or moving the
// folder or a folder above it (multi-window M6e: another window showing a
// subfolder made renaming its parent fail with error 5). The watch holds a
// handle lease, steps aside when it breaks, and keeps reporting changes.
#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../fs/fs_watch.h"

namespace {
int failures = 0;

void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
    if (!ok) ++failures;
}

bool WaitFor(const std::function<bool()>& done, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

struct Recorder {
    std::mutex mutex;
    std::vector<std::wstring> names;
    std::atomic<int> overflows{0};

    pulse::fs::DirWatch::ChangeCallback Callback() {
        return [this](bool overflow, std::vector<pulse::fs::DirNotifyEvent> events) {
            if (overflow) ++overflows;
            std::lock_guard lock(mutex);
            for (auto& event : events) names.push_back(event.name);
        };
    }
    bool Saw(const std::wstring& name) {
        std::lock_guard lock(mutex);
        for (const auto& seen : names)
            if (seen == name) return true;
        return false;
    }
};

bool Rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (MoveFileExW(from.c_str(), to.c_str(), 0)) return true;
    std::cout << "  rename error " << GetLastError() << std::endl;
    return false;
}

void Touch(const std::filesystem::path& file) { std::ofstream(file) << "x"; }
}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / (L"pulse-watch-lease-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / L"p" / L"w" / L"sub");

    {
        Recorder seen;
        pulse::fs::DirWatch watch;
        watch.Start((root / L"p" / L"w").wstring(), seen.Callback());
        Check(WaitFor([&] { return watch.Armed(); }, 5000), "the watch arms");
        Touch(root / L"p" / L"w" / L"a.txt");
        const bool first = WaitFor([&] { return seen.Saw(L"a.txt"); }, 3000);
        Touch(root / L"p" / L"w" / L"b.txt");
        const bool second = WaitFor([&] { return seen.Saw(L"b.txt"); }, 3000);
        Check(first && second, "changes keep arriving after one has broken and renewed the lease");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int before = seen.overflows.load();
        Check(Rename(root / L"p", root / L"p2"), "the folder above a watched folder can be renamed while it is watched");
        Check(WaitFor([&] { return seen.overflows.load() > before; }, 3000),
              "the watch that stepped aside reports its folder as changed");
        watch.Stop();
    }
    {
        Recorder seen;
        pulse::fs::DirWatch watch;
        watch.Start((root / L"p2" / L"w").wstring(), seen.Callback());
        Check(WaitFor([&] { return watch.Armed(); }, 5000), "a watch arms again on the renamed tree");
        Check(Rename(root / L"p2" / L"w", root / L"p2" / L"w2"), "the watched folder itself can be renamed");
        watch.Stop();
    }
    {
        pulse::fs::DirWatchSet set;
        Recorder seen;
        const std::wstring deep = (root / L"p2" / L"w2" / L"sub").wstring();
        set.Sync({deep}, [&](const std::wstring&, bool overflow, std::vector<pulse::fs::DirNotifyEvent> events) {
            seen.Callback()(overflow, std::move(events));
        });
        Check(WaitFor([&] { return set.Armed(deep); }, 5000), "a watch set arms on a folder two levels down");
        Check(Rename(root / L"p2", root / L"p3"), "a folder two levels above a watched folder can be moved");
        set.Stop();
    }

    fs::remove_all(root, error);
    std::cout << (failures ? "FAILED" : "all passed") << std::endl;
    return failures ? 1 : 0;
}
