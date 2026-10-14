#include "../app/app_worker.cpp"
#include <cstdio>
static HANDLE blocked, release_io, delivered;
static std::atomic<int> block_count{0}, callbacks{0};
namespace pulse::fs {
bool IsVirtualPath(const std::wstring& p) { return p.starts_with(L"pulse:"); }
bool IsUncPath(const std::wstring& p) { return p.starts_with(L"\\\\"); }
DWORD ReadReparseTag(const std::wstring&) { return 0; }
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>&, const std::atomic_bool*) {
    if (path.find(L"blocked") != std::wstring::npos) {
        ++block_count;
        SetEvent(blocked);
        WaitForSingleObject(release_io, INFINITE);
    }
}
void EnumerateRecycleBin(std::vector<DirEntry>&, RecycleBinInfo*) {}
bool QueryDirectoryIdentity(const std::wstring&, DirectoryIdentity&) { return true; }
NetSnapshotWrite BeginNetSnapshotWrite(const std::wstring&) { return {}; }
bool SaveNetSnapshot(const NetSnapshotWrite&, const SnapshotPtr&) { return true; }
}
namespace pulse::app {
std::wstring FindGitRoot(const std::wstring&) { return {}; }
void ResolveLinksInPlace(const std::wstring&, std::vector<fs::DirEntry>&, const std::function<bool()>&) {}
ScopedEntryGrouping::ScopedEntryGrouping(int, const std::wstring&) {}
ScopedEntryGrouping::~ScopedEntryGrouping() = default;
bool EntryLess(const fs::DirEntry&, const fs::DirEntry&, ui::SortColumn, ui::SortDirection) { return false; }
void SortEntriesBySize(std::vector<fs::DirEntry>&, ui::SortDirection, const FolderSizeLookup&, const std::function<void()>&) {}
void SortEntries(std::vector<fs::DirEntry>& entries, ui::SortColumn col, ui::SortDirection dir, const std::function<void()>& tick) {
    std::sort(entries.begin(), entries.end(), [&](const fs::DirEntry& a, const fs::DirEntry& b) { if (tick) tick(); return EntryLess(a, b, col, dir); });
}
}
namespace pulse::diagnostics::runtime {
void Event(const char*, std::initializer_list<Field>, Level) noexcept {}
}
static int failures = 0;
static void Check(bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
static bool Drained() {
    for (int i = 0; i < 1000 && pulse::fs::active_network_enumerations; ++i) Sleep(2);
    return pulse::fs::active_network_enumerations == 0;
}
int main() {
    using namespace pulse;
    blocked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_io = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    delivered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; SetEvent(delivered); });
        auto generation = worker.Refresh(L"\\\\server\\blocked", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(blocked, 2000) == WAIT_OBJECT_0, "WorkerPool reaches blocked enumeration");
        worker.CancelGeneration(generation);
        worker.Refresh(L"C:\\healthy", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(delivered, 1000) == WAIT_OBJECT_0, "replacement navigation completes while old provider blocked");
        const auto start = GetTickCount64();
        worker.Stop();
        Check(GetTickCount64() - start < 1000, "Stop returns while provider ignores cancellation");
        Check(callbacks == 1, "only replacement result delivered");
    }
    SetEvent(release_io);
    Check(Drained(), "provider retires safely after WorkerPool destruction");
    Check(callbacks == 1, "late completion cannot call destroyed pool");
    ResetEvent(release_io); ResetEvent(blocked); ResetEvent(delivered);
    callbacks = 0; block_count = 0;
    {
        app::WorkerPool worker;
        worker.Start([](app::WorkResult) { ++callbacks; SetEvent(delivered); });
        for (int i = 0; i < 4; ++i)
            worker.Refresh(L"\\\\server\\blocked" + std::to_wstring(i), ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto waiting = GetTickCount64();
        const unsigned expected = std::min(4u, std::max(2u, std::thread::hardware_concurrency())) - 1;
        while (block_count < static_cast<int>(expected) && GetTickCount64() - waiting < 2000) Sleep(2);
        Check(block_count == static_cast<int>(expected), "network concurrency reserves one navigation worker");
        worker.Refresh(L"C:\\healthy", ui::SortColumn::Name, ui::SortDirection::Asc);
        Check(WaitForSingleObject(delivered, 1000) == WAIT_OBJECT_0, "queued local navigation bypasses blocked UNC workers");
        const auto start = GetTickCount64();
        worker.Stop();
        Check(GetTickCount64() - start < 1000, "Stop cancels concurrent and queued generations");
    }
    SetEvent(release_io);
    Check(Drained() && callbacks == 1, "concurrent late results discarded and reclaimed");
    CloseHandle(blocked); CloseHandle(release_io); CloseHandle(delivered);
    return failures ? 1 : 0;
}
