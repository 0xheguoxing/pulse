#include "ai_activity_log.h"
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <mutex>

namespace pulse::app::ai {
namespace {

std::mutex& LogMutex() {
    static auto* mu = new std::mutex;   // never torn down at exit
    return *mu;
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

void AddFolder(Activity& activity, std::wstring folder) {
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    if (folder.empty() || activity.folders.size() >= 3) return;
    for (const auto& f : activity.folders)
        if (SameText(f, folder)) return;
    activity.folders.push_back(std::move(folder));
}

void AddPath(Activity& activity, const json::Value& v, bool is_folder) {
    if (!v.is_string() || v.as_string().empty()) return;
    ++activity.path_count;
    std::wstring path = json::Wide(v.as_string());
    if (!is_folder) {
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        const size_t slash = path.find_last_of(L"\\/");
        path = slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash == 2 ? 3 : slash);
    }
    AddFolder(activity, std::move(path));
}

FILETIME ToFileTime(uint64_t unix_ms) {
    const uint64_t ticks = unix_ms * 10000ull + 116444736000000000ull;
    return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}

bool LocalMonth(uint64_t unix_ms, int& year, int& month) {
    const FILETIME utc = ToFileTime(unix_ms);
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return false;
    year = st.wYear;
    month = st.wMonth;
    return true;
}

} // namespace

uint64_t UnixMillisecondsNow() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const uint64_t ticks = (uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime;
    return (ticks - 116444736000000000ull) / 10000ull;
}

void SummarizeArgs(const json::Value& args, Activity& activity) {
    static constexpr struct { const char* key; bool folder; } kKeys[] = {
        {"path", false}, {"paths", false}, {"folder", true}, {"dest_folder", true}};
    for (const auto& k : kKeys) {
        const json::Value* v = args.find(k.key);
        if (!v) continue;
        if (v->is_array()) {
            for (const auto& item : v->items()) AddPath(activity, item, k.folder);
        } else {
            AddPath(activity, *v, k.folder);
        }
    }
}

bool AppendActivity(const std::wstring& directory, const Activity& a) {
    if (directory.empty()) return false;
    int year = 0, month = 0;
    if (!LocalMonth(a.time, year, month)) return false;
    json::Value line = json::Value::Object();
    line.set("time", a.time);
    line.set("client", json::Utf8(a.client));
    line.set("tool", a.tool);
    line.set("paths", static_cast<uint64_t>(a.path_count));
    json::Value folders = json::Value::Array();
    for (const auto& f : a.folders) folders.push(json::Utf8(f));
    line.set("folders", std::move(folders));
    line.set("outcome", a.outcome);
    line.set("changed", a.changed_files);
    if (a.undo_group) line.set("undo_group", a.undo_group);
    line.set("ms", static_cast<uint64_t>(a.duration_ms));
    const std::string text = json::Serialize(line) + "\n";

    wchar_t name[32];
    swprintf_s(name, L"\\%04d-%02d.jsonl", year, month);
    std::lock_guard lock(LogMutex());
    const size_t cut = directory.find_last_of(L'\\');
    if (cut != std::wstring::npos) {
        const std::wstring parent = directory.substr(0, cut);
        const size_t up = parent.find_last_of(L'\\');
        if (up != std::wstring::npos) CreateDirectoryW(parent.substr(0, up).c_str(), nullptr);
        CreateDirectoryW(parent.c_str(), nullptr);
    }
    CreateDirectoryW(directory.c_str(), nullptr);
    const HANDLE file = CreateFileW((directory + name).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    CloseHandle(file);
    return ok;
}

void PruneActivity(const std::wstring& directory, int log_days, uint64_t now) {
    if (directory.empty() || log_days < 1) return;
    const uint64_t keep_after = now - static_cast<uint64_t>(log_days) * 86400000ull;
    int keep_year = 0, keep_month = 0;
    if (!LocalMonth(keep_after, keep_year, keep_month)) return;
    // A month file goes once its whole month lies before keep_after, i.e.
    // its month is earlier than keep_after's month.
    WIN32_FIND_DATAW data{};
    std::lock_guard lock(LogMutex());
    const HANDLE find = FindFirstFileExW((directory + L"\\*.jsonl").c_str(), FindExInfoBasic, &data,
                                         FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int year = 0, month = 0;
        wchar_t tail[8]{};
        if (wcslen(data.cFileName) != 13 || swscanf_s(data.cFileName, L"%4d-%2d%6s", &year, &month, tail,
                                                      static_cast<unsigned>(std::size(tail))) != 3 ||
            wcscmp(tail, L".jsonl") != 0 || month < 1 || month > 12)
            continue;
        if (year < keep_year || (year == keep_year && month < keep_month))
            DeleteFileW((directory + L"\\" + data.cFileName).c_str());
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

} // namespace pulse::app::ai
