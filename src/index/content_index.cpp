#include "content_sort_sql.h"
#include "../fs/drive_kind.h"
#include "../fs/cloud_state.h"
#include "content_config_storage.h"
#include "content_task_search.h"
#include "content_index.h"
#include "search_trace.h"
#include "index_feed.h"
#include "content_scope.h"
#include "document_reader.h"
#include "document_protocol.h"
#include "content_search_protocol.h"
#include "../fs/fs_watch.h"
#include "../../third_party/sqlite/sqlite3.h"
#include "../common/current_user_security.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>
#include <set>
#include <array>
#include <deque>
#include <map>
#include <future>
#include <optional>
#include <winioctl.h>

namespace pulse::index {
using namespace config_storage;
namespace {
struct QueryMatch {
    std::optional<ContentHit> hit;
    LONGLONG ticks = 0;
};
class QueryMatchWorkers {
public:
    QueryMatchWorkers() {
        const unsigned count = (std::min)(4u, (std::max)(1u, std::thread::hardware_concurrency()));
        try {
            for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] {
            for (;;) {
                std::packaged_task<QueryMatch()> task;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, [&] { return stopping_ || !tasks_.empty(); });
                    if (stopping_) return;
                    task = std::move(tasks_.front()); tasks_.pop_front();
                }
                task();
            }
            });
        } catch (...) {
            Stop();
            throw;
        }
    }
    ~QueryMatchWorkers() { Stop(); }
    std::future<QueryMatch> Submit(std::packaged_task<QueryMatch()> task) {
        auto result = task.get_future();
        { std::lock_guard lock(mutex_); tasks_.push_back(std::move(task)); }
        ready_.notify_one(); return result;
    }
    std::atomic<bool> cancelled{false};
private:
    void Stop() {
        cancelled = true;
        { std::lock_guard lock(mutex_); stopping_ = true; tasks_.clear(); }
        ready_.notify_all();
        for (auto& worker : workers_) if (worker.joinable()) worker.join();
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::packaged_task<QueryMatch()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};
bool InternalContentPath(const std::wstring& path) {
    std::wstring profile(256, L'\0');
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", profile.data(), static_cast<DWORD>(profile.size()));
    if (n >= profile.size()) {
        profile.resize(n);
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", profile.data(), static_cast<DWORD>(profile.size()));
    }
    if (!n || n >= profile.size()) return false;
    profile.resize(n);
    return ContentPathUnder(ContentScopeKey(path), ContentScopeKey(profile + L"\\Pulse"));
}
bool ExcludedContentPath(const ContentIndexConfig& config, const std::wstring& path) {
    return InternalContentPath(path) || ContentPathExcluded(config, path);
}
std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring Wide(const char* value, int n) {
    if (!value || n <= 0) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, n, nullptr, 0);
    std::wstring out(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, n, out.data(), count);
    return out;
}
std::wstring Column(sqlite3_stmt* s, int col) {
    const auto* p = static_cast<const wchar_t*>(sqlite3_column_text16(s, col));
    return p ? std::wstring(p, static_cast<size_t>(sqlite3_column_bytes16(s, col)) / 2) : std::wstring{};
}
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(sqlite3* db, const char* sql) { sqlite3_prepare_v2(db, sql, -1, &p, nullptr); }
    ~Statement() { sqlite3_finalize(p); }
    void Text(int n, std::wstring_view s) { sqlite3_bind_text16(p, n, s.data(), static_cast<int>(s.size() * 2), SQLITE_TRANSIENT); }
    void Int(int n, uint64_t v) { sqlite3_bind_int64(p, n, static_cast<sqlite3_int64>(v)); }
    bool Done() { return p && sqlite3_step(p) == SQLITE_DONE; }
};
bool Exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }
std::string Gram(wchar_t a, wchar_t b = 0, bool pair = false) {
    char buf[16]{};
    if (pair) sprintf_s(buf, "b%04x%04x", static_cast<unsigned>(towlower(a)), static_cast<unsigned>(towlower(b)));
    else sprintf_s(buf, "u%04x", static_cast<unsigned>(towlower(a)));
    return buf;
}
// Unicode UTF-16 units deliberately mirror the existing literal matcher,
// including its casing and supplementary-character behavior. Query grams are
// ASCII identifiers; punctuation and CJK receive exactly the same indexing.
struct GramTokenizer {
    std::array<uint32_t, 65536> unigram_epoch{};
    std::array<uint32_t, 65536> bigram_epoch{};
    std::array<uint32_t, 65536> bigram_key{};
    uint32_t epoch = 0;
};
int TokenCreate(void*, const char**, int, Fts5Tokenizer** out) {
    auto* tokenizer = new (std::nothrow) GramTokenizer;
    *out = reinterpret_cast<Fts5Tokenizer*>(tokenizer); return tokenizer ? SQLITE_OK : SQLITE_NOMEM;
}
void TokenDelete(Fts5Tokenizer* p) { delete reinterpret_cast<GramTokenizer*>(p); }
int Tokenize(Fts5Tokenizer* raw, void* context, int flags, const char* data, int size,
             int (*token)(void*, int, const char*, int, int, int)) {
    if (flags & FTS5_TOKENIZE_QUERY) return token(context, 0, data, size, 0, size);
    auto& cache = *reinterpret_cast<GramTokenizer*>(raw);
    if (++cache.epoch == 0) { cache.unigram_epoch.fill(0); cache.bigram_epoch.fill(0); ++cache.epoch; }
    const auto text = Wide(data, size);
    uint16_t previous = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto current = static_cast<uint16_t>(towlower(text[i]));
        if (cache.unigram_epoch[current] != cache.epoch) {
            cache.unigram_epoch[current] = cache.epoch;
            const auto gram = Gram(current);
            const int rc = token(context, 0, gram.data(), static_cast<int>(gram.size()), 0, size);
            if (rc != SQLITE_OK) return rc;
        }
        if (i) {
            const uint32_t key = (static_cast<uint32_t>(previous) << 16) | current;
            const uint32_t slot = (key * 2654435761u) >> 16;
            // Exact direct-mapped dedup is bounded (<1 MiB). Hash collisions
            // emit again, so this optimization can never suppress a new gram.
            if (cache.bigram_epoch[slot] != cache.epoch || cache.bigram_key[slot] != key) {
                cache.bigram_epoch[slot] = cache.epoch; cache.bigram_key[slot] = key;
                const auto gram = Gram(previous, current, true);
                const int rc = token(context, 0, gram.data(), static_cast<int>(gram.size()), 0, size);
                if (rc != SQLITE_OK) return rc;
            }
        }
        previous = current;
    }
    return SQLITE_OK;
}

bool RegisterTokenizer(sqlite3* db) {
    fts5_api* api = nullptr;
    Statement s(db, "SELECT fts5(?1)");
    if (!s.p) return false;
    sqlite3_bind_pointer(s.p, 1, &api, "fts5_api_ptr", nullptr);
    sqlite3_step(s.p);
    fts5_tokenizer tokenizer{TokenCreate, TokenDelete, Tokenize};
    return api && api->xCreateTokenizer(api, "pulsegram", nullptr, &tokenizer, nullptr) == SQLITE_OK;
}
bool Open(const std::wstring& path, sqlite3** db, bool readonly) {
    const auto name = Utf8(path);
    if (name.empty() || sqlite3_open_v2(name.c_str(), db, readonly ? SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) return false;
    sqlite3_busy_timeout(*db, 2000);
    sqlite3_create_collation(*db, "PULSE_ORDINAL", SQLITE_UTF16, nullptr, OrdinalCollation);
    sqlite3_create_function_v2(*db, "pulse_filename", 1, SQLITE_UTF16 | SQLITE_DETERMINISTIC, nullptr, SqlFilename, nullptr, nullptr, nullptr);
    sqlite3_create_function_v2(*db,"pulse_extension",1,SQLITE_UTF16|SQLITE_DETERMINISTIC,nullptr,SqlExtension,nullptr,nullptr,nullptr);
    return RegisterTokenizer(*db) && Exec(*db, readonly ? "PRAGMA cache_size=-8192; PRAGMA temp_store=FILE;" : "PRAGMA cache_size=-32768; PRAGMA temp_store=FILE;");
}
std::wstring LongPath(const std::wstring& p) {
    return p.starts_with(L"\\\\?\\") ? p : L"\\\\?\\" + p;
}
std::wstring Canonical(std::wstring p) {
    if (p.starts_with(L"\\\\?\\")) p.erase(0, 4);
    if (p.size() < 3 || p[1] != L':' || (p[2] != L'\\' && p[2] != L'/')) return {};
    wchar_t full[32768]{};
    const DWORD n = GetFullPathNameW(p.c_str(), ARRAYSIZE(full), full, nullptr);
    if (!n || n >= ARRAYSIZE(full)) return {};
    p.assign(full, n);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    for (size_t end = 3; end <= p.size(); ++end) {
        if (end != p.size() && p[end] != L'\\') continue;
        const DWORD attr = GetFileAttributesW(LongPath(p.substr(0, end)).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    }
    const auto drive = p.substr(0, 3);
    if (::pulse::fs::IsRemoteLikeDrive(drive)) return {};  // mapped drive or cloud mount
    return p;
}
bool Same(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool Under(std::wstring_view path, std::wstring_view root, bool recursive = true) {
    if (root.empty()) return true;
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.remove_suffix(1);
    if (path.size() < root.size() || !Same(path.substr(0, root.size()), root)) return false;
    if (path.size() == root.size()) return true;
    const size_t next = root.back() == L'\\' ? root.size() : root.size() + 1;
    if (root.back() != L'\\' && path[root.size()] != L'\\') return false;
    return recursive || path.find(L'\\', next) == std::wstring_view::npos;
}
bool Accessible(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(LongPath(path).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & FILE_ATTRIBUTE_REPARSE_POINT) || text::IsOfflinePlaceholder(attr)) return false;
    HANDLE h = CreateFileW(LongPath(path).c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h); return true;
}
std::wstring FileVersion(const std::wstring& path) {
    HANDLE h = CreateFileW(LongPath(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION id{}; FILE_BASIC_INFO basic{};
    const bool ok = GetFileInformationByHandle(h, &id) && GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic));
    CloseHandle(h);
    if (!ok) return {};
    auto version = std::to_wstring(id.dwVolumeSerialNumber) + L":" + std::to_wstring(id.nFileIndexHigh) + L":" +
        std::to_wstring(id.nFileIndexLow) + L":" + std::to_wstring(basic.ChangeTime.QuadPart);
    const auto dot = path.find_last_of(L'.');
    // Re-extract documents cached by older parsers, including empty Office
    // bodies, without rebuilding unrelated text or user settings.
    if (dot != std::wstring::npos && IsExtractedDocumentExtension(std::wstring_view(path).substr(dot)))
        version += L":extract2";
    return version;
}
unsigned DiskReaders(const std::wstring& path) {
    const auto volume = L"\\\\.\\" + path.substr(0, 2);
    HANDLE h = CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    STORAGE_PROPERTY_QUERY query{}; query.PropertyId = StorageDeviceSeekPenaltyProperty; query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR result{}; DWORD returned = 0;
    const bool ssd = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &result, sizeof(result), &returned, nullptr) &&
        returned >= sizeof(result) && !result.IncursSeekPenalty;
    CloseHandle(h); return ssd ? 4u : 1u;
}uint64_t Stamp(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
std::string CandidateQuery(const ContentSearchRequest& r) {
    std::vector<std::wstring> terms = r.needles;
    if (r.match_mode == ContentMatchMode::Phrase) {
        std::wstring phrase = r.needle;
        if (phrase.empty()) for (const auto& t : terms) { if (!phrase.empty()) phrase += L' '; phrase += t; }
        terms = {phrase};
    } else if (terms.empty()) {
        size_t start = 0;
        while (start < r.needle.size()) {
            while (start < r.needle.size() && iswspace(r.needle[start])) ++start;
            size_t end = start;
            while (end < r.needle.size() && !iswspace(r.needle[end])) ++end;
            if (end > start) terms.push_back(r.needle.substr(start, end - start));
            start = end;
        }
    }
    std::string query;
    for (const auto& term : terms) {
        if (term.empty()) continue;
        if (!query.empty()) query += r.match_mode == ContentMatchMode::AnyWord ? " OR " : " AND ";
        query += '(';
        // Eight distributed grams bound query cost without sacrificing recall.
        const size_t count = (std::min)(term.size() > 1 ? term.size() - 1 : size_t{1}, size_t{8});
        for (size_t i = 0; i < count; ++i) {
            if (i) query += " AND ";
            const size_t pos = count > 1 ? i * (term.size() - 2) / (count - 1) : 0;
            query += '"'; query += term.size() == 1 ? Gram(term[0]) : Gram(term[pos], term[pos + 1], true); query += '"';
        }
        query += ')';
    }
    return query;
}
ContentIndexConfig ReadConfig(sqlite3* db) {
    ContentIndexConfig c;
    Statement roots(db, "SELECT path,encoding FROM roots ORDER BY path");
    while (roots.p && sqlite3_step(roots.p) == SQLITE_ROW)
        c.roots.push_back({Column(roots.p, 0), static_cast<text::Encoding>(sqlite3_column_int(roots.p, 1))});
    Statement settings(db, "SELECT value FROM settings WHERE name='maximum_file_bytes'");
    const bool configured = settings.p && sqlite3_step(settings.p) == SQLITE_ROW;
    if (configured) c.maximum_file_bytes = static_cast<uint64_t>(sqlite3_column_int64(settings.p, 0));
    Statement documents(db, "SELECT value FROM settings WHERE name='maximum_document_bytes'");
    if (documents.p && sqlite3_step(documents.p) == SQLITE_ROW)
        c.maximum_document_bytes = static_cast<uint64_t>(sqlite3_column_int64(documents.p, 0));
    Statement excluded(db, "SELECT value FROM exclusions");
    if (excluded.p && configured) {
        c.excluded_directories.clear();
        while (sqlite3_step(excluded.p) == SQLITE_ROW) {
            auto value = Column(excluded.p, 0);
            if (value.find_first_of(L"\\/:") != std::wstring::npos) c.excluded_paths.push_back(std::move(value));
            else c.excluded_directories.push_back(std::move(value));
        }
    }
    Statement shared(db, "SELECT value FROM settings WHERE name='shared_scope'");
    c.shared_scope = shared.p && sqlite3_step(shared.p) == SQLITE_ROW && sqlite3_column_int(shared.p, 0) != 0;
    Statement encoding(db, "SELECT value FROM settings WHERE name='default_encoding'");
    if (encoding.p && sqlite3_step(encoding.p) == SQLITE_ROW)
        c.default_encoding = static_cast<text::Encoding>(sqlite3_column_int(encoding.p, 0));
    return c;
}
}
namespace {
struct ReadSlot {
    std::array<HANDLE, 4> slots{};
    HANDLE acquired = nullptr;
    ReadSlot(const std::wstring& path, const std::atomic<bool>* cancel = nullptr) {
        for (size_t i = 0; i < slots.size(); ++i) slots[i] = NamedMutex(path, (L"reader" + std::to_wstring(i)).c_str());
        const auto deadline = GetTickCount64() + 5000;
        while ((!cancel || !cancel->load()) && GetTickCount64() < deadline) {
            for (HANDLE slot : slots) {
                if (!slot) continue;
                const DWORD wait = WaitForSingleObject(slot, 0);
                if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) { acquired = slot; return; }
            }
            Sleep(5);
        }
    }
    ~ReadSlot() { if (acquired) ReleaseMutex(acquired); for (HANDLE slot : slots) if (slot) CloseHandle(slot); }
};
}
bool IsInternalContentPath(const std::wstring& path) {
    if (InternalContentPath(path)) return true;
    wchar_t directory[32768]{};
    const auto n = GetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", directory, ARRAYSIZE(directory));
    return n && n < ARRAYSIZE(directory) && ContentPathUnder(ContentScopeKey(path), ContentScopeKey(std::wstring(directory, n)));
}
struct ContentIndex::Impl {
    std::wstring path;
    HANDLE writer_mutex = nullptr;
    HANDLE pause_event = nullptr;
    HANDLE rebuild_event = nullptr;
    std::array<HANDLE, 4> query_slots{};
    bool owns_writer = false;
    std::vector<uint8_t> last_status_payload;
    mutable std::mutex mu;
    std::condition_variable cv;
    ContentIndexConfig config;
    ContentIndexStatus status;
    std::wstring recoverable_error_path;
    bool stopping = false;
    bool dirty = false;
    bool reset = false;
    bool config_changed = false;
    std::atomic<unsigned> queries{0};
    std::thread worker;
    uint64_t epoch = 0;
    sqlite3* writer_db = nullptr;
    std::atomic<uint64_t> published_sequence{0};
    struct ReadJob {
        ContentIndexRoot root;
        std::wstring path;
        uint64_t maximum = 0;
        uint64_t reservation = 0;
        uint64_t epoch = 0;
    };
    struct ReadResult {
        ReadJob job;
        std::wstring body;
        std::wstring version;
        uint64_t bytes = 0;
        uint64_t modified = 0;
        DWORD error = 0;
    };
    std::mutex queue_mu;
    std::condition_variable queue_cv;
    std::deque<ReadJob> read_jobs;
    std::deque<ReadResult> write_queue;
    std::vector<std::thread> readers;
    bool readers_stop = false;
    uint64_t reserved_bytes = 0;
    size_t outstanding = 0;
    unsigned reader_limit = 1;
    ULONGLONG last_count_refresh = 0;
    static constexpr uint64_t kQueueBytes = 128ull * 1024 * 1024;
    std::vector<std::unique_ptr<fs::DirWatch>> watches;
    HANDLE feed_stop = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread feed_thread;
    bool feed_ready = false;
    uint64_t feed_epoch = 0;
    struct Change { ContentIndexRoot root; bool added = false; ULONGLONG due = 0; std::wstring old_path; };
    std::map<std::wstring, Change> changes;
    struct Retry { Change change; ULONGLONG due = 0; unsigned attempts = 0; };
    std::map<std::wstring, Retry> retries;
    const ContentAgentMode mode;
    const bool read_only;
    explicit Impl(std::wstring value, ContentAgentMode selected) : path(value.empty() ? DatabasePath() : std::move(value)), mode(selected), read_only(selected != ContentAgentMode::LegacyWriter) {
        writer_mutex = NamedMutex(path, L"writer");
        pause_event = NamedEvent(path, L"paused", true);
        rebuild_event = NamedEvent(path, L"rebuild", false);
        for (size_t i = 0; i < query_slots.size(); ++i) query_slots[i] = NamedMutex(path, (L"reader" + std::to_wstring(i)).c_str());
        if (mode == ContentAgentMode::Instant) {
            if (!ReadConfigFile(path, config)) {
                sqlite3* db = nullptr;
                if (Open(path, &db, true)) config = ReadConfig(db);
                if (db) sqlite3_close(db);
            }
        } else {
            status.pending_files = 1; worker = std::thread([this] { Run(); });
        }
    }
    ~Impl() {
        { std::lock_guard lock(mu); stopping = true; } cv.notify_all();
        SetEvent(feed_stop);
        if (worker.joinable()) worker.join();
        if (feed_thread.joinable()) feed_thread.join();
        CloseHandle(feed_stop);
        watches.clear();
        for (HANDLE h : query_slots) if (h) CloseHandle(h);
        if (writer_mutex) CloseHandle(writer_mutex);
        if (pause_event) CloseHandle(pause_event);
        if (rebuild_event) CloseHandle(rebuild_event);
    }
    bool OtherQueryActive() const {
        for (HANDLE h : query_slots) {
            if (!h) continue;
            const DWORD wait = WaitForSingleObject(h, 0);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return true;
            ReleaseMutex(h);
        }
        return false;
    }
    bool ContinueIndexing() {
        std::unique_lock lock(mu);
        for (;;) {
            status.paused = pause_event && WaitForSingleObject(pause_event, 0) == WAIT_OBJECT_0;
            if (stopping || config_changed || reset) return false;
            if (!status.paused) return true;
            cv.wait_for(lock, std::chrono::milliseconds(25));
        }
    }
    void PersistStatus() {
        ContentIndexStatus copy;
        { std::lock_guard lock(mu); copy = status; }
        ipc::PayloadWriter w; content::PutStatus(w, copy);
        if (w.data() != last_status_payload && WriteSidecar(path + L".status", 2, w.data())) last_status_payload = w.data();
    }
    void Run() {
        if (!writer_mutex || !pause_event || !rebuild_event) { Error(ERROR_ACCESS_DENIED); return; }
        for (;;) {
            { std::lock_guard lock(mu); if (stopping) return; }
            const DWORD acquired = read_only ? WAIT_TIMEOUT : WaitForSingleObject(writer_mutex, 100);
            if (acquired == WAIT_OBJECT_0 || acquired == WAIT_ABANDONED) {
                { std::lock_guard lock(mu); owns_writer = true; }
                RunWriter();
                { std::lock_guard lock(mu); owns_writer = false; }
                ReleaseMutex(writer_mutex); return;
            }
            ContentIndexConfig saved_config; ContentIndexStatus saved_status;
            const bool have_config = ReadConfigFile(path, saved_config);
            const bool have_status = ReadStatusFile(path, saved_status);
            { std::lock_guard lock(mu);
              if (have_config) config = std::move(saved_config);
              if (have_status) { status = std::move(saved_status); published_sequence = status.change_sequence; }
              status.paused = WaitForSingleObject(pause_event, 0) == WAIT_OBJECT_0;
              config_changed = false; reset = false; dirty = false; }
            cv.notify_all();
            if (read_only) {
                std::unique_lock lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping; });
            }
        }
    }    bool Save(sqlite3* db, const ContentIndexConfig& c) {
        const auto previous = ReadConfig(db);
        if (!Exec(db, "BEGIN IMMEDIATE; DELETE FROM roots; DELETE FROM exclusions; DELETE FROM scan_queue;")) return false;
        bool ok = true;
        for (const auto& r : c.roots) {
            Statement s(db, "INSERT INTO roots(path,encoding,available) VALUES(?1,?2,0)"); s.Text(1, r.path); s.Int(2, static_cast<uint32_t>(r.encoding)); ok = s.Done() && ok;
        }
        for (const auto& ex : c.excluded_directories) {
            Statement s(db, "INSERT INTO exclusions(value) VALUES(?1)"); s.Text(1, ex); ok = s.Done() && ok;
        }
        for (const auto& ex : c.excluded_paths) {
            Statement excluded(db, "INSERT INTO exclusions(value) VALUES(?1)"); excluded.Text(1, ex); ok = excluded.Done() && ok;
        }
        Statement shared(db, "INSERT OR REPLACE INTO settings(name,value) VALUES('shared_scope',?1)");
        shared.Int(1, c.shared_scope); ok = shared.Done() && ok;
        Statement encoding(db, "INSERT OR REPLACE INTO settings(name,value) VALUES('default_encoding',?1)");
        encoding.Int(1, static_cast<uint32_t>(c.default_encoding)); ok = encoding.Done() && ok;
        // Scope migration retains cached bodies inside the new scope. The
        // subsequent crawl revalidates them without blanking every result.
        Statement rows(db, "SELECT id,path,root FROM documents ORDER BY id");
        int rc = SQLITE_DONE;
        while (ok && rows.p && (rc = sqlite3_step(rows.p)) == SQLITE_ROW) {
            const auto path_value = Column(rows.p, 1);
            const auto root = std::find_if(c.roots.begin(), c.roots.end(), [&](const auto& value) { return Under(path_value, value.path); });
            const auto old_root_path = Column(rows.p, 2);
            const auto old_root = std::find_if(previous.roots.begin(), previous.roots.end(), [&](const auto& value) { return Same(value.path, old_root_path); });
            const auto id = static_cast<uint64_t>(sqlite3_column_int64(rows.p, 0));
            if (root == c.roots.end() || ExcludedContentPath(c, path_value) ||
                (old_root != previous.roots.end() && old_root->encoding != root->encoding)) {
                Statement remove(db, "DELETE FROM documents WHERE id=?1"); remove.Int(1, id); ok = remove.Done();
            } else {
                Statement move(db, "UPDATE documents SET root=?2 WHERE id=?1"); move.Int(1, id); move.Text(2, root->path); ok = move.Done();
            }
        }
        ok = ok && rows.p && rc == SQLITE_DONE;
        Statement s(db, "INSERT OR REPLACE INTO settings(name,value) VALUES('maximum_file_bytes',?1)"); s.Int(1, c.maximum_file_bytes); ok = s.Done() && ok;
        Statement documents(db, "INSERT OR REPLACE INTO settings(name,value) VALUES('maximum_document_bytes',?1)");
        documents.Int(1, c.maximum_document_bytes); ok = documents.Done() && ok;
        return Exec(db, ok ? "COMMIT" : "ROLLBACK") && ok;
    }
    void Error(DWORD error) {
        std::lock_guard lock(mu); ++status.errors; status.error = error;
        recoverable_error_path.clear();
        for (auto& root : status.root_status) if (root.path == status.current_root) root.error = error;
    }
    void Changed() {
        if (writer_db) {
            Statement sequence(writer_db, "SELECT coalesce(max(seq),0) FROM content_changes");
            if (sequence.p && sqlite3_step(sequence.p) == SQLITE_ROW)
                published_sequence = static_cast<uint64_t>(sqlite3_column_int64(sequence.p, 0));
        }
        if(writer_db && published_sequence>100000) {
            Statement prune(writer_db,"DELETE FROM content_changes WHERE seq<?1");prune.Int(1,published_sequence-100000);prune.Done();
        }
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        std::lock_guard lock(mu);
        status.revision = (std::max)(status.revision + 1, Stamp(now));
        status.change_sequence = published_sequence;
        cv.notify_all();
        TraceSearch("content_commit", status.revision);
    }
    void Remove(sqlite3* db, const std::wstring& p) {
        Statement s(db, "DELETE FROM documents WHERE path=?1"); s.Text(1, p);
        if (!s.Done()) Error(ERROR_WRITE_FAULT);
        else if (sqlite3_changes(db)) Changed();
    }
    ReadResult Read(ReadJob job) {
        ReadResult result; result.job = std::move(job);
        const auto native = LongPath(result.job.path);
        for (int attempt = 0; attempt < 3 && ContinueIndexing(); ++attempt) {
            WIN32_FILE_ATTRIBUTE_DATA before{}, after{};
            if (!GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &before)) { result.error = GetLastError(); return result; }
            constexpr DWORD cloud = ::pulse::fs::kCloudPlaceholderAttrs;
            if (before.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | cloud)) { result.error = ERROR_FILE_OFFLINE; return result; }
            const auto version = FileVersion(result.job.path);
            if (!ReadSearchableDocument(native, result.job.maximum, result.body, result.bytes,
                    &result.error, result.job.root.encoding, [&] { return !ContinueIndexing(); })) {
                if (result.error == ERROR_RETRY) continue;
                return result;
            }
            if (GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &after) &&
                before.nFileSizeLow == after.nFileSizeLow && before.nFileSizeHigh == after.nFileSizeHigh &&
                Stamp(before.ftLastWriteTime) == Stamp(after.ftLastWriteTime) && !version.empty() && version == FileVersion(result.job.path)) {
                result.modified = Stamp(after.ftLastWriteTime); result.version = version; result.error = 0; return result;
            }
            result.body.clear(); result.error = ERROR_RETRY;
        }
        if (!result.error) result.error = ERROR_CANCELLED;
        return result;
    }
    void Reader() {
        for (;;) {
            ReadJob job;
            { std::unique_lock lock(queue_mu); queue_cv.wait(lock, [&] { return readers_stop || !read_jobs.empty(); });
              if (readers_stop && read_jobs.empty()) return;
              job = std::move(read_jobs.front()); read_jobs.pop_front(); }
            auto result = Read(std::move(job));
            { std::lock_guard lock(queue_mu); write_queue.push_back(std::move(result)); }
            queue_cv.notify_all();
        }
    }
    void Store(sqlite3* db, ReadResult result) {
        if (!ContinueIndexing()) return;
        if (result.error) {
            if (result.error == ERROR_SHARING_VIOLATION || result.error == ERROR_LOCK_VIOLATION || result.error == ERROR_RETRY) {
                std::lock_guard lock(mu);
                auto& retry = retries[result.job.path];
                retry.change.root = result.job.root;
                if (++retry.attempts <= 10) retry.due = GetTickCount64() + 500;
                else {
                    retries.erase(result.job.path); ++status.errors; status.error = result.error;
                    recoverable_error_path = result.job.path;
                }
                return;
            }
            { std::lock_guard lock(mu); retries.erase(result.job.path); }
            Remove(db, result.job.path);
            if (result.error == ERROR_BAD_FORMAT || result.error == ERROR_FILE_TOO_LARGE ||
                result.error == ERROR_FILE_OFFLINE || result.error == ERROR_NOT_SUPPORTED) {
                std::lock_guard lock(mu); ++status.skipped_files;
            }
            else Error(result.error);
            return;
        }
        { std::lock_guard lock(mu); retries.erase(result.job.path); }
        // The file may have changed again while waiting for the writer. Never
        // publish an old body with new metadata; the notification requeues it.
        WIN32_FILE_ATTRIBUTE_DATA current{};
        if (!GetFileAttributesExW(LongPath(result.job.path).c_str(), GetFileExInfoStandard, &current) ||
            Stamp(current.ftLastWriteTime) != result.modified || FileVersion(result.job.path) != result.version ||
            ((static_cast<uint64_t>(current.nFileSizeHigh) << 32) | current.nFileSizeLow) != result.bytes) {
            if (GetFileAttributesW(LongPath(result.job.path).c_str()) == INVALID_FILE_ATTRIBUTES &&
                (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) Remove(db, result.job.path);
            std::lock_guard lock(mu);
            changes[result.job.path] = {result.job.root}; return;
        }
        if (!Exec(db, "BEGIN IMMEDIATE")) { Error(ERROR_WRITE_FAULT); return; }
        Exec(db,"UPDATE settings SET value=value+1 WHERE name='next_document_id'");
        Statement put(db, "INSERT INTO documents(id,path,root,size,modified,body,epoch,version) VALUES((SELECT value FROM settings WHERE name='next_document_id'),?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(path) DO UPDATE SET root=excluded.root,size=excluded.size,modified=excluded.modified,body=excluded.body,epoch=excluded.epoch,version=excluded.version");
        put.Text(1, result.job.path); put.Text(2, result.job.root.path); put.Int(3, result.bytes); put.Int(4, result.modified); put.Text(5, result.body); put.Int(6, result.job.epoch); put.Text(7, result.version);
        const bool stored = put.Done();
        if (!Exec(db, stored ? "COMMIT" : "ROLLBACK") || !stored) Error(ERROR_WRITE_FAULT);
        else {
            Changed();
            std::lock_guard lock(mu);
            if (recoverable_error_path == result.job.path) {
                status.error = ERROR_SUCCESS;
                recoverable_error_path.clear();
            }
        }
    }
    bool Drain(sqlite3* db, bool wait) {
        ReadResult result;
        { std::unique_lock lock(queue_mu);
          if (wait) queue_cv.wait_for(lock, std::chrono::milliseconds(25), [&] { return !write_queue.empty() || outstanding == 0; });
          if (write_queue.empty()) return false;
          result = std::move(write_queue.front()); write_queue.pop_front(); }
        const auto reservation = result.job.reservation;
        Store(db, std::move(result));
        reserved_bytes -= reservation; --outstanding;
        { std::lock_guard lock(mu); status.pending_files = outstanding + 1; }
        return true;
    }
    void IndexFile(sqlite3* db, const ContentIndexRoot& root, const std::wstring& p,
                   const WIN32_FIND_DATAW& data, const ContentIndexConfig& c, bool realtime = false) {
        const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        const auto dot = p.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? L"" : p.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const auto maximum = IsExtractedDocumentExtension(ext) ? c.maximum_document_bytes : c.maximum_file_bytes;
        constexpr DWORD cloud = ::pulse::fs::kCloudPlaceholderAttrs;
        if (size > maximum || !IsIndexedContentExtension(ext) || ExcludedContentPath(c, p) || (data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE | cloud))) {
            Remove(db, p); std::lock_guard lock(mu); ++status.skipped_files; return;
        }
        const uint64_t modified = Stamp(data.ftLastWriteTime);
        const auto version = FileVersion(p);
        {
            Statement existing(db, "SELECT size,modified,version FROM documents WHERE path=?1"); existing.Text(1, p);
            if (existing.p && sqlite3_step(existing.p) == SQLITE_ROW &&
                static_cast<uint64_t>(sqlite3_column_int64(existing.p, 0)) == size &&
                static_cast<uint64_t>(sqlite3_column_int64(existing.p, 1)) == modified && !version.empty() && Column(existing.p, 2) == version) {
                Statement seen(db, "UPDATE documents SET epoch=?2,root=?3 WHERE path=?1"); seen.Text(1, p); seen.Int(2, epoch); seen.Text(3, root.path);
                if (!seen.Done()) Error(ERROR_WRITE_FAULT);
                { std::lock_guard lock(mu); retries.erase(p); }
                return;
            }
        }
        const uint64_t reservation = IsExtractedDocumentExtension(ext)
            ? document::kMaximumTextChars * sizeof(wchar_t) : (std::max)(uint64_t{2}, size * 2);
        const auto limit = realtime ? 4u : (std::min)(reader_limit, 3u);
        while (outstanding >= limit || reserved_bytes + reservation > kQueueBytes) {
            if (!ContinueIndexing()) return;
            if (!realtime) ProcessChanges(db, c);
            PublishScanProgress(db);
            Drain(db, true);
        }
        if (!ContinueIndexing()) return;
        { std::lock_guard lock(queue_mu);
          if (realtime) read_jobs.push_front({root, p, maximum, reservation, epoch});
          else read_jobs.push_back({root, p, maximum, reservation, epoch}); }
        reserved_bytes += reservation; ++outstanding;
        { std::lock_guard lock(mu); status.pending_files = outstanding + 1; }
        queue_cv.notify_all();
        Drain(db, false);
    }
    bool IncludedChange(const std::wstring& relative, const ContentIndexConfig& c) const {
        size_t start = 0;
        for (size_t end = 0; end <= relative.size(); ++end) {
            if (end != relative.size() && relative[end] != L'\\') continue;
            const auto part = std::wstring_view(relative).substr(start, end - start);
            if (part == L".." || std::any_of(c.excluded_directories.begin(), c.excluded_directories.end(),
                    [&](const auto& ex) { return Same(ex, part); })) return false;
            start = end + 1;
        }
        return !relative.empty();
    }
    void RemoveTree(sqlite3* db, const std::wstring& file) {
        const auto prefix = file + L"\\";
        auto end = prefix; end.back() = L']';
        Statement remove(db, "DELETE FROM documents WHERE path=?1 OR (path>=?2 AND path<?3)");
        remove.Text(1, file); remove.Text(2, prefix); remove.Text(3, end);
        if (!remove.Done()) Error(ERROR_WRITE_FAULT);
        else if (sqlite3_changes(db)) Changed();
    }
    bool ProcessChanges(sqlite3* db, const ContentIndexConfig& c) {
        std::map<std::wstring, Change> batch;
        {
            std::lock_guard lock(mu);
            const auto now = GetTickCount64();
            for (auto& [file, retry] : retries) if (retry.due && retry.due <= now) {
                changes.try_emplace(file, retry.change); retry.due = 0;
            }
            for (auto it = changes.begin(); it != changes.end() && batch.size() < 256;) {
                if (it->second.due <= now) batch.insert(changes.extract(it++));
                else ++it;
            }
            if (!batch.empty()) status.pending_files = batch.size();
        }
        if (batch.empty()) return false;
        cv.notify_all();
        for (const auto& [file, change] : batch) {
            if (!ContinueIndexing()) break;
            if (std::none_of(c.roots.begin(), c.roots.end(), [&](const auto& r) { return Same(r.path, change.root.path); })) continue;
            if (!change.old_path.empty()) {
                Statement move(db, "UPDATE OR REPLACE documents SET path=?2||substr(path,length(?1)+1),root=?3 WHERE path=?1 OR substr(path,1,length(?1)+1)=?1||'\\'");
                move.Text(1, change.old_path); move.Text(2, file); move.Text(3, change.root.path);
                if (!move.Done()) Error(ERROR_WRITE_FAULT);
                else if (sqlite3_changes(db)) Changed();
            }
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(LongPath(file).c_str(), GetFileExInfoStandard, &attributes)) {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) RemoveTree(db, file);
                else Error(error);
                continue;
            }
            if (Canonical(file).empty() || text::IsOfflinePlaceholder(attributes.dwFileAttributes)) {
                RemoveTree(db, file); continue;
            }
            if (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (change.added) { std::lock_guard lock(mu); dirty = true; }
                continue;
            }
            WIN32_FIND_DATAW data{};
            data.dwFileAttributes = attributes.dwFileAttributes;
            data.ftLastWriteTime = attributes.ftLastWriteTime;
            data.nFileSizeLow = attributes.nFileSizeLow; data.nFileSizeHigh = attributes.nFileSizeHigh;
            IndexFile(db, change.root, file, data, c, true);
        }
        return true;
    }
    void UpdateCounts(sqlite3* db) {
        Statement count(db, "SELECT root,count(*),coalesce(sum(size),0) FROM documents GROUP BY root");
        if (!count.p) return;
        std::vector<std::pair<std::wstring, uint64_t>> roots;
        uint64_t files = 0, bytes = 0;
        int result = SQLITE_OK;
        while ((result = sqlite3_step(count.p)) == SQLITE_ROW) {
            const auto number = static_cast<uint64_t>(sqlite3_column_int64(count.p, 1));
            roots.emplace_back(Column(count.p, 0), number);
            files += number;
            bytes += static_cast<uint64_t>(sqlite3_column_int64(count.p, 2));
        }
        if (result != SQLITE_DONE) return;
        std::lock_guard lock(mu);
        status.indexed_files = files;
        status.indexed_bytes = bytes;
        for (auto& root : status.root_status) {
            const auto found = std::find_if(roots.begin(), roots.end(), [&](const auto& row) { return row.first == root.path; });
            root.indexed_files = found == roots.end() ? 0 : found->second;
        }
    }
    void PublishScanProgress(sqlite3* db, bool force = false) {
        const auto now = GetTickCount64();
        if (!force && now - last_count_refresh < 500) return;
        last_count_refresh = now;
        UpdateCounts(db);
        {
            std::lock_guard lock(mu);
            status.pending_files = outstanding + changes.size() + retries.size() + (status.indexing ? 1 : 0);
        }
        PersistStatus();
    }
    void Reconcile(sqlite3* db, const ContentIndexConfig& c) {
        struct Cursor {
            std::deque<std::wstring> directories;
            std::wstring directory;
            HANDLE find = INVALID_HANDLE_VALUE;
            WIN32_FIND_DATAW data{};
            bool complete = true, available = false, finished = false;
            ~Cursor() { if (find != INVALID_HANDLE_VALUE) FindClose(find); }
        };
        Exec(db, "CREATE TABLE IF NOT EXISTS scan_queue(root TEXT,path TEXT PRIMARY KEY,epoch INTEGER)");
        bool resumed = false;
        { Statement saved(db, "SELECT max(epoch) FROM scan_queue");
          if (saved.p && sqlite3_step(saved.p) == SQLITE_ROW && sqlite3_column_type(saved.p,0) != SQLITE_NULL) {
              epoch = static_cast<uint64_t>(sqlite3_column_int64(saved.p,0)); resumed = true;
          } }
        if (!resumed) ++epoch;
        std::vector<std::unique_ptr<Cursor>> cursors;
        { std::lock_guard lock(mu); status.indexing = true; status.pending_files = 1;
          status.skipped_files = 0; status.errors = 0; status.error = 0; status.root_status.clear(); recoverable_error_path.clear(); }
        auto enqueue = [&](const ContentIndexRoot& root, Cursor& cursor, const std::wstring& directory) {
            Statement put(db, "INSERT OR IGNORE INTO scan_queue(root,path,epoch) VALUES(?1,?2,?3)");
            put.Text(1,root.path); put.Text(2,directory); put.Int(3,epoch);
            if (!put.Done()) Error(ERROR_WRITE_FAULT);
            cursor.directories.push_back(directory);
        };
        for (const auto& root : c.roots) {
            auto cursor = std::make_unique<Cursor>();
            cursor->available = Accessible(root.path);
            const DWORD root_error = cursor->available ? 0 : GetLastError();
            ContentIndexRootStatus state;
            state.path = root.path; state.available = cursor->available;
            state.indexing = cursor->available; state.error = root_error;
            state.state = cursor->available ? (resumed ? ContentIndexRootStatus::State::Recovering : ContentIndexRootStatus::State::Scanning)
                : root_error == ERROR_ACCESS_DENIED ? ContentIndexRootStatus::State::AccessFailed : ContentIndexRootStatus::State::Offline;
            { std::lock_guard lock(mu); status.root_status.push_back(state); }
            Statement availability(db, "UPDATE roots SET available=?2 WHERE path=?1 AND available<>?2");
            availability.Text(1,root.path); availability.Int(2,cursor->available);
            if (availability.Done() && sqlite3_changes(db)) Changed();
            if (cursor->available) {
                Statement pending(db, "SELECT path FROM scan_queue WHERE root=?1 ORDER BY path"); pending.Text(1,root.path);
                while (pending.p && sqlite3_step(pending.p) == SQLITE_ROW) cursor->directories.push_back(Column(pending.p,0));
                if (cursor->directories.empty()) enqueue(root,*cursor,root.path);
            } else cursor->finished = true;
            cursors.push_back(std::move(cursor));
        }
        auto service_due = GetTickCount64();
        bool unfinished = true;
        while (unfinished && ContinueIndexing()) {
            unfinished = false;
            for (size_t i = 0; i < c.roots.size() && ContinueIndexing(); ++i) {
                auto& cursor = *cursors[i]; const auto& root = c.roots[i];
                if (cursor.finished) continue;
                unfinished = true;
                { std::lock_guard lock(mu); status.current_root = root.path; }
                reader_limit = DiskReaders(root.path);
                if (GetTickCount64() >= service_due) {
                    ProcessChanges(db,c); while (Drain(db,false)) {}
                    PersistStatus(); service_due = GetTickCount64() + 50;
                }
                for (unsigned quantum = 0; quantum < 32 && ContinueIndexing(); ++quantum) {
                    if (cursor.find == INVALID_HANDLE_VALUE) {
                        if (cursor.directories.empty()) { cursor.finished = true; break; }
                        cursor.directory = std::move(cursor.directories.front()); cursor.directories.pop_front();
                        cursor.find = FindFirstFileExW((LongPath(cursor.directory) + L"\\*").c_str(), FindExInfoBasic,
                            &cursor.data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                        if (cursor.find == INVALID_HANDLE_VALUE) {
                            const auto error = GetLastError();
                            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) { cursor.complete = false; Error(error); }
                        }
                    }
                    if (cursor.find != INVALID_HANDLE_VALUE) {
                        const std::wstring name = cursor.data.cFileName;
                        if (name != L"." && name != L"..") {
                            const auto file = cursor.directory + (cursor.directory.back() == L'\\' ? L"" : L"\\") + name;
                            if (cursor.data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                                if (!(cursor.data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                                    !text::IsOfflinePlaceholder(cursor.data.dwFileAttributes) && !ExcludedContentPath(c,file)) enqueue(root,cursor,file);
                            } else IndexFile(db,root,file,cursor.data,c);
                        }
                        if (FindNextFileW(cursor.find,&cursor.data)) continue;
                        if (GetLastError() != ERROR_NO_MORE_FILES) { cursor.complete = false; Error(GetLastError()); }
                        FindClose(cursor.find); cursor.find = INVALID_HANDLE_VALUE;
                    }
                    // Persist the frontier only after this directory's reads have
                    // committed. A restart can replay an unfinished directory safely.
                    while (outstanding && ContinueIndexing()) { ProcessChanges(db,c); Drain(db,true); }
                    if (ContinueIndexing()) {
                        Statement completed(db,"DELETE FROM scan_queue WHERE path=?1"); completed.Text(1,cursor.directory);
                        if (!completed.Done()) Error(ERROR_WRITE_FAULT);
                    }
                    break;
                }
            }
        }
        while (outstanding) { if (ContinueIndexing()) ProcessChanges(db,c); Drain(db,true); }
        for (size_t i = 0; i < c.roots.size(); ++i) {
            auto& cursor = *cursors[i]; const auto& root = c.roots[i];
            if (cursor.available && cursor.finished && cursor.complete && ContinueIndexing()) {
                Statement prune(db,"DELETE FROM documents WHERE root=?1 AND epoch<>?2"); prune.Text(1,root.path); prune.Int(2,epoch);
                if (!prune.Done()) Error(ERROR_WRITE_FAULT); else if (sqlite3_changes(db)) Changed();
            }
            std::lock_guard lock(mu); auto& state = status.root_status[i];
            state.indexing = false;
            if (state.available) state.state = cursor.finished ? ContentIndexRootStatus::State::Ready : ContentIndexRootStatus::State::Waiting;
        }
        UpdateCounts(db);
        {
            std::lock_guard lock(mu);
            status.indexing = false; status.pending_files = changes.size() + retries.size(); status.current_root.clear();
            const auto available = std::count_if(status.root_status.begin(),status.root_status.end(),[](const auto& root){return root.available;});
            status.coverage = std::to_wstring(available) + L"/" + std::to_wstring(c.roots.size()) + L" roots available; " +
                std::to_wstring(status.indexed_files) + L" files; " + std::to_wstring(status.skipped_files) + L" skipped; " + std::to_wstring(status.errors) + L" errors";
        }
        cv.notify_all();
    }
    void WatchFeed(ContentIndexConfig c) {
        IndexFeedConnection connection(feed_stop);
        uint64_t source_epoch=0,cursor=0;
        while(WaitForSingleObject(feed_stop,0)!=WAIT_OBJECT_0) {
            FileFeedPage page;
            if(!connection.Request(true,L"",source_epoch,cursor,page) || !page.ready || page.gap) {
                {std::lock_guard lock(mu);feed_ready=false;dirty=true;}
                source_epoch=0;cursor=0;
                if(WaitForSingleObject(feed_stop,250)==WAIT_OBJECT_0) break;
                continue;
            }
            {
                std::unique_lock lock(mu);
                if(!source_epoch) {feed_ready=true;feed_epoch=page.epoch;dirty=true;cv.notify_all();}
                for(const auto& event:page.records) {
                    cv.wait(lock,[&]{return stopping || changes.size()<4096 || WaitForSingleObject(feed_stop,0)==WAIT_OBJECT_0;});
                    if(stopping || WaitForSingleObject(feed_stop,0)==WAIT_OBJECT_0) return;
                    auto enqueue=[&](const std::wstring& file,const std::wstring& previous,bool added,bool immediate) {
                        if(file.empty()) return;
                        const auto root=std::find_if(c.roots.begin(),c.roots.end(),[&](const auto& r){return Under(file,r.path);});
                        if(root==c.roots.end() || ExcludedContentPath(c,file)) return;
                        auto& change=changes[file];change.root=*root;change.old_path=previous;change.added|=added;
                        change.due=GetTickCount64()+(immediate ? 0 : 150);retries.erase(file);
                        TraceSearch("content_event",event.id,file);
                    };
                    const bool renamed=event.kind==ChangeKind::Renamed;
                    const bool included=std::any_of(c.roots.begin(),c.roots.end(),[&](const auto& r){return Under(event.path,r.path);}) && !ExcludedContentPath(c,event.path);
                    if(renamed && !included) enqueue(event.old_path,L"",false,true);
                    else enqueue(event.path,renamed ? event.old_path : std::wstring{},event.kind==ChangeKind::Created || renamed,
                        renamed || event.kind==ChangeKind::Deleted);
                }
                source_epoch=page.epoch;cursor=page.next;
            }
            cv.notify_all();
        }
    }
    void ReconcileShared(sqlite3* db,const ContentIndexConfig& c) {
        UpdateCounts(db);
        uint64_t source_epoch=0;
        {
            std::unique_lock lock(mu);
            cv.wait_for(lock,std::chrono::milliseconds(250),[&]{return stopping || config_changed || feed_ready;});
            if(!feed_ready) {
                dirty=true;status.indexing=false;status.pending_files=1;status.error=0;
                status.coverage=L"Waiting for the shared file index";return;
            }
            source_epoch=feed_epoch;status.indexing=true;status.pending_files=1;status.root_status.clear();
            for(const auto& root:c.roots) {ContentIndexRootStatus state;state.path=root.path;state.state=ContentIndexRootStatus::State::Scanning;state.indexing=true;status.root_status.push_back(std::move(state));}
        }
        ++epoch;
        IndexFeedConnection source(feed_stop);
        std::vector<uint64_t> cursors(c.roots.size());
        std::vector<bool> completed(c.roots.size());
        std::vector<FileFeedPage> pages(c.roots.size());
        std::vector<size_t> offsets(c.roots.size());
        std::vector<bool> loaded(c.roots.size());
        std::vector<unsigned> root_readers;
        for (const auto& root : c.roots) root_readers.push_back(DiskReaders(root.path));
        size_t remaining=c.roots.size();
        size_t first_root = 0;
        PublishScanProgress(db, true);
        while(remaining && ContinueIndexing()) {
            bool advanced = false;
            for(size_t turn=0;turn<c.roots.size() && ContinueIndexing();++turn) {
                const size_t i = (first_root + turn) % c.roots.size();
                if(completed[i]) continue;
                const auto& root=c.roots[i];
                auto& page = pages[i];
                if(!loaded[i] && (!source.Request(false,root.path,source_epoch,cursors[i],page) || page.gap || !page.ready)) {
                    std::lock_guard lock(mu);dirty=true;status.indexing=false;status.pending_files=1;
                    for(auto& state:status.root_status) {state.indexing=false;state.state=ContentIndexRootStatus::State::Recovering;}
                    return;
                }
                loaded[i] = true;
                {std::lock_guard lock(mu);status.current_root=root.path;}
                reader_limit=root_readers[i];
                // Keep a bounded page per root; slow parses must not hold the
                // other roots behind the rest of a 1024-record page.
                for(size_t visited=0; offsets[i]<page.records.size() && visited<16; ++visited) {
                    const auto& record = page.records[offsets[i]];
                    if(!ContinueIndexing()) break;
                    ProcessChanges(db,c);
                    if(ExcludedContentPath(c,record.path)) {++offsets[i];advanced=true;continue;}
                    WIN32_FILE_ATTRIBUTE_DATA attributes{};
                    if(!GetFileAttributesExW(LongPath(record.path).c_str(),GetFileExInfoStandard,&attributes)) {++offsets[i];advanced=true;continue;}
                    WIN32_FIND_DATAW data{};data.dwFileAttributes=attributes.dwFileAttributes;data.ftLastWriteTime=attributes.ftLastWriteTime;
                    data.nFileSizeLow=attributes.nFileSizeLow;data.nFileSizeHigh=attributes.nFileSizeHigh;
                    const auto dot = record.path.find_last_of(L'.');
                    const auto ext = dot == std::wstring::npos ? std::wstring{} : record.path.substr(dot);
                    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                    const uint64_t reservation = IsExtractedDocumentExtension(ext)
                        ? document::kMaximumTextChars * sizeof(wchar_t) : (std::max)(uint64_t{2}, size * 2);
                    const auto maximum = IsExtractedDocumentExtension(ext) ? c.maximum_document_bytes : c.maximum_file_bytes;
                    if (size <= maximum && IsIndexedContentExtension(ext) &&
                        (outstanding >= (std::min)(reader_limit, 3u) || (outstanding && reserved_bytes + reservation > kQueueBytes))) break;
                    IndexFile(db,root,record.path,data,c);
                    ++offsets[i]; advanced=true;
                    if (IsIndexedContentExtension(ext)) break;
                }
                while(Drain(db,false)) {}
                if(offsets[i] == page.records.size()) {
                    cursors[i]=page.next; offsets[i]=0; loaded[i]=false; advanced=true;
                    if(page.done) {completed[i]=true;--remaining;}
                    page.records.clear();
                }
                PublishScanProgress(db);
            }
            first_root = (first_root + 1) % c.roots.size();
            if (!advanced && outstanding) Drain(db, true);
            PublishScanProgress(db);
        }
        while(outstanding) {if(ContinueIndexing()) ProcessChanges(db,c);Drain(db,true);PublishScanProgress(db);}
        for(size_t i=0;i<c.roots.size();++i) {
            const auto& root=c.roots[i];const bool available=Accessible(root.path);
            const DWORD error=available ? 0 : GetLastError();
            Statement state(db,"UPDATE roots SET available=?2 WHERE path=?1");state.Text(1,root.path);state.Int(2,available);state.Done();
            // The source snapshot is authoritative only after the corresponding
            // change stream remains continuous through the last page.
            bool continuous=false;{std::lock_guard lock(mu);continuous=feed_ready && feed_epoch==source_epoch;}
            bool pending_changes=false;{std::lock_guard lock(mu);pending_changes=!changes.empty();}
            if(completed[i] && available && continuous && !pending_changes && ContinueIndexing()) {
                Statement prune(db,"DELETE FROM documents WHERE root=?1 AND epoch<>?2");prune.Text(1,root.path);prune.Int(2,epoch);
                if(!prune.Done()) Error(ERROR_WRITE_FAULT);else if(sqlite3_changes(db)) Changed();
            }
            std::lock_guard lock(mu);auto& status_root=status.root_status[i];status_root.available=available;status_root.error=error;status_root.indexing=false;
            status_root.state=!available ? (error==ERROR_ACCESS_DENIED ? ContentIndexRootStatus::State::AccessFailed : ContentIndexRootStatus::State::Offline)
                : completed[i] ? ContentIndexRootStatus::State::Ready : ContentIndexRootStatus::State::Waiting;
        }
        UpdateCounts(db);
        {std::lock_guard lock(mu);status.indexing=false;status.pending_files=changes.size()+retries.size();status.current_root.clear();status.coverage=L"Shared file index";}
        cv.notify_all();
    }
    void Watch(const ContentIndexConfig& c) {
        SetEvent(feed_stop);cv.notify_all();
        if(feed_thread.joinable()) feed_thread.join();
        ResetEvent(feed_stop);
        watches.clear();
        { std::lock_guard lock(mu); changes.clear(); retries.clear(); }
        if(c.shared_scope) {
            {std::lock_guard lock(mu);feed_ready=false;}
            feed_thread=std::thread([this,c]{WatchFeed(c);});return;
        }
        for (const auto& root : c.roots) {
            auto watch = std::make_unique<fs::DirWatch>();
            watch->Start(root.path, [this, root, c](bool overflow, std::vector<fs::DirNotifyEvent> events) {
                std::lock_guard lock(mu);
                if (stopping) return;
                if (overflow) dirty = true;
                auto enqueue = [&](const std::wstring& relative, bool added, bool immediate, const std::wstring& old_name = {}) {
                    if (!IncludedChange(relative, c)) return;
                    if (changes.size() >= 4096) { changes.clear(); dirty = true; return; }
                    const auto file = root.path + (root.path.back() == L'\\' ? L"" : L"\\") + relative;
                    if (ExcludedContentPath(c, file)) return;
                    auto& change = changes[file]; change.root = root; change.added |= added;
                    change.due = GetTickCount64() + (immediate ? 0 : 150);
                    if (!old_name.empty()) change.old_path = root.path + (root.path.back() == L'\\' ? L"" : L"\\") + old_name;
                    TraceSearch("content_event", 0, file);
                    retries.erase(file);
                };
                for (const auto& event : events) {
                    const bool renamed = event.action == FILE_ACTION_RENAMED_NEW_NAME && !event.old_name.empty();
                    if (!renamed) enqueue(event.old_name, false, true);
                    enqueue(event.name, event.action == FILE_ACTION_ADDED || event.action == FILE_ACTION_RENAMED_NEW_NAME,
                        event.action == FILE_ACTION_REMOVED || renamed, renamed ? event.old_name : std::wstring{});
                }
                cv.notify_all();
            }, true);
            watches.push_back(std::move(watch));
        }
    }
    void RunWriter() {
        ContentIndexConfig durable;
        const bool have_durable = ReadConfigFile(path, durable);
        sqlite3* db = nullptr;
        auto initialize = [&] { return Open(path, &db, false) && Exec(db,
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA recursive_triggers=ON;"
            "CREATE TABLE IF NOT EXISTS roots(path TEXT PRIMARY KEY,encoding INTEGER,available INTEGER);"
            "CREATE TABLE IF NOT EXISTS settings(name TEXT PRIMARY KEY,value INTEGER);"
            "CREATE TABLE IF NOT EXISTS exclusions(value TEXT);"
            "CREATE TABLE IF NOT EXISTS documents(id INTEGER PRIMARY KEY,path TEXT UNIQUE,root TEXT,size INTEGER,modified INTEGER,body TEXT,epoch INTEGER,version TEXT);"
            "CREATE INDEX IF NOT EXISTS documents_root ON documents(root);"
            "CREATE INDEX IF NOT EXISTS documents_root_size ON documents(root,size);"
            "INSERT INTO settings(name,value) SELECT 'next_document_id',coalesce(max(id),0) FROM documents WHERE true ON CONFLICT(name) DO UPDATE SET value=max(value,excluded.value);"
            "CREATE TABLE IF NOT EXISTS scan_queue(root TEXT,path TEXT PRIMARY KEY,epoch INTEGER);"
            "CREATE TABLE IF NOT EXISTS content_changes(seq INTEGER PRIMARY KEY AUTOINCREMENT,path TEXT NOT NULL);"
            "INSERT INTO content_changes(path) SELECT '' WHERE NOT EXISTS(SELECT 1 FROM content_changes);"
            "CREATE TRIGGER IF NOT EXISTS content_changes_ai AFTER INSERT ON documents BEGIN INSERT INTO content_changes(path) VALUES(new.path); END;"
            "CREATE TRIGGER IF NOT EXISTS content_changes_ad AFTER DELETE ON documents BEGIN INSERT INTO content_changes(path) VALUES(old.path); END;"
            "CREATE TRIGGER IF NOT EXISTS content_changes_au AFTER UPDATE OF body,path,size,modified ON documents BEGIN INSERT INTO content_changes(path) VALUES(old.path); INSERT INTO content_changes(path) SELECT new.path WHERE old.path<>new.path; END;"
            "CREATE VIRTUAL TABLE IF NOT EXISTS content_fts USING fts5(body,content='documents',content_rowid='id',tokenize='pulsegram',detail=none);"
            "CREATE TRIGGER IF NOT EXISTS documents_ai AFTER INSERT ON documents BEGIN INSERT INTO content_fts(rowid,body) VALUES(new.id,new.body); END;"
            "CREATE TRIGGER IF NOT EXISTS documents_ad AFTER DELETE ON documents BEGIN INSERT INTO content_fts(content_fts,rowid,body) VALUES('delete',old.id,old.body); END;"
            "CREATE TRIGGER IF NOT EXISTS documents_au AFTER UPDATE OF body ON documents BEGIN INSERT INTO content_fts(content_fts,rowid,body) VALUES('delete',old.id,old.body); INSERT INTO content_fts(rowid,body) VALUES(new.id,new.body); END;"); };
        if (!initialize()) {
            const int code = db ? sqlite3_errcode(db) : SQLITE_CANTOPEN;
            if (db) { sqlite3_close(db); db = nullptr; }
            if (!have_durable || (code != SQLITE_CORRUPT && code != SQLITE_NOTADB)) { Error(ERROR_DATABASE_FAILURE); return; }
            // Only the rebuildable cache is quarantined. Durable roots/encoding
            // configuration lives independently and survives database corruption.
            const auto quarantine = path + L".corrupt." + std::to_wstring(GetTickCount64());
            if (!MoveFileExW(path.c_str(), quarantine.c_str(), MOVEFILE_WRITE_THROUGH)) { Error(GetLastError()); return; }
            for (const auto* suffix : {L"-wal", L"-shm"}) MoveFileExW((path + suffix).c_str(), (quarantine + suffix).c_str(), MOVEFILE_WRITE_THROUGH);
            if (!initialize()) { Error(ERROR_DATABASE_FAILURE); if (db) sqlite3_close(db); return; }
        }
        { Statement columns(db, "PRAGMA table_info(documents)"); bool has_version = false;
          while (columns.p && sqlite3_step(columns.p) == SQLITE_ROW) if (Column(columns.p, 1) == L"version") has_version = true;
          if (!has_version) Exec(db, "ALTER TABLE documents ADD COLUMN version TEXT"); }
        writer_db = db;
        ContentIndexConfig c;
        // A new writer invalidates existing queries even after an interrupted
        // commit whose status sidecar was not yet published.
        ContentIndexStatus previous_status;
        if (ReadStatusFile(path, previous_status)) {
            std::lock_guard lock(mu); status.revision = previous_status.revision;
        }
        Changed();
        const auto database_config = ReadConfig(db);
        if (!LoadOrCreateConfig(path, database_config, durable)) Error(ERROR_WRITE_FAULT);
        { std::lock_guard lock(mu); if (!config_changed) config = durable; c = config; config_changed = !SameConfig(database_config, config); dirty = true; }
        for (unsigned i = 0; i < 4; ++i) readers.emplace_back([this] { Reader(); });
        { Statement previous(db, "SELECT coalesce(max(epoch),0) FROM documents"); if (previous.p && sqlite3_step(previous.p) == SQLITE_ROW) epoch = static_cast<uint64_t>(sqlite3_column_int64(previous.p, 0)); }
        // Register before enumeration so changes during a crawl trigger another pass.
        Watch(c);
        auto last_reconcile = std::chrono::steady_clock::now();
        for (;;) {
            bool run = false, save = false, rebuild = false;
            ContentIndexConfig external;
            if (ReadConfigFile(path, external)) { std::lock_guard lock(mu); if (!SameConfig(config, external)) { config = std::move(external); config_changed = true; status.pending_files = 1; } }
            {
                std::unique_lock lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping || ((!status.paused) && (dirty || config_changed || reset)); });
                if (stopping) break;
                status.paused = WaitForSingleObject(pause_event, 0) == WAIT_OBJECT_0;
                if (WaitForSingleObject(rebuild_event, 0) == WAIT_OBJECT_0) reset = true;
                if (status.paused) { lock.unlock(); PersistStatus(); continue; }
                // Periodic reconciliation also recovers unavailable roots and watcher failures/overflow.
                if (std::chrono::steady_clock::now() - last_reconcile > std::chrono::minutes(5)) dirty = true;
                save = config_changed; rebuild = reset; run = dirty || save || rebuild;
                if (run) { c = config; dirty = false; config_changed = false; reset = false; }
            }
            if (!run) {
                bool changed = ProcessChanges(db, c);
                while (Drain(db, false)) changed = true;
                if (changed) {
                    UpdateCounts(db);
                    cv.notify_all();
                }
                { std::lock_guard lock(mu); status.pending_files = outstanding + changes.size() + retries.size(); }
                PersistStatus(); continue;
            }
            if (save) {
                if (!Save(db, c)) { Error(ERROR_WRITE_FAULT); std::lock_guard lock(mu); status.pending_files = 0; config = ReadConfig(db); continue; }
                Changed();
                Watch(c);
            }
            else if (rebuild) {
                if (!Exec(db, "DELETE FROM documents")) Error(ERROR_WRITE_FAULT);
                else if (sqlite3_changes(db)) Changed();
            }
            if(c.shared_scope) ReconcileShared(db,c);else Reconcile(db,c);
            PersistStatus();
            last_reconcile = std::chrono::steady_clock::now();
        }
        { std::lock_guard lock(queue_mu); readers_stop = true; }
        queue_cv.notify_all();
        for (auto& reader : readers) reader.join();
        sqlite3_close(db);
    }
};
ContentIndex::ContentIndex(std::wstring path, bool read_only) : ContentIndex(std::move(path), read_only ? ContentAgentMode::Observer : ContentAgentMode::LegacyWriter) {}
ContentIndex::ContentIndex(std::wstring path, ContentAgentMode mode) : impl_(std::make_unique<Impl>(std::move(path), mode)) {}
ContentIndex::~ContentIndex() = default;
bool ContentIndex::Configure(const ContentIndexConfig& input) {
    if (impl_->mode == ContentAgentMode::Observer) return false;
    if (!input.maximum_document_bytes || input.maximum_document_bytes > document::kMaximumFileBytes) return false;
    if (static_cast<uint32_t>(input.default_encoding) > 3) return false;
    if (input.roots.size() > 32 || input.excluded_directories.size() > 256 || input.excluded_paths.size() > 256 || !input.maximum_file_bytes || input.maximum_file_bytes > 64ull * 1024 * 1024) return false;
    auto config = input;
    for (auto& root : config.roots) {
        root.path = Canonical(root.path);
        if (root.path.empty() || static_cast<uint32_t>(root.encoding) > 3) return false;
    }
    for (size_t i = 0; i < config.roots.size(); ++i) {
        for (size_t j = i + 1; j < config.roots.size();) {
            if (Same(config.roots[i].path, config.roots[j].path)) config.roots.erase(config.roots.begin() + static_cast<ptrdiff_t>(j));
            else ++j;
        }
    }
    for (const auto& ex : config.excluded_directories) if (ex.empty() || ex.find_first_of(L"\\/:") != std::wstring::npos) return false;
    for (auto& ex : config.excluded_paths) {
        ex = ContentScopeKey(std::move(ex));
        if (ex.size() < 3 || ex.size() > 32768 || ex[1] != L':' || ex[2] != L'\\') return false;
    }
    if (!WriteConfigFile(impl_->path, config)) return false;
    { std::lock_guard lock(impl_->mu);
      if (!SameConfig(impl_->config, config)) { impl_->config = std::move(config);
          if (impl_->mode == ContentAgentMode::LegacyWriter) { impl_->config_changed = true; impl_->status.pending_files = 1; }
          else { ++impl_->status.revision; }
      } }
    impl_->cv.notify_all(); return true;
}
ContentIndexConfig ContentIndex::Configuration() const { std::lock_guard lock(impl_->mu); return impl_->config; }
ContentIndexStatus ContentIndex::Status() const { std::lock_guard lock(impl_->mu); return impl_->status; }
void ContentIndex::Pause(bool paused) {
    if (impl_->read_only) return;
    if (paused) SetEvent(impl_->pause_event); else ResetEvent(impl_->pause_event);
    { std::lock_guard lock(impl_->mu); impl_->status.paused = paused; }
    impl_->cv.notify_all();
}
void ContentIndex::Rebuild() {
    if (impl_->read_only) return;
    SetEvent(impl_->rebuild_event);
    { std::lock_guard lock(impl_->mu); impl_->status.pending_files = 1; }
    impl_->cv.notify_all();
}
bool ContentIndex::WaitUntilIdle(DWORD timeout_ms) {
    std::unique_lock lock(impl_->mu);
    return impl_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !impl_->config_changed && !impl_->reset && !impl_->dirty && impl_->changes.empty() && impl_->retries.empty() && !impl_->status.indexing && !impl_->status.pending_files; });
}
bool ContentIndex::WaitForRevision(uint64_t revision, const std::atomic<bool>& cancel, DWORD timeout_ms) {
    const auto until = GetTickCount64() + timeout_ms;
    std::unique_lock lock(impl_->mu);
    while (!impl_->stopping && !cancel.load() && GetTickCount64() < until) {
        if (impl_->published_sequence.load() > revision) return true;
        impl_->cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    return false;
}
bool ContentIndex::Search(const ContentSearchRequest& r, const std::atomic<bool>& cancel, ContentBatchCallback callback) {
    return SearchCached(r, cancel, std::move(callback), nullptr);
}
bool ContentIndex::SearchCached(const ContentSearchRequest& r, const std::atomic<bool>& cancel, ContentBatchCallback callback, void* snapshot) {
    TraceSearch("content_query_start", r.generation);
    struct QueryGuard { Impl* p; QueryGuard(Impl* v):p(v) { ++p->queries; } ~QueryGuard() { --p->queries; p->cv.notify_all(); } } guard(impl_.get());
    ContentSearchProgress progress; progress.generation = r.generation;
    progress.live = r.subscribe; progress.delta = r.incremental || r.after_revision != 0;
    if (!cancel.load() && !callback(progress, {})) return false;
    auto slot = snapshot ? nullptr : std::make_unique<ReadSlot>(impl_->path, &cancel);
    sqlite3* db = static_cast<sqlite3*>(snapshot);
    if (!snapshot && (!slot->acquired || !Open(impl_->path, &db, true))) {
        if (db) sqlite3_close(db); progress.done = true; progress.error = cancel.load() ? ERROR_CANCELLED : ERROR_DATABASE_FAILURE; callback(progress, {}); return false;
    }
    sqlite3_progress_handler(db, 512, [](void* p) { return static_cast<const std::atomic<bool>*>(p)->load() ? 1 : 0; }, const_cast<std::atomic<bool>*>(&cancel));
    if (!snapshot) Exec(db, "BEGIN");
    {
        Statement sequence(db, "SELECT coalesce(max(seq),0) FROM content_changes");
        if (sequence.p && sqlite3_step(sequence.p) == SQLITE_ROW)
            progress.index_revision = static_cast<uint64_t>(sqlite3_column_int64(sequence.p, 0));
    }
    bool gap=false;
    if(progress.delta) {
        Statement earliest(db,"SELECT coalesce(min(seq),0) FROM content_changes");
        if(earliest.p && sqlite3_step(earliest.p)==SQLITE_ROW)
            gap=r.after_revision+1<static_cast<uint64_t>(sqlite3_column_int64(earliest.p,0));
    }
    if(gap) {
        if (!snapshot) { Exec(db,"COMMIT");sqlite3_close(db); } progress.error=ERROR_RETRY;progress.done=true;
        callback(progress,{});return false;
    }
    bool ok = true;
    try {
        std::vector<std::wstring> live_roots;
        Statement roots(db, "SELECT path,available FROM roots");
        int root_rc = SQLITE_DONE;
        while (roots.p && (root_rc = sqlite3_step(roots.p)) == SQLITE_ROW) {
            auto path = Column(roots.p, 0);
            const auto overlaps = [&](const std::wstring& scope) {
                return scope.empty() || Under(path, scope) || Under(scope, path);
            };
            if (!overlaps(r.root) || (!r.roots.empty() &&
                std::none_of(r.roots.begin(), r.roots.end(), overlaps))) continue;
            if (Accessible(path)) {
                live_roots.push_back(std::move(path));
            } else {
                // Missing coverage is not an authoritative empty query. In
                // particular, a refresh must not erase previously visible hits.
                progress.error = ERROR_NOT_READY;
            }
        }
        if (!roots.p || root_rc != SQLITE_DONE) progress.error = ERROR_DATABASE_FAILURE;
        const auto gram_query = CandidateQuery(r);
        const auto filename_query = ParseQuery(r.filename_query);
        std::string sql = "SELECT path,size,modified,root,id FROM documents";
        if (progress.delta) sql = "SELECT c.path,d.size,d.modified,d.root,d.id FROM (SELECT DISTINCT path FROM content_changes WHERE seq>?2 AND seq<=?3) c LEFT JOIN documents d ON d.path=c.path";
        else if (!gram_query.empty()) sql += " WHERE id IN (SELECT rowid FROM content_fts WHERE content_fts MATCH ?1)";
        {
            Statement count(db, ("SELECT count(*) FROM (" + sql + ")").c_str());
            if (!gram_query.empty() && count.p) sqlite3_bind_text(count.p, 1, gram_query.c_str(), -1, SQLITE_TRANSIENT);
            if (progress.delta) { count.Int(2, r.after_revision); count.Int(3, progress.index_revision); }
            if (count.p && sqlite3_step(count.p) == SQLITE_ROW)
                progress.total_files = static_cast<uint64_t>(sqlite3_column_int64(count.p, 0));
        }
        // Publish the denominator before candidate ordering or body work; a
        // long zero-hit query must still expose what it is checking.
        if (progress.total_files && !callback(progress, {})) ok = false;
        if (!progress.delta) {
        switch (r.sort) {
        case ContentResultSort::Type: sql += " ORDER BY pulse_extension(path) COLLATE PULSE_ORDINAL"; break;
        case ContentResultSort::Name: sql += " ORDER BY pulse_filename(path) COLLATE PULSE_ORDINAL"; break;
        case ContentResultSort::Path: sql += " ORDER BY path COLLATE PULSE_ORDINAL"; break;
        case ContentResultSort::Size: sql += " ORDER BY size"; break;
        case ContentResultSort::Mtime: sql += " ORDER BY modified"; break;
        default: sql += " ORDER BY id"; break;
        }
        sql += r.sort_desc ? " DESC,id ASC" : " ASC,id ASC";
        }
        Statement candidates(db, sql.c_str());        if (!gram_query.empty() && candidates.p) sqlite3_bind_text(candidates.p, 1, gram_query.c_str(), -1, SQLITE_TRANSIENT);
        if (progress.delta) { candidates.Int(2, r.after_revision); candidates.Int(3, progress.index_revision); }
        size_t total = 0;
        std::vector<ContentHit> batch;
        auto last_update = GetTickCount64();
        int rc = SQLITE_DONE;
        LONGLONG candidate_ticks = 0, fetch_ticks = 0, match_ticks = 0;
        auto clock = [] { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; };
        auto next = [&] {
            const auto start = clock();
            const int result = sqlite3_step(candidates.p);
            candidate_ticks += clock() - start;
            return result;
        };
        bool first_candidate = true;
        Statement cached(db, "SELECT body FROM documents WHERE id=?1");
        std::unique_ptr<QueryMatchWorkers> workers;
        struct PendingMatch { std::future<QueryMatch> result; size_t bytes = 0; uint64_t file_bytes = 0; };
        std::deque<PendingMatch> pending;
        size_t pending_bytes = 0, checked_bodies = 0;
        auto publish = [&] {
            const bool accepted = callback(progress, std::move(batch));
            batch.clear(); last_update = GetTickCount64(); return accepted;
        };
        auto complete = [&](QueryMatch result, uint64_t file_bytes) {
            ++progress.scanned_files; progress.scanned_bytes += file_bytes;
            match_ticks += result.ticks;
            if (cancel.load()) return false;
            if (result.hit) {
                if (r.maximum_hits && total >= r.maximum_hits) { progress.truncated = true; return false; }
                ++total;
                if (progress.delta) batch.back() = std::move(*result.hit);
                else batch.push_back(std::move(*result.hit));
            }
            return !((result.hit && total == 1) || batch.size() >= 64 || GetTickCount64() - last_update >= 100) || publish();
        };
        auto consume = [&] {
            while (pending.front().result.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
                if (cancel.load()) return false;
                if (GetTickCount64() - last_update >= 100 && !publish()) return false;
            }
            auto result = pending.front().result.get();
            const auto bytes = pending.front().file_bytes;
            pending_bytes -= pending.front().bytes; pending.pop_front();
            return complete(std::move(result), bytes);
        };
        while (ok && candidates.p && !cancel.load() && (rc = next()) == SQLITE_ROW) {
            if (first_candidate) { TraceSearch("content_first_candidate", r.generation); first_candidate = false; }
            // Flush partial results even when later candidates do not match.
            const auto now = GetTickCount64();
            if (now - last_update >= 100) {
                if (!callback(progress, std::move(batch))) { ok = false; break; }
                batch.clear(); last_update = now;
            }
            const auto path = Column(candidates.p, 0);
            bool body_pending = false;
            struct CheckedCandidate {
                uint64_t& count; bool& deferred;
                ~CheckedCandidate() { if (!deferred) ++count; }
            } checked_candidate{progress.scanned_files, body_pending};
            if (progress.delta) {
                ContentHit removed; removed.path = path; removed.removed = true;
                batch.push_back(std::move(removed));
                if (sqlite3_column_type(candidates.p, 4) == SQLITE_NULL) continue;
            }
            const auto root = Column(candidates.p, 3);
            // Existing databases can contain our history/configuration from
            // older versions. Never surface those rows while pruning catches up.
            if (InternalContentPath(path)) continue;
            if (std::none_of(live_roots.begin(), live_roots.end(), [&](const auto& live) { return Same(root, live); })) continue;
            if (!r.root.empty() && !Under(path, r.root, r.recursive)) continue;
            if (!r.roots.empty() && std::none_of(r.roots.begin(), r.roots.end(), [&](const auto& scope) { return Under(path, scope, r.recursive); })) continue;
            const uint64_t size = static_cast<uint64_t>(sqlite3_column_int64(candidates.p, 1));
            const uint64_t modified = static_cast<uint64_t>(sqlite3_column_int64(candidates.p, 2));
            const auto dot = path.find_last_of(L'.');
            const auto extension = dot == std::wstring::npos ? std::wstring_view{} : std::wstring_view(path).substr(dot);
            const auto maximum = IsExtractedDocumentExtension(extension) ? r.maximum_document_bytes : r.maximum_file_bytes;
            if (size < r.minimum_file_bytes || size > maximum || !MatchContentFilename(path, size, modified, filename_query)) continue;
            // Sort metadata only. Fetching bodies after filtering prevents a
            // broad sorted query from spilling gigabytes into the SQL sorter.
            if (!cached.p) { progress.error = ERROR_DATABASE_FAILURE; ok = false; break; }
            const auto fetch_start = clock();
            sqlite3_reset(cached.p);
            cached.Int(1, static_cast<uint64_t>(sqlite3_column_int64(candidates.p, 4)));
            if (!cached.p || sqlite3_step(cached.p) != SQLITE_ROW) { progress.error = ERROR_DATABASE_FAILURE; ok = false; break; }
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(cached.p, 0));
            const auto text_bytes = static_cast<size_t>(sqlite3_column_bytes(cached.p, 0));
            fetch_ticks += clock() - fetch_start;
            while (!pending.empty() && (pending.size() >= 8 || pending_bytes + text_bytes > 8 * 1024 * 1024)) {
                if (!consume()) { ok = false; break; }
            }
            if (!ok) break;
            std::string body(text ? text : "", text_bytes);
            body_pending = true;
            const bool parallel = !progress.delta && checked_bodies != 0 && text_bytes >= 32768;
            if (parallel && !workers) workers = std::make_unique<QueryMatchWorkers>();
            const auto* cancellation = parallel ? &workers->cancelled : &cancel;
            const auto id = static_cast<uint64_t>(sqlite3_column_int64(candidates.p, 4));
            auto match = [&, path, size, modified, id, body = std::move(body), cancellation] {
                QueryMatch result; const auto start = clock();
                if (!cancellation->load()) {
                    const auto decoded = Wide(body.data(), static_cast<int>(body.size()));
                    const auto found = MatchCachedContent(decoded, r, cancellation);
                    if (found != std::wstring::npos && !cancellation->load()) {
                        result.hit = MakeCachedContentHit(path, size, modified, decoded, found);
                        result.hit->file_id = id;
                    }
                }
                result.ticks = clock() - start; return result;
            };
            ++checked_bodies;
            if (parallel) {
                pending_bytes += text_bytes;
                pending.push_back({workers->Submit(std::packaged_task<QueryMatch()>(std::move(match))), text_bytes, size});
            } else {
                // Drain preceding jobs before synchronous rows to retain the
                // database's global sort order, including mixed-size files.
                while (!pending.empty()) { if (!consume()) { ok = false; break; } }
                if (!ok || !complete(match(), size)) { ok = false; break; }
            }
        }
        while (ok && !cancel.load() && !pending.empty()) { if (!consume()) ok = false; }
        // No decoded bodies or worker threads outlive this query, even when a
        // callback stops early or a result limit is reached.
        workers.reset();
        if (progress.truncated) ok = true;
        if (!candidates.p || (rc != SQLITE_ROW && rc != SQLITE_DONE && !cancel.load())) { progress.error = ERROR_DATABASE_FAILURE; ok = false; }
        if (!batch.empty() && ok) ok = callback(progress, std::move(batch));
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        TraceSearch("content_candidate_us", static_cast<uint64_t>(candidate_ticks * 1000000 / frequency.QuadPart));
        TraceSearch("content_fetch_us", static_cast<uint64_t>(fetch_ticks * 1000000 / frequency.QuadPart));
        TraceSearch("content_match_us", static_cast<uint64_t>(match_ticks * 1000000 / frequency.QuadPart));
    }
    catch (const std::bad_alloc&) { progress.error = ERROR_NOT_ENOUGH_MEMORY; ok = false; }
    catch (const std::system_error&) { progress.error = ERROR_NOT_ENOUGH_MEMORY; ok = false; }
    catch (...) { progress.error = ERROR_GEN_FAILURE; ok = false; }
    if (!snapshot) { Exec(db, "COMMIT"); sqlite3_close(db); }
    TraceSearch("content_query", progress.index_revision);
    progress.done = true;
    if (cancel.load()) { progress.error = ERROR_CANCELLED; ok = false; }
    callback(progress, {}); return ok;
}
bool ContentIndex::SearchTask(const ContentSearchRequest& request, const std::atomic<bool>& cancel, ContentBatchCallback callback) {
    if (request.after_revision || request.mode != ContentSearchMode::Content) return Search(request, cancel, std::move(callback));
    ContentTiming timing(request.generation);
    const auto cache_started = ContentTiming::NowMicros();
    const auto config = Configuration();
    struct Version { uint64_t size = 0, modified = 0; std::wstring value; };
    std::map<std::wstring, Version> versions;
    std::mutex versions_mutex;
    std::set<std::wstring> cached_positive_paths;
    std::unique_ptr<ReadSlot> read_slot;
    struct Prefetch {
        std::thread worker;
        std::atomic<bool> stop{false}, done{false};
        const std::atomic<bool>& cancel;
        std::mutex database_mutex;
        sqlite3* database = nullptr;
        explicit Prefetch(const std::atomic<bool>& cancelled) : cancel(cancelled) {}
        void Stop() {
            stop = true;
            {
                std::lock_guard lock(database_mutex);
                if (database) sqlite3_interrupt(database);
            }
            if (worker.joinable()) worker.join();
        }
        ~Prefetch() { Stop(); }
    } prefetch(cancel);
    ContentSearchProgress progress; progress.generation = request.generation; progress.live = request.subscribe;
    size_t hits = 0;
    bool accepted = true;
    auto deliver = [&](const ContentSearchProgress& state, std::vector<ContentHit> batch) {
        if (prefetch.done || state.done || cancel) { prefetch.Stop(); read_slot.reset(); }
        try {
            if (accepted) accepted = callback(state, std::move(batch));
        } catch (...) {
            accepted = false;
            prefetch.Stop(); read_slot.reset();
            throw;
        }
        if (!accepted) { prefetch.Stop(); read_slot.reset(); }
        return accepted;
    };
    auto excluded = [&](const std::wstring& path) { return ExcludedContentPath(config, path); };
    auto fresh = [&](const std::wstring& path, uint64_t size, uint64_t modified, const std::wstring& version) {
        const auto key = ContentScopeKey(path);
        std::lock_guard lock(versions_mutex);
        const auto found = versions.find(key);
        return found != versions.end() && found->second.size == size && found->second.modified == modified &&
            !version.empty() && found->second.value == version;
    };
    if (!deliver(progress, {})) {
        timing.Finish(0, 0, 0, true, ERROR_CANCELLED);
        return false;
    }
    // Changed-file tasks already have a bounded candidate list. Reading the
    // full old cache before filtering those paths made every delta pay the
    // startup cost of a whole search. Read those live files directly instead.
    bool compatible_cache = false;
    {
        sqlite3* cache_db = nullptr;
        if (Open(impl_->path, &cache_db, true)) {
            const auto cached_config = ReadConfig(cache_db);
            compatible_cache = cached_config.default_encoding == config.default_encoding &&
                std::all_of(config.roots.begin(), config.roots.end(), [&](const auto& root) {
                    return std::any_of(cached_config.roots.begin(), cached_config.roots.end(), [&](const auto& cached) {
                        return Same(root.path, cached.path) && root.encoding == cached.encoding;
                    });
                });
            sqlite3_close(cache_db);
        }
    }
    if (request.candidate_paths.empty() && compatible_cache) try {
        read_slot = std::make_unique<ReadSlot>(impl_->path, &cancel);
        sqlite3* db = nullptr;
        struct Close { sqlite3*& db; ~Close() { if (db) { Exec(db,"ROLLBACK"); sqlite3_close(db); } } } close{db};
        bool prefetch_negatives = false;
        if (!read_slot->acquired || !Open(impl_->path, &db, true) || !Exec(db,"BEGIN")) {
            // A first-run instant search does not need a persistent body cache.
            progress.error = cancel ? ERROR_CANCELLED : impl_->mode == ContentAgentMode::Instant ? ERROR_SUCCESS : ERROR_DATABASE_FAILURE;
        } else {
            // Only this snapshot opts into a bounded read-only mapping.
            MEMORYSTATUSEX memory{sizeof(memory)};
            const uint64_t mapping_budget = GlobalMemoryStatusEx(&memory)
                ? (std::min)(16ull * 1024 * 1024 * 1024, memory.ullAvailPhys / 2) : 0;
            const auto mapping_sql = "PRAGMA mmap_size=" + std::to_string(mapping_budget);
            Exec(db, mapping_sql.c_str());
            Statement mapping(db, "PRAGMA mmap_size");
            const uint64_t effective_mapping = mapping.p && sqlite3_step(mapping.p) == SQLITE_ROW
                ? static_cast<uint64_t>(sqlite3_column_int64(mapping.p, 0)) : 0;
            sqlite3_reset(mapping.p);
            timing.SetCacheMapping(mapping_budget, effective_mapping);
            sqlite3_progress_handler(db, 512, [](void* value) { return static_cast<const std::atomic<bool>*>(value)->load() ? 1 : 0; }, const_cast<std::atomic<bool>*>(&cancel));
            // Positive results are validated and delivered before optional
            // negative-version metadata traverses body overflow pages.
            Statement hit_version(db, "SELECT size,modified,version FROM documents WHERE path=?1");
            bool metadata_ok = hit_version.p != nullptr;
            if (metadata_ok) {
                auto cached_request = request;
                // Stale hits discarded below must not consume the caller's cap.
                cached_request.maximum_hits = 0;
                const bool queried = SearchCached(cached_request, cancel, [&](const auto& state, auto batch) {
                    if (!accepted) return false;
                    progress = state; progress.done = false; progress.total_files = 0;
                    const auto previous_hits = hits;
                    for (auto& hit : batch) {
                        // A positive that failed live validation must still be
                        // scanned if a transient file-access failure recovers.
                        cached_positive_paths.insert(ContentScopeKey(hit.path));
                        if (excluded(hit.path) || std::none_of(config.roots.begin(), config.roots.end(),
                            [&](const auto& root) { return Under(hit.path, root.path); })) continue;
                        WIN32_FILE_ATTRIBUTE_DATA data{};
                        if (!GetFileAttributesExW(LongPath(hit.path).c_str(),GetFileExInfoStandard,&data) ||
                            (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE|::pulse::fs::kCloudPlaceholderAttrs))) continue;
                        const uint64_t size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
                        sqlite3_reset(hit_version.p);
                        hit_version.Text(1, hit.path);
                        if (sqlite3_step(hit_version.p) != SQLITE_ROW) continue;
                        const auto version = FileVersion(hit.path);
                        if (version.empty() || size != static_cast<uint64_t>(sqlite3_column_int64(hit_version.p, 0)) ||
                            Stamp(data.ftLastWriteTime) != static_cast<uint64_t>(sqlite3_column_int64(hit_version.p, 1)) ||
                            version != Column(hit_version.p, 2)) continue;
                        if (request.maximum_hits && hits >= request.maximum_hits) { progress.truncated=true; break; }
                        versions.emplace(ContentScopeKey(hit.path), Version{size, Stamp(data.ftLastWriteTime), version});
                        hit.file_id = 0;
                        ++hits;
                        if (request.maximum_hits && hits >= request.maximum_hits) progress.truncated = true;
                        timing.FlushProgress(0, 0, hits);
                        deliver(progress, {std::move(hit)});
                        if (!accepted || cancel || progress.truncated) break;
                    }
                    timing.FlushProgress(0, 0, hits);
                    if (hits == previous_hits && accepted) deliver(progress, {});
                    return accepted && !(request.maximum_hits && hits >= request.maximum_hits);
                },db);
                prefetch_negatives = queried && accepted && !cancel && !progress.error && !progress.truncated &&
                    request.candidate_paths.empty() && !(request.maximum_hits && hits >= request.maximum_hits);
                if (!queried && !progress.error && accepted && !cancel &&
                    !(request.maximum_hits && hits >= request.maximum_hits)) progress.error = ERROR_DATABASE_FAILURE;
            }
            TraceSearch("content_task_cache_ready", request.generation);
            timing.RecordStage(ContentTimingStage::CacheWait, cache_started);
            timing.FlushProgress(0, 0, hits, true);
            if (!metadata_ok) { versions.clear(); if (!cancel) progress.error = ERROR_DATABASE_FAILURE; }
        }
        // All caller-owned statements have been finalized. Transfer this exact
        // transaction, rather than opening a newer snapshot after the hit query.
        if (prefetch_negatives) {
            sqlite3_progress_handler(db, 512, [](void* value) {
                const auto& state = *static_cast<Prefetch*>(value);
                return state.stop || state.cancel ? 1 : 0;
            }, &prefetch);
            prefetch.database = db;
            try {
                prefetch.worker = std::thread([&, database = db] {
                    const auto started = ContentTiming::NowMicros();
                    try {
                        Statement rows(database, "SELECT path,size,modified,version FROM documents");
                        while (!prefetch.stop && !cancel && rows.p && sqlite3_step(rows.p) == SQLITE_ROW) {
                            const auto key = ContentScopeKey(Column(rows.p, 0));
                            if (cached_positive_paths.contains(key)) continue;
                            Version value{static_cast<uint64_t>(sqlite3_column_int64(rows.p, 1)),
                                static_cast<uint64_t>(sqlite3_column_int64(rows.p, 2)), Column(rows.p, 3)};
                            if (value.value.empty()) continue;
                            std::lock_guard lock(versions_mutex);
                            versions.emplace(key, std::move(value));
                        }
                    } catch (...) { /* Optional metadata: unprepared paths are scanned. */ }
                    {
                        std::lock_guard lock(prefetch.database_mutex);
                        sqlite3_progress_handler(database, 0, nullptr, nullptr);
                        Exec(database, "ROLLBACK");
                        sqlite3_close(database);
                        prefetch.database = nullptr;
                    }
                    timing.RecordStage(ContentTimingStage::MetadataPrepare, started);
                    prefetch.done = true;
                });
                db = nullptr;
            } catch (...) {
                prefetch.database = nullptr;
                // Thread creation failure only disables this optimization.
            }
        }
    } catch (const std::bad_alloc&) { progress.error = ERROR_NOT_ENOUGH_MEMORY; }
      catch (...) { progress.error = ERROR_GEN_FAILURE; }
    if (!prefetch.worker.joinable()) read_slot.reset();
    if (!accepted) { timing.Finish(0, 0, hits, true, ERROR_CANCELLED); return false; }
    if (cancel || (request.maximum_hits && hits >= request.maximum_hits)) {
        progress.done=true; progress.truncated=!cancel; if(cancel) progress.error=ERROR_CANCELLED;
        timing.Finish(0, 0, hits, cancel, progress.error);
        deliver(progress,{}); return !cancel;
    }
    ContentTaskCache cache{FileVersion, excluded, fresh, true};
    const bool completed = RunContentTaskSupplement(config,request,cancel,cache,progress,hits,deliver, {}, &timing);
    return completed && accepted;
}
} // namespace pulse::index
