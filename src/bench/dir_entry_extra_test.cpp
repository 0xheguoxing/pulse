// dir_entry_extra_test.cpp - DirEntry's out-of-line fields (fs_enum.h).
#include "../fs/fs_enum.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
int passed = 0, failed = 0;
void Check(bool value, const wchar_t* name) {
    std::wprintf(L"[%s] %s\n", value ? L"PASS" : L"FAIL", name);
    value ? ++passed : ++failed;
}

using pulse::fs::DirEntry;

// Returned by value, as Tab::EntryAt does.
__declspec(noinline) DirEntry Row(const std::vector<DirEntry>& rows, size_t index) {
    return index < rows.size() ? rows[index] : DirEntry{};
}

// Reuses freed heap blocks, so a dangling reference would read other data.
void Churn() {
    std::vector<std::wstring> noise(256, std::wstring(96, L'x'));
    (void)noise;
}
} // namespace

int wmain() {
    DirEntry plain;
    plain.name = L"a.txt";
    Check(plain.recycle_path.empty() && plain.link_target_size == 0, L"unset fields read as their defaults");
    plain.full_path = std::wstring();
    plain.drive_total = 0;
    Check(!plain.extra_, L"assigning a default value allocates no record");

    std::vector<DirEntry> rows(10);
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i].name = L"item" + std::to_wstring(i) + L".txt";
        rows[i].recycle_path = L"\\\\?\\C:\\$Recycle.Bin\\S-1-5-21-0\\$RPAYLOAD" + std::to_wstring(i) + L".txt";
        rows[i].full_path = L"C:\\Users\\Someone\\Documents\\folder\\item" + std::to_wstring(i) + L".txt";
        rows[i].link_target_size = 1000 + i;
    }
    Check(rows[3].extra_ && rows[3].recycle_path.ends_with(L"$RPAYLOAD3.txt"), L"a set field is stored and read back");

    const DirEntry copy = rows[4];
    rows[4].recycle_path = L"changed";
    Check(copy.recycle_path.ends_with(L"$RPAYLOAD4.txt"), L"a copied entry owns its own record");

    // The 还原 bug: a reference bound to a field of a by-value entry.
    std::vector<std::wstring> paths;
    for (int index : std::vector<int>{7}) {
        const auto& path = Row(rows, static_cast<size_t>(index)).recycle_path;
        Churn();
        if (!path.empty()) paths.push_back(path);
    }
    Check(paths.size() == 1 && paths.front().ends_with(L"$RPAYLOAD7.txt"),
          L"a reference to a temporary entry's field stays valid (recycle_path)");
    const auto& full = Row(rows, 2).full_path;
    const auto& size = Row(rows, 2).link_target_size;
    Churn();
    Check(full.ends_with(L"\\item2.txt") && size == 1002, L"the same holds for full_path and numeric fields");

    const DirEntry& named = rows[5];
    const auto& field = named.recycle_path;
    Check(&field == &named.extra_.Get().recycle_path, L"a named entry's field is a reference, not a copy");

    std::wprintf(L"== dir entry extra test: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
