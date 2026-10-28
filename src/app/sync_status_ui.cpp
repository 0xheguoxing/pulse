#include "sync_status_ui.h"

#include "app_internal.h"
#include "cloud_roots.h"
#include "git_status.h"
#include "../common/file_placeholder.h"
#include "../common/localization.h"
#include "sqlite3.h"
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse {
namespace {

using ui::GitMark;

// One folder's SVN state, as the worker read it.
struct SvnAnswer {
    std::wstring folder;    // GitLowerPath key of the listed folder
    std::wstring root;      // working-copy root; empty = not inside one
    long long revision = -1;
    int changed = 0;        // changed files at or below the folder
    std::unordered_map<std::wstring, GitMark> by_name;   // lowercase child name
    ULONGLONG cost = 0;
};

// Per listed folder, UI thread only.
struct SvnFolder {
    bool running = false;
    ULONGLONG last_done = 0, last_cost = 0;
    const void* seen_listing = nullptr;
    size_t seen_count = 0;
    std::shared_ptr<const SvnAnswer> answer;
    std::weak_ptr<const std::vector<fs::DirEntry>> marks_listing;
    size_t marks_count = 0;
    const SvnAnswer* marks_answer = nullptr;
    std::shared_ptr<const std::unordered_map<int, GitMark>> marks;
};

std::unordered_map<std::wstring, SvnFolder>& Folders() {
    static std::unordered_map<std::wstring, SvnFolder> folders;
    return folders;
}

std::mutex& InboxLock() {
    static std::mutex lock;
    return lock;
}
std::vector<std::shared_ptr<const SvnAnswer>>& Inbox() {
    static std::vector<std::shared_ptr<const SvnAnswer>> inbox;
    return inbox;
}

// Without the \\?\ (or \\?\UNC\) long-path prefix.
std::wstring PlainPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

std::wstring LongPath(const std::wstring& path) {
    if (path.size() < MAX_PATH - 12 || path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(n, 0)), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Highest folder that still is a folder: "C:" or "\\server\share".
bool IsVolumeTop(const std::wstring& dir) {
    if (dir.size() <= 3 && dir.size() >= 2 && dir[1] == L':') return true;
    if (dir.starts_with(L"\\\\")) {
        const size_t server = dir.find(L'\\', 2);
        return server == std::wstring::npos || dir.find(L'\\', server + 1) == std::wstring::npos;
    }
    return false;
}

// The working copy holding `folder`: the nearest folder above with .svn\wc.db
// (Subversion 1.7+ keeps a single .svn at the root).
std::wstring FindWorkingCopy(std::wstring dir) {
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    for (int depth = 0; depth < 64 && !dir.empty(); ++depth) {
        const DWORD attrs = GetFileAttributesW(LongPath(dir + L"\\.svn\\wc.db").c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return dir;
        if (IsVolumeTop(dir)) break;
        const size_t slash = dir.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash < 2) break;
        dir.resize(slash);
    }
    return {};
}

struct Statement {
    sqlite3_stmt* stmt = nullptr;
    Statement(sqlite3* db, const char* sql) {
        if (!db || sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) stmt = nullptr;
    }
    ~Statement() { if (stmt) sqlite3_finalize(stmt); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    explicit operator bool() const { return stmt != nullptr; }
    void Bind(int at, const std::string& text) {
        sqlite3_bind_text(stmt, at, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
    }
    bool Step() { return sqlite3_step(stmt) == SQLITE_ROW; }
    std::string Text(int column) const {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
        return text ? std::string(text, static_cast<size_t>(sqlite3_column_bytes(stmt, column))) : std::string();
    }
    long long Int(int column, long long missing = -1) const {
        return sqlite3_column_type(stmt, column) == SQLITE_NULL ? missing : sqlite3_column_int64(stmt, column);
    }
};

// Lowercase hex SHA-1 of a file, or empty when it could not be read.
std::string FileSha1(const std::wstring& path) {
    HANDLE file = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<unsigned char> buffer(256 * 1024);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got > 0)
            ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), got, 0));
        unsigned char digest[20]{};
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            static constexpr char kHex[] = "0123456789abcdef";
            for (unsigned char b : digest) {
                hex += kHex[b >> 4];
                hex += kHex[b & 15];
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    return hex;
}

// A versioned file differs from its pristine copy. Subversion's own test:
// recorded size and time first, the content checksum only when they disagree.
bool WorkingFileChanged(const std::wstring& path, long long recorded_size, long long recorded_time,
                        const std::string& checksum, uint64_t& hash_budget, bool& missing) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    missing = false;
    if (!GetFileAttributesExW(LongPath(path).c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = GetLastError();
        missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        return missing;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return true;   // obstructed
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (recorded_size >= 0 && size != static_cast<uint64_t>(recorded_size)) return true;
    const uint64_t ticks = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                           data.ftLastWriteTime.dwLowDateTime;
    // apr_time_t: microseconds since 1970.
    const long long micros = ticks >= 116444736000000000ull
        ? static_cast<long long>((ticks - 116444736000000000ull) / 10) : 0;
    if (recorded_time >= 0 && micros == recorded_time) return false;
    // Touched: compare the content, but never download a cloud placeholder.
    if (files::PreviewWouldRecall(data.dwFileAttributes)) return false;
    if (!checksum.starts_with("$sha1$") || size > hash_budget) return true;
    hash_budget -= size;
    const std::string hex = FileSha1(path);
    return hex.empty() || hex != checksum.substr(6);
}

void Raise(GitMark& slot, GitMark mark) { slot = app::StrongerGitMark(slot, mark); }

// Reads the folder's state from wc.db. Runs on a worker thread.
std::shared_ptr<SvnAnswer> ScanFolder(const std::wstring& key, const std::wstring& folder) {
    auto answer = std::make_shared<SvnAnswer>();
    answer->folder = key;
    const std::wstring root = FindWorkingCopy(folder);
    if (root.empty()) return answer;
    std::wstring rel_w = folder.size() > root.size() ? folder.substr(root.size() + 1) : std::wstring();
    std::replace(rel_w.begin(), rel_w.end(), L'\\', L'/');
    while (!rel_w.empty() && rel_w.back() == L'/') rel_w.pop_back();
    std::string rel = Utf8(rel_w);

    sqlite3* db = nullptr;
    const std::string db_path = Utf8(root + L"\\.svn\\wc.db");
    if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return answer;
    }
    sqlite3_busy_timeout(db, 1500);
    answer->root = root;
    {
        // wc.db keeps paths in the case they were added; the window may show another.
        Statement exact(db, "SELECT 1 FROM nodes WHERE wc_id=1 AND local_relpath=?1 LIMIT 1");
        if (exact && !rel.empty()) {
            exact.Bind(1, rel);
            if (!exact.Step()) {
                Statement any_case(db, "SELECT local_relpath FROM nodes WHERE wc_id=1 AND local_relpath=?1 COLLATE NOCASE LIMIT 1");
                if (any_case) {
                    any_case.Bind(1, rel);
                    if (any_case.Step()) rel = any_case.Text(0);
                }
            }
        }
    }
    {
        Statement revision(db, "SELECT revision FROM nodes WHERE wc_id=1 AND local_relpath=?1 AND op_depth=0");
        if (revision) {
            revision.Bind(1, rel);
            if (revision.Step()) answer->revision = revision.Int(0);
        }
    }
    // Paths strictly below the folder: ('rel/', 'rel0') since '0' follows '/'.
    const std::string range = rel.empty() ? std::string(" AND local_relpath<>''")
                                          : std::string(" AND local_relpath>?1 AND local_relpath<?2");
    const auto bind_range = [&](Statement& st) {
        if (rel.empty()) return;
        st.Bind(1, rel + "/");
        st.Bind(2, rel + "0");
    };
    const size_t skip = rel.empty() ? 0 : rel.size() + 1;
    // The listed child a path belongs to, and whether it is that child itself.
    const auto child_of = [&](const std::string& path, bool& direct) {
        const std::string sub = path.substr((std::min)(skip, path.size()));
        const size_t slash = sub.find('/');
        direct = slash == std::string::npos;
        return direct ? sub : sub.substr(0, slash);
    };
    std::unordered_map<std::string, GitMark> marks;
    // Scheduled changes (added, deleted, replaced, copied): rows above the base layer.
    std::unordered_set<std::string> layered;
    {
        Statement st(db, ("SELECT local_relpath, op_depth, presence, kind FROM nodes WHERE wc_id=1 AND op_depth>0" + range +
                          " ORDER BY local_relpath, op_depth").c_str());
        if (st) {
            bind_range(st);
            struct Top { long long depth = 0; std::string presence, kind; };
            std::unordered_map<std::string, Top> top;
            while (st.Step()) {
                Top& t = top[st.Text(0)];
                t.depth = st.Int(1, 0);
                t.presence = st.Text(2);
                t.kind = st.Text(3);
            }
            Statement base(db, "SELECT 1 FROM nodes WHERE wc_id=1 AND local_relpath=?1 AND op_depth=0 AND presence='normal'");
            for (const auto& [path, t] : top) {
                layered.insert(path);
                GitMark mark = GitMark::None;
                if (t.presence == "base-deleted") mark = GitMark::Deleted;
                else if (t.presence == "normal" || t.presence == "incomplete") mark = GitMark::Added;
                if (mark == GitMark::None) continue;
                bool direct = false;
                const std::string child = child_of(path, direct);
                if (direct && mark == GitMark::Added && base) {
                    sqlite3_reset(base.stmt);
                    base.Bind(1, path);
                    if (base.Step()) mark = GitMark::Modified;   // replaced
                }
                if (t.kind != "dir") ++answer->changed;
                Raise(marks[child], direct || mark == GitMark::Added ? mark : GitMark::Modified);
            }
        }
    }
    // Local edits: stat the versioned files, the listed folder's own first.
    {
        Statement st(db, ("SELECT local_relpath, translated_size, last_mod_time, checksum, parent_relpath=?3 AS direct"
                          " FROM nodes WHERE wc_id=1 AND op_depth=0 AND presence='normal' AND kind='file'" + range +
                          " ORDER BY direct DESC").c_str());
        if (st) {
            bind_range(st);
            st.Bind(3, rel);
            int stat_budget = 5000;
            uint64_t hash_budget = uint64_t{128} << 20;
            while (stat_budget > 0 && st.Step()) {
                const std::string path = st.Text(0);
                if (layered.contains(path)) continue;
                --stat_budget;
                std::wstring full = root + L"\\" + app::GitUtf8ToWide(path);
                std::replace(full.begin(), full.end(), L'/', L'\\');
                bool missing = false;
                if (!WorkingFileChanged(full, st.Int(1), st.Int(2), st.Text(3), hash_budget, missing)) continue;
                bool direct = false;
                const std::string child = child_of(path, direct);
                ++answer->changed;
                Raise(marks[child], missing && direct ? GitMark::Deleted : GitMark::Modified);
            }
        }
    }
    // Conflicts (1.8+ keeps them in conflict_data; 1.7 in the older columns).
    {
        Statement st(db, ("SELECT local_relpath FROM actual_node WHERE wc_id=1 AND conflict_data IS NOT NULL" + range).c_str());
        Statement old(st ? nullptr : db,
            ("SELECT local_relpath FROM actual_node WHERE wc_id=1 AND (conflict_old IS NOT NULL OR conflict_new IS NOT NULL"
             " OR conflict_working IS NOT NULL OR prop_reject IS NOT NULL OR tree_conflict_data IS NOT NULL)" + range).c_str());
        Statement& use = st ? st : old;
        if (use) {
            bind_range(use);
            while (use.Step()) {
                bool direct = false;
                ++answer->changed;
                Raise(marks[child_of(use.Text(0), direct)], GitMark::Conflict);
            }
        }
    }
    // Locks this working copy holds, on the listed files.
    {
        Statement use(db, "SELECT n.local_relpath FROM lock l JOIN nodes n ON n.wc_id=1 AND n.op_depth=0"
                          " AND n.repos_id=l.repos_id AND n.repos_path=l.repos_relpath WHERE n.parent_relpath=?1");
        if (use) {
            use.Bind(1, rel);
            while (use.Step()) {
                bool direct = false;
                Raise(marks[child_of(use.Text(0), direct)], GitMark::Locked);
            }
        }
    }
    sqlite3_close(db);
    for (const auto& [child, mark] : marks)
        if (mark != GitMark::None) answer->by_name[app::GitLowerPath(app::GitUtf8ToWide(child))] = mark;
    return answer;
}

void TakeAnswers() {
    std::vector<std::shared_ptr<const SvnAnswer>> answers;
    {
        std::lock_guard<std::mutex> hold(InboxLock());
        answers.swap(Inbox());
    }
    for (auto& answer : answers) {
        SvnFolder& state = Folders()[answer->folder];
        state.running = false;
        state.last_done = GetTickCount64();
        state.last_cost = answer->cost;
        state.answer = std::move(answer);
    }
}

void RequestScan(HWND hwnd, const std::wstring& key, const std::wstring& folder) {
    SvnFolder& state = Folders()[key];
    if (state.running) return;
    state.running = true;
    std::thread([hwnd, key, folder] {
        const ULONGLONG start = GetTickCount64();
        auto answer = ScanFolder(key, folder);
        answer->cost = GetTickCount64() - start;
        {
            std::lock_guard<std::mutex> hold(InboxLock());
            Inbox().push_back(std::move(answer));
        }
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }).detach();
}

// --- OneDrive: the shell's own sync state per item -------------------------

// One folder's PKEY_StorageProviderState values, as the worker read them.
struct CloudAnswer {
    std::wstring folder;
    std::unordered_map<std::wstring, uint8_t> by_name;   // lowercase child name
    bool busy = false;   // something is waiting or transferring: ask again soon
    ULONGLONG cost = 0;
};

struct CloudFolder {
    bool running = false;
    ULONGLONG last_done = 0, last_cost = 0;
    const void* seen_listing = nullptr;
    size_t seen_count = 0;
    std::shared_ptr<const CloudAnswer> answer;
    std::weak_ptr<const std::vector<fs::DirEntry>> states_listing;
    size_t states_count = 0;
    const CloudAnswer* states_answer = nullptr;
    std::shared_ptr<const std::unordered_map<int, uint8_t>> states;
};

std::unordered_map<std::wstring, CloudFolder>& CloudFolders() {
    static std::unordered_map<std::wstring, CloudFolder> folders;
    return folders;
}
std::vector<std::shared_ptr<const CloudAnswer>>& CloudInbox() {
    static std::vector<std::shared_ptr<const CloudAnswer>> inbox;
    return inbox;
}

// System.StorageProviderState ({E77E90DF-6271-4F5B-834F-2DD1F245DDA4}, 3).
constexpr PROPERTYKEY kStorageProviderState = {
    {0xE77E90DF, 0x6271, 0x4F5B, {0x83, 0x4F, 0x2D, 0xD1, 0xF2, 0x45, 0xDD, 0xA4}}, 3};
constexpr bool CloudStateBusy(uint8_t state) { return state == 4 || state == 5 || state == 6 || state == 10; }

// Test fixture (PULSE_TEST_DATA_DIR only): "<name>|<state>" lines in the
// folder's .pulse_sync_states stand in for a sync engine.
void ReadTestCloudStates(const std::wstring& folder, CloudAnswer& answer) {
    if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) == 0) return;
    HANDLE file = CreateFileW(LongPath(folder + L"\\.pulse_sync_states").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string bytes(4096, '\0');
    DWORD got = 0;
    ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
    CloseHandle(file);
    bytes.resize(got);
    size_t at = 0;
    while (at < bytes.size()) {
        size_t end = bytes.find('\n', at);
        if (end == std::string::npos) end = bytes.size();
        std::string line = bytes.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t bar = line.rfind('|');
        if (bar == std::string::npos || bar == 0) continue;
        const int state = atoi(line.c_str() + bar + 1);
        if (state <= 0 || state > 10) continue;
        answer.by_name[app::GitLowerPath(app::GitUtf8ToWide(line.substr(0, bar)))] = static_cast<uint8_t>(state);
        answer.busy = answer.busy || CloudStateBusy(static_cast<uint8_t>(state));
    }
}

// Reads each child's sync state through the folder's shell view of it (the
// cloud-files property handler). Runs on a worker thread.
std::shared_ptr<CloudAnswer> ScanCloudFolder(const std::wstring& key, const std::wstring& folder) {
    auto answer = std::make_shared<CloudAnswer>();
    answer->folder = key;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // The handler only reports placeholders this thread is allowed to see.
    using SetThreadMode = CHAR(NTAPI*)(CHAR);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto set = reinterpret_cast<SetThreadMode>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlSetThreadPlaceholderCompatibilityMode"))))
            set(2);   // PHCM_EXPOSE_PLACEHOLDERS
    }
    IShellItem* item = nullptr;
    IShellFolder2* shell_folder = nullptr;
    IEnumIDList* children = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
        SUCCEEDED(item->BindToHandler(nullptr, BHID_SFObject, IID_PPV_ARGS(&shell_folder))) &&
        shell_folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN |
                                               SHCONTF_INCLUDESUPERHIDDEN, &children) == S_OK && children) {
        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        for (int budget = 5000; budget > 0 && children->Next(1, &child, &fetched) == S_OK; --budget) {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(shell_folder->GetDetailsEx(child, &kStorageProviderState, &value)) && value.vt == VT_UI4 &&
                value.ulVal > 0 && value.ulVal <= 10) {
                STRRET name{};
                PWSTR text = nullptr;
                if (SUCCEEDED(shell_folder->GetDisplayNameOf(child, SHGDN_INFOLDER | SHGDN_FORPARSING, &name)) &&
                    SUCCEEDED(StrRetToStrW(&name, child, &text)) && text) {
                    const auto state = static_cast<uint8_t>(value.ulVal);
                    answer->by_name[app::GitLowerPath(text)] = state;
                    answer->busy = answer->busy || CloudStateBusy(state);
                }
                CoTaskMemFree(text);
            }
            VariantClear(&value);
            CoTaskMemFree(child);
        }
    }
    if (children) children->Release();
    if (shell_folder) shell_folder->Release();
    if (item) item->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    ReadTestCloudStates(folder, *answer);
    return answer;
}

void TakeCloudAnswers() {
    std::vector<std::shared_ptr<const CloudAnswer>> answers;
    {
        std::lock_guard<std::mutex> hold(InboxLock());
        answers.swap(CloudInbox());
    }
    for (auto& answer : answers) {
        CloudFolder& state = CloudFolders()[answer->folder];
        state.running = false;
        state.last_done = GetTickCount64();
        state.last_cost = answer->cost;
        state.answer = std::move(answer);
    }
}

void RequestCloudScan(HWND hwnd, const std::wstring& key, const std::wstring& folder) {
    CloudFolder& state = CloudFolders()[key];
    if (state.running) return;
    state.running = true;
    std::thread([hwnd, key, folder] {
        const ULONGLONG start = GetTickCount64();
        auto answer = ScanCloudFolder(key, folder);
        answer->cost = GetTickCount64() - start;
        const bool busy = answer->busy;
        {
            std::lock_guard<std::mutex> hold(InboxLock());
            CloudInbox().push_back(std::move(answer));
        }
        if (!hwnd) return;
        InvalidateRect(hwnd, nullptr, FALSE);
        // Syncing: wake the window again so it asks once more (FillSyncPane).
        if (busy) {
            Sleep(1600);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }).detach();
}

// Shell sync states for the listed folder's own rows (pane.onedrive_states).
void FillCloudStates(AppState& s, const app::Tab& tab, const std::wstring& folder, ui::PaneViewModel& pane) {
    TakeCloudAnswers();
    auto& folders = CloudFolders();
    if (folders.size() > 64) {
        for (auto it = folders.begin(); it != folders.end();) it = it->second.running ? std::next(it) : folders.erase(it);
    }
    const std::wstring key = app::GitLowerPath(folder);
    CloudFolder& state = folders[key];
    const ULONGLONG now = GetTickCount64();
    const bool listing_changed = state.seen_listing != tab.snapshot.get() || state.seen_count != tab.EntryCount();
    // Syncing: soon again (slow folders wait longer); otherwise now and then.
    const ULONGLONG stale_after = state.answer && state.answer->busy
        ? (std::max<ULONGLONG>)(1500, state.last_cost * 4) : (std::max<ULONGLONG>)(30000, state.last_cost * 20);
    if (!state.running && (state.last_done == 0 || listing_changed || now - state.last_done >= stale_after)) {
        state.seen_listing = tab.snapshot.get();
        state.seen_count = tab.EntryCount();
        RequestCloudScan(s.hwnd, key, folder);
    }
    if (!state.answer || state.answer->by_name.empty() || !tab.snapshot) return;
    if (state.states && state.states_answer == state.answer.get() && state.states_count == tab.EntryCount() &&
        !state.states_listing.expired() && state.states_listing.lock() == tab.snapshot) {
        pane.onedrive_states = state.states;
        return;
    }
    auto states = std::make_shared<std::unordered_map<int, uint8_t>>();
    const size_t count = tab.EntryCount();
    for (size_t i = 0; i < count; ++i) {
        const fs::DirEntry entry = tab.EntryAt(i);
        if (!entry.full_path.empty() && app::GitLowerPath(PlainPath(fs::ParentPath(entry.full_path))) != key) continue;
        const auto found = state.answer->by_name.find(app::GitLowerPath(entry.name));
        if (found != state.answer->by_name.end()) (*states)[static_cast<int>(i)] = found->second;
    }
    state.states_listing = tab.snapshot;
    state.states_count = count;
    state.states_answer = state.answer.get();
    state.states = std::move(states);
    pane.onedrive_states = state.states;
}

bool UnderOneDrive(const AppState& s, const std::wstring& folder) {
    for (const auto& entry : s.sidebar.cloud) {
        std::wstring root = PlainPath(entry.path);
        while (!root.empty() && root.back() == L'\\') root.pop_back();
        if (root.empty() || folder.size() < root.size()) continue;
        if (_wcsnicmp(folder.c_str(), root.c_str(), root.size()) != 0) continue;
        if (folder.size() == root.size() || folder[root.size()] == L'\\') return true;
    }
    return false;
}

}  // namespace

void FillSyncPane(AppState& s, const app::Tab& tab, ui::PaneViewModel& pane) {
    pane.onedrive_status = false;
    if (tab.current_path.empty() || fs::IsVirtualPath(tab.current_path)) return;
    const std::wstring folder = PlainPath(tab.current_path);
    // Under a Cloud Files sync root (OneDrive Files On-Demand included) the
    // 状态 column shows the same states (PaneViewModel::cloud_status_column), so
    // the name-column mark and its shell scan stay for folders without it.
    if (s.appPrefs.sync_onedrive && !app::IsUnderCloudFilesRoot(tab.current_path))
        pane.onedrive_status = UnderOneDrive(s, folder);
    if (pane.onedrive_status) FillCloudStates(s, tab, folder, pane);
    if (!s.appPrefs.sync_svn || !tab.git_root.empty() || (pane.git_marks && !pane.git_marks->empty())) return;
    TakeAnswers();
    auto& folders = Folders();
    if (folders.size() > 256) {
        for (auto it = folders.begin(); it != folders.end();) it = it->second.running ? std::next(it) : folders.erase(it);
    }
    const std::wstring key = app::GitLowerPath(folder);
    SvnFolder& state = folders[key];
    // Inside a working copy: ask again when the listing changed (a save, an
    // update) or the answer is old; slow ones wait longer. Outside: rarely.
    const ULONGLONG now = GetTickCount64();
    const bool in_copy = state.answer && !state.answer->root.empty();
    const bool listing_changed = state.seen_listing != tab.snapshot.get() || state.seen_count != tab.EntryCount();
    const ULONGLONG stale_after = in_copy ? (std::max<ULONGLONG>)(5000, state.last_cost * 10) : 60000;
    const ULONGLONG min_gap = in_copy ? 0 : 10000;
    if (!state.running && (state.last_done == 0 ||
                           (now - state.last_done >= min_gap && (listing_changed || now - state.last_done > stale_after)))) {
        state.seen_listing = tab.snapshot.get();
        state.seen_count = tab.EntryCount();
        RequestScan(s.hwnd, key, folder);
    }
    if (!in_copy || state.answer->by_name.empty() || !tab.snapshot) return;
    if (state.marks && state.marks_answer == state.answer.get() && state.marks_count == tab.EntryCount() &&
        !state.marks_listing.expired() && state.marks_listing.lock() == tab.snapshot) {
        pane.git_marks = state.marks;
        return;
    }
    auto marks = std::make_shared<std::unordered_map<int, GitMark>>();
    const size_t count = tab.EntryCount();
    for (size_t i = 0; i < count; ++i) {
        const fs::DirEntry entry = tab.EntryAt(i);
        if (entry.name == L".svn") continue;
        // Rows of an expanded subfolder (details tree) are not this folder's children.
        if (!entry.full_path.empty() && app::GitLowerPath(PlainPath(fs::ParentPath(entry.full_path))) != key) continue;
        const auto found = state.answer->by_name.find(app::GitLowerPath(entry.name));
        if (found != state.answer->by_name.end()) (*marks)[static_cast<int>(i)] = found->second;
    }
    state.marks_listing = tab.snapshot;
    state.marks_count = count;
    state.marks_answer = state.answer.get();
    state.marks = std::move(marks);
    pane.git_marks = state.marks;
}

void AppendSvnStatusText(AppState& s, const app::Tab& tab, std::wstring& text) {
    if (!s.appPrefs.sync_svn || !tab.git_root.empty() || tab.current_path.empty() ||
        fs::IsVirtualPath(tab.current_path))
        return;
    const auto found = Folders().find(app::GitLowerPath(PlainPath(tab.current_path)));
    if (found == Folders().end() || !found->second.answer || found->second.answer->root.empty()) return;
    const SvnAnswer& answer = *found->second.answer;
    if (!text.empty()) text += L"  \u00B7  ";
    text += L"SVN";
    if (answer.revision >= 0) text += L" r" + std::to_wstring(answer.revision);
    if (answer.changed > 0) {
        wchar_t changes[128]{};
        swprintf_s(changes, l10n::Get(l10n::StringId::GitChanges).c_str(), answer.changed);
        text += L"  \u00B7  ";
        text += changes;
    }
}

}  // namespace pulse
