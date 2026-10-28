#include "../index/content_index.h"
#include "../index/content_search_client.h"
#include "../common/text_decode.h"
#include "../index/document_reader.h"
#include "../index/content_scope.h"
#include "../index/index_protocol.h"
#include "../index/index_feed.h"
#include "../index/content_task_search.h"
#include "../index/content_search_protocol.h"
#include "../index/content_search_session.h"
#include <sddl.h>
#include <filesystem>
#include <cstdio>
#include <algorithm>
#include <psapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <set>
#include "../../third_party/sqlite/sqlite3.h"
using namespace pulse;
namespace {
int passed = 0, failed = 0;
void Check(bool ok, const char* name) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); fflush(stdout); ++(ok ? passed : failed); }
bool Write(const std::filesystem::path& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0; bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) && n == bytes.size(); CloseHandle(h); return ok;
}
std::vector<index::ContentHit> Search(index::ContentIndex& db, index::ContentSearchRequest r, index::ContentSearchProgress* final = nullptr) {
    std::vector<index::ContentHit> hits; std::atomic<bool> cancel{false};
    db.Search(r, cancel, [&](const auto& progress, auto batch) {
        if (final) *final = progress;
        hits.insert(hits.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end())); return true;
    }); return hits;
}
bool AwaitHit(index::ContentIndex& db, index::ContentSearchRequest r, size_t expected) {
    for (int i = 0; i < 100; ++i) { if (Search(db, r).size() == expected) return true; Sleep(100); } return false;
}
}

int RunLatencyTest(const std::filesystem::path& base) {
    const auto root = base / L"latency-files";
    const auto profile = base / L"profile";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(profile);
    for (int i = 0; i < 256; ++i)
        Write(root / (std::to_wstring(i) + L".txt"), "20 latency_marker\n" + std::string(4096, 'x'));
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    {
        index::ContentIndex db((base / L"latency.sqlite").wstring());
        Check(db.Configure(config) && db.WaitUntilIdle(30000), "latency fixture indexed");
        for (const auto sort : {index::ContentResultSort::Index, index::ContentResultSort::Name}) {
            index::ContentSearchRequest r; r.needle = L"20"; r.sort = sort;
            std::atomic<bool> cancel{false}; size_t hits = 0, first_batch = 0;
            ULONGLONG first_ms = 0; bool done = false; DWORD error = 0;
            const auto start = GetTickCount64();
            db.Search(r, cancel, [&](const auto& progress, auto batch) {
                if (!batch.empty() && !first_batch) { first_batch = batch.size(); first_ms = GetTickCount64() - start; }
                hits += batch.size(); done = progress.done; error = progress.error; return true;
            });
            printf("sort=%d first_batch=%zu first_ms=%llu total_ms=%llu hits=%zu\n", static_cast<int>(sort), first_batch, first_ms, GetTickCount64() - start, hits);
            Check(done && !error && hits == 256, "two-character indexed query preserves all matches");
            Check(first_batch == 1, "first match arrives without waiting for 64 matches");
            r.maximum_hits = 3;
            index::ContentSearchProgress final;
            const auto capped = Search(db, r, &final);
            Check(capped.size() == 3 && final.truncated, "streaming preserves result limit");
        }
        std::atomic<bool> cancel{false}; size_t hits = 0; bool done = false; DWORD error = 0;
        index::ContentSearchRequest r; r.needle = L"20";
        db.Search(r, cancel, [&](const auto& progress, auto batch) {
            hits += batch.size(); if (!batch.empty()) cancel = true;
            done = progress.done; error = progress.error; return true;
        });
        Check(hits == 1 && done && error == ERROR_CANCELLED, "cancel immediately after first visible result");
        r.needle = L"no_such_content";
        Check(Search(db, r).empty(), "completed no-match query stays empty");
    }
    wchar_t previous[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", previous, ARRAYSIZE(previous));
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    {
        index::ContentSearchClient client; client.Start(nullptr, 0); client.Configure(config);
        bool ready = false;
        for (int i = 0; i < 600; ++i) {
            const auto status = client.GetStatus();
            if (status.indexed_files == 256 && !status.indexing) { ready = true; break; }
            Sleep(25);
        }
        Check(ready, "isolated persistent client ready");
        auto request = [&](uint64_t generation) {
            index::ContentSearchRequest r; r.generation = generation; r.needle = L"20";
            const auto start = GetTickCount64(); client.SearchAsync(r);
            size_t hits = 0; bool done = false, stale = false; DWORD error = 0;
            ULONGLONG first_ms = 0;
            while (!done && GetTickCount64() - start < 10000) {
                index::ContentSearchUpdate update;
                while (client.TakeUpdate(update)) {
                    stale |= update.progress.generation != generation;
                    if (!hits && !update.hits.empty()) first_ms = GetTickCount64() - start;
                    hits += update.hits.size(); done = update.progress.done; error = update.progress.error;
                }
                if (!done) Sleep(5);
            }
            printf("generation=%llu first_ms=%llu total_ms=%llu hits=%zu error=%lu\n", generation, first_ms, GetTickCount64()-start, hits, error);
            return done && !error && !stale && hits == 256;
        };
        Check(request(100), "initial persistent content search");
        client.Cancel();
        Check(request(101), "adjacent history generation survives navigation cancellation");
        client.Cancel(); client.Cancel();
        Check(request(102), "repeated exit cancellation cannot poison next generation");
        for (uint64_t generation = 103; generation < 113; ++generation) {
            index::ContentSearchRequest r; r.generation = generation; r.needle = L"20";
            client.SearchAsync(r); client.Cancel();
        }
        Check(request(113), "rapid history switches deliver only latest results");
        client.Stop();
        client.Start(nullptr, 0, false);
        index::ContentSearchRequest transient; transient.indexed = false;
        transient.root = root.wstring(); transient.needle = L"20"; transient.generation = 114;
        client.SearchAsync(transient); client.Cancel(); client.Cancel();
        transient.generation = 115; client.SearchAsync(transient);
        size_t hits = 0; bool done = false, stale = false; DWORD error = 0;
        const auto start = GetTickCount64();
        while (!done && GetTickCount64() - start < 10000) {
            index::ContentSearchUpdate update;
            while (client.TakeUpdate(update)) {
                stale |= update.progress.generation != 115;
                hits += update.hits.size(); done = update.progress.done; error = update.progress.error;
            }
            if (!done) Sleep(5);
        }
        Check(done && !stale && !error && hits == 256, "shared cancellation also preserves explicit transient scans");
        client.Stop();
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", n ? previous : nullptr);
    printf("%d passed, %d failed\n", passed, failed);
    wprintf(L"Fixture: %s\n", base.c_str());
    return failed ? 1 : 0;
}

int RunAvailabilityTest(const std::filesystem::path& base) {
    const auto root = base / L"files";
    const auto other = base / L"other";
    const auto offline = base / L"offline";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(other);
    Check(Write(root / L"one.txt", "contract") && Write(other / L"two.txt", "contract"), "create coverage fixtures");
    {
        index::ContentIndex db((base / L"coverage.sqlite").wstring());
        index::ContentIndexConfig config; config.roots = {{root.wstring()}, {other.wstring()}};
        Check(db.Configure(config) && db.WaitUntilIdle(10000), "coverage fixture indexed");
        db.Pause(true);
        index::ContentSearchRequest request; request.needle = L"contract";
        index::ContentSearchProgress final;
        Check(Search(db, request, &final).size() == 2 && !final.error, "available roots return complete matches");
        std::filesystem::rename(root, offline);
        Check(Search(db, request, &final).size() == 1 && final.error == ERROR_NOT_READY,
            "unavailable root produces partial results rather than authoritative removal");
        request.root = root.wstring();
        Check(Search(db, request, &final).empty() && final.error == ERROR_NOT_READY,
            "offline search scope cannot report successful zero matches");
        request.root = other.wstring();
        Check(Search(db, request, &final).size() == 1 && !final.error,
            "unrelated unavailable root does not invalidate healthy scoped query");
        std::filesystem::rename(offline, root);
        request.root.clear();
        Check(Search(db, request, &final).size() == 2 && !final.error,
            "restored root returns complete matches again");
        request.needle = L"missing_marker";
        Check(Search(db, request, &final).empty() && !final.error, "healthy no-match query remains authoritative");
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

int RunPartialRootTest(const std::filesystem::path& base) {
    const auto root = base / L"files";
    const auto blocked = root / L"blocked";
    std::filesystem::create_directories(blocked);
    for (int i = 0; i < 9; ++i) Write(root / (std::to_wstring(i) + L".txt"), "contract");
    Write(blocked / L"hidden.txt", "contract");
    DWORD length = 0;
    GetFileSecurityW(blocked.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &length);
    std::vector<BYTE> security(length);
    Check(length && GetFileSecurityW(blocked.c_str(), DACL_SECURITY_INFORMATION,
        reinterpret_cast<PSECURITY_DESCRIPTOR>(security.data()), length, &length), "save isolated directory permissions");
    PSECURITY_DESCRIPTOR denied = nullptr;
    const bool protected_folder = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &denied, nullptr) &&
        SetFileSecurityW(blocked.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, denied);
    if (denied) LocalFree(denied);
    Check(protected_folder, "deny enumeration only in isolated child directory");
    if (protected_folder) {
        index::ContentIndex db((base / L"partial.sqlite").wstring());
        index::ContentIndexConfig config; config.roots = {{root.wstring()}};
        Check(db.Configure(config) && db.WaitUntilIdle(10000), "partial root scan completes");
        const auto status = db.Status();
        Check(status.errors && status.error == ERROR_ACCESS_DENIED, "child enumeration failure is reproduced");
        Check(status.indexed_files == 9, "nine healthy files remain indexed");
        Check(status.root_status.size() == 1 && status.root_status.front().available,
            "child failure does not mark accessible root unavailable");
        index::ContentSearchRequest request; request.needle = L"contract";
        Check(Search(db, request).size() == 9, "nine matches remain searchable after partial scan finishes");
    }
    Check(!security.empty() && SetFileSecurityW(blocked.c_str(),
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        reinterpret_cast<PSECURITY_DESCRIPTOR>(security.data())), "restore isolated directory permissions");
    const auto legacy = root / L"unsupported.xls";
    Write(legacy, "not a supported legacy workbook");
    std::wstring body; uint64_t bytes = 0; DWORD error = 0;
    const bool extracted = index::ReadSearchableDocument(legacy.wstring(), 1024, body, bytes, &error);
    printf("Legacy extractor error: %lu\n", error);
    Check(!extracted && error == ERROR_NOT_SUPPORTED, "real legacy extractor reproduces error 50");
    {
        index::ContentIndex db((base / L"unsupported.sqlite").wstring());
        index::ContentIndexConfig config; config.roots = {{root.wstring()}};
        Check(db.Configure(config) && db.WaitUntilIdle(10000), "unsupported format scan completes");
        Check(db.Status().skipped_files >= 1 && db.Status().errors == 0 && db.Status().error == 0,
            "unsupported document is skipped instead of failing the index");
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

int RunSharedScopeTest(const std::filesystem::path& base) {
    const auto root = base / L"files";
    const auto desktop = root / L"desktop";
    const auto excluded = root / L"excluded";
    const auto sibling = root / L"excluded-sibling";
    for (const auto& dir : {desktop, excluded, sibling}) std::filesystem::create_directories(dir);
    for (const auto& dir : {desktop, excluded, sibling}) Write(dir / L"hit.txt", "shared_contract");
    index::ContentIndexConfig old; old.roots = {{desktop.wstring()}};
    index::VolumeInfo enabled; enabled.enabled = enabled.supported = true; enabled.mount_point = root.wstring();
    auto disabled = enabled; disabled.enabled = false; disabled.mount_point = (base / L"disabled").wstring();
    auto unsupported = enabled; unsupported.supported = false; unsupported.mount_point = (base / L"unsupported").wstring();
    auto shared = index::SharedContentScope(old, {enabled, disabled, unsupported}, {excluded.wstring()});
    auto encoding_preference = shared; encoding_preference.default_encoding = text::Encoding::Utf8;
    encoding_preference.roots.clear();
    Check(index::SharedContentScope(encoding_preference, {enabled}, {}).roots.front().encoding == text::Encoding::Utf8,
        "newly enabled shared volumes inherit the chosen text encoding");
    Check(shared.shared_scope && shared.roots.size() == 1 && shared.roots[0].path == index::ContentScopeKey(root.wstring()) &&
        shared.excluded_directories.empty(), "legacy desktop selection is replaced by enabled shared volumes");
    Check(index::ContentPathExcluded(shared, (excluded / L"hit.txt").wstring()) &&
        !index::ContentPathExcluded(shared, (sibling / L"hit.txt").wstring()), "excluded paths respect directory boundaries");
    ipc::PayloadWriter writer; index::content::PutConfig(writer, shared);
    ipc::PayloadReader reader(writer.data().data(), writer.data().size()); index::ContentIndexConfig decoded;
    Check(index::content::GetConfig(reader, decoded) && decoded.shared_scope && decoded.excluded_paths == shared.excluded_paths,
        "shared scope and path exclusions survive the agent protocol");
    const auto database = (base / L"shared.sqlite").wstring();
    index::ContentSearchRequest query; query.needle = L"shared_contract";
    {
        index::ContentIndex db(database);
        Check(db.Configure(old) && db.WaitUntilIdle(10000) && Search(db, query).size() == 1, "old desktop-only configuration indexed");
        Check(db.Configure(shared) && db.WaitUntilIdle(10000) && Search(db, query).size() == 2,
            "scope migration keeps desktop matches and adds other shared locations");
        Write(excluded / L"new.txt", "shared_contract");
        Write(sibling / L"new.txt", "shared_contract");
        Check(AwaitHit(db, query, 3), "watcher admits enabled paths and ignores excluded paths");
        auto disabled_scope = index::SharedContentScope(shared, {disabled}, {});
        Check(db.Configure(disabled_scope) && db.WaitUntilIdle(10000) && Search(db, query).empty(),
            "disabling shared volumes removes their content coverage");
        Check(db.Configure(shared) && db.WaitUntilIdle(10000) && Search(db, query).size() == 3,
            "re-enabling shared scope restores its searchable documents");
    }
    {
        index::ContentIndex reopened(database);
        Check(reopened.WaitUntilIdle(10000) && reopened.Configuration().shared_scope &&
            reopened.Configuration().excluded_paths == shared.excluded_paths && Search(reopened, query).size() == 3,
            "shared scope and exclusions persist across restart");
    }
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

int RunRecoveryTest(const std::filesystem::path& base) {
    const auto root = base / L"files";
    std::filesystem::create_directories(root);
    index::ContentIndex db((base / L"recovery.sqlite").wstring());
    index::ContentIndexConfig config; config.roots = {{root.wstring()}};
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "recovery fixture ready");
    const auto file = root / L"locked.txt";
    HANDLE locked = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(locked != INVALID_HANDLE_VALUE, "hold isolated file exclusively");
    if (locked == INVALID_HANDLE_VALUE) return 1;
    const char body[] = "recovery_locked_marker";
    DWORD written = 0;
    Check(WriteFile(locked, body, sizeof(body) - 1, &written, nullptr) && written == sizeof(body) - 1,
          "write occupied fixture");
    auto wait = [&](auto ready, DWORD timeout) {
        const auto until = GetTickCount64() + timeout;
        while (GetTickCount64() < until) { if (ready()) return true; Sleep(25); }
        return false;
    };
    Check(wait([&] { return db.Status().error == ERROR_SHARING_VIOLATION; }, 20000),
          "exhausted sharing retries expose partial indexing error");
    const auto errors = db.Status().errors;
    index::ContentSearchRequest request; request.needle = L"unrelated_marker";
    Check(Write(root / L"other.txt", "unrelated_marker") && AwaitHit(db, request, 1),
          "unrelated file remains searchable while one file is occupied");
    Check(db.Status().error == ERROR_SHARING_VIOLATION, "unrelated success preserves occupied-file error");
    CloseHandle(locked);
    Check(Write(file, "recovery_locked_marker recovered"), "save previously occupied file after unlock");
    request.needle = L"recovery_locked_marker";
    Check(AwaitHit(db, request, 1) && wait([&] { return db.Status().error == ERROR_SUCCESS; }, 5000),
          "same-file successful commit clears current error");
    Check(errors > 0 && db.Status().errors == errors, "recovery preserves historical error count");
    return failed ? 1 : 0;
}

int RunInternalPathTest(const std::filesystem::path& base) {
    const auto profile = base / L"profile";
    const auto previous_profile = base / L"previous-profile";
    const auto internal = profile / L"Pulse";
    const auto adjacent = profile / L"PulseDocuments";
    std::filesystem::create_directories(internal);
    std::filesystem::create_directories(adjacent);
    std::filesystem::create_directories(previous_profile);
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", previous_profile.c_str()) != FALSE, "set legacy isolated profile");
    Check(Write(internal / L"search_history.json", "{\"query\":\"self_history_marker\"}"), "seed legacy history file");
    Check(Write(adjacent / L"notes.txt", "adjacent_user_marker"), "seed adjacent ordinary directory");
    index::ContentIndex db((base / L"internal.sqlite").wstring());
    index::ContentIndexConfig config; config.roots = {{profile.wstring()}};
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "legacy database indexed before internal scope changes");
    index::ContentSearchRequest request; request.needle = L"self_history_marker";
    Check(Search(db, request).size() == 1, "legacy internal row exists before query exclusion");
    // The existing database remains open: changing this test process's profile
    // exercises query-time exclusion without relying on reindexing or cleanup.
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str()) != FALSE, "select isolated current profile");
    Check(Search(db, request).empty(), "already-indexed internal history is filtered without clearing database");
    request.needle = L"adjacent_user_marker";
    Check(Search(db, request).size() == 1, "adjacent user directory is not excluded by prefix");
    const auto count = db.Status().indexed_files;
    Check(Write(internal / L"new_history.json", "new_internal_marker"), "write new internal history");
    Check(Write(adjacent / L"new.txt", "new_adjacent_marker"), "write new adjacent content");
    request.needle = L"new_adjacent_marker";
    Check(AwaitHit(db, request, 1) && db.WaitUntilIdle(10000), "process internal and adjacent notifications");
    request.needle = L"new_internal_marker";
    Check(Search(db, request).empty(), "new internal history never appears in query");
    Check(db.Status().indexed_files == count + 1, "only adjacent ordinary file is added to index");
    return failed ? 1 : 0;
}

int RunQueryTaskTest(const std::filesystem::path& base) {
    const auto root = base / L"query-files";
    const auto database = base / L"query.sqlite";
    const bool seed = !std::filesystem::exists(database);
    std::filesystem::create_directories(root);
    constexpr size_t count = 512;
    const std::string original_body = "\xe4\xb8\xad\xe6\x96\x87\xe6\x8a\xa5\xe5\x91\x8a ab bb ba " +
        std::string(128 * 1024, 'x') + " \xe5\xb0\xbe\xe9\x83\xa8\xe6\xa0\x87\xe8\xae\xb0";
    if (seed) {
        bool created = true;
        for (size_t i = 0; i < count; ++i) created &= Write(root / (std::to_wstring(i) + L".txt"), original_body);
        Check(created, "seed query task documents");
    }
    index::ContentIndex db(database.wstring());
    index::ContentIndexConfig config; config.roots = {{root.wstring()}};
    Check(db.Configure(config) && db.WaitUntilIdle(120000), "query task fixture ready");
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    auto clock = [] { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; };
    auto milliseconds = [&](LONGLONG ticks) { return static_cast<double>(ticks) * 1000.0 / frequency.QuadPart; };
    for (const auto& needle : {std::wstring(L"报告"), std::wstring(L"尾部标记"), std::wstring(L"abba")}) {
        for (const auto sort : {index::ContentResultSort::Index, index::ContentResultSort::Name}) {
            for (int repeat = 0; repeat < 3; ++repeat) {
                index::ContentSearchRequest request; request.needle = needle; request.sort = sort;
                std::atomic<bool> cancel{false}; size_t hits = 0, notifications = 0;
                uint64_t last_checked = 0; bool valid_progress = true;
                LONGLONG first = 0; index::ContentSearchProgress final;
                const auto start = clock();
                db.Search(request, cancel, [&](const auto& progress, auto batch) {
                    if (!batch.empty() && !first) first = clock();
                    valid_progress &= progress.scanned_files >= last_checked &&
                        (!progress.total_files || progress.scanned_files <= progress.total_files);
                    last_checked = progress.scanned_files;
                    hits += batch.size(); ++notifications; final = progress; return true;
                });
                printf("QUERY needle=%s sort=%d repeat=%d first_ms=%.3f total_ms=%.3f hits=%zu checked=%llu candidates=%llu notifications=%zu error=%lu\n",
                    needle == L"报告" ? "short-cjk" : needle == L"abba" ? "false-positive" : "tail-cjk",
                    static_cast<int>(sort), repeat, first ? milliseconds(first-start) : -1.0,
                    milliseconds(clock()-start), hits, final.scanned_files, final.total_files, notifications, final.error);
                Check(final.done && !final.error && hits == (needle == L"abba" ? 0 : count), "query task exact results");
                Check(final.scanned_files == count, "query task checks all gram candidates");
                Check(valid_progress && final.total_files == count, "candidate progress is monotonic and exact");
            }
        }
    }
    index::ContentSearchRequest request; request.needle = L"abba";
    std::atomic<bool> cancel{false}; index::ContentSearchProgress final;
    const auto start = clock();
    std::thread cancellation([&] { Sleep(25); cancel = true; });
    db.Search(request, cancel, [&](const auto& progress, auto) { final = progress; return true; });
    const auto finished = clock(); cancellation.join();
    printf("CANCEL total_ms=%.3f checked=%llu error=%lu\n", milliseconds(finished-start), final.scanned_files, final.error);
    Check(final.done && (final.error == ERROR_CANCELLED || milliseconds(finished-start) < 25), "bounded query cancellation");
    auto threads = [] {
        size_t thread_count = 0; HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{sizeof(entry)};
        if (snapshot != INVALID_HANDLE_VALUE) {
            if (Thread32First(snapshot, &entry)) do { if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++thread_count; } while (Thread32Next(snapshot, &entry));
            CloseHandle(snapshot);
        }
        return thread_count;
    };
    struct Resources { SIZE_T working = 0, committed = 0; uint64_t cpu = 0; };
    auto resources = [] {
        Resources value; PROCESS_MEMORY_COUNTERS_EX memory{};
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            value.working = memory.WorkingSetSize; value.committed = memory.PrivateUsage;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            value.cpu = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) + kernel.dwLowDateTime +
                (static_cast<uint64_t>(user.dwHighDateTime) << 32) + user.dwLowDateTime;
        return value;
    };
    for (bool cancel_task : {false, true}) {
        const auto before = resources(); const auto before_threads = threads();
        std::atomic<bool> stop_sampling{false}, task_cancel{false};
        SIZE_T peak_working = before.working, peak_committed = before.committed;
        size_t peak_threads = before_threads;
        const auto resource_start = clock();
        std::thread sampler([&] {
            while (!stop_sampling) {
                const auto sample = resources(); peak_working = (std::max)(peak_working, sample.working);
                peak_committed = (std::max)(peak_committed, sample.committed);
                peak_threads = (std::max)(peak_threads, threads());
                if (cancel_task && milliseconds(clock()-resource_start) >= 25) task_cancel = true;
                Sleep(5);
            }
        });
        db.Search(request, task_cancel, [&](const auto&, auto) { return true; });
        stop_sampling = true; sampler.join();
        const auto after = resources(); const auto after_threads = threads();
        printf("RESOURCES cancel=%d cpu_ms=%.3f working_before=%zu working_peak=%zu working_after=%zu commit_before=%zu commit_peak=%zu commit_after=%zu threads_before=%zu threads_peak=%zu threads_after=%zu\n",
            cancel_task, static_cast<double>(after.cpu-before.cpu)/10000, before.working, peak_working, after.working,
            before.committed, peak_committed, after.committed, before_threads, peak_threads, after_threads);
        Check(after_threads <= before_threads, "query workers are joined before return");
    }
    request.needle = L"报告"; request.sort = index::ContentResultSort::Name;
    const auto ordered = Search(db, request);
    Check(std::is_sorted(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }), "parallel results retain global name order");
    request.maximum_hits = 3;
    const auto capped = Search(db, request, &final);
    Check(capped.size() == 3 && final.truncated && capped[0].path == ordered[0].path && capped[2].path == ordered[2].path,
          "parallel limit retains first sorted hits and truncation");
    request.maximum_hits = 0; request.needle = L"AB"; request.case_sensitive = true;
    Check(Search(db, request).empty(), "case-sensitive gram candidates remain non-matches");
    request.case_sensitive = false;
    Check(Search(db, request).size() == count, "case-insensitive matches preserved");
    request.needle = L"a"; request.whole_word = true;
    Check(Search(db, request).empty(), "whole-word rejects embedded unigram candidates");
    request.whole_word = false; request.needle = L"尾部标记"; request.excluded_needles = {L"报告"};
    Check(Search(db, request).empty(), "excluded terms override positive matches");
    request.excluded_needles.clear(); request.needle = L"报告";
    std::atomic<bool> first_cancel{false}; size_t first_hits = 0;
    db.Search(request, first_cancel, [&](const auto& progress, auto batch) {
        first_hits += batch.size(); if (!batch.empty()) first_cancel = true; final = progress; return true;
    });
    Check(first_hits == 1 && final.error == ERROR_CANCELLED, "first-result cancellation preserves immediate delivery");
    std::atomic<bool> snapshot_cancel{false}; bool changed = false; size_t snapshot_hits = 0;
    db.Search(request, snapshot_cancel, [&](const auto&, auto batch) {
        snapshot_hits += batch.size();
        if (!changed && !batch.empty()) {
            changed = true;
            Check(Write(root / L"511.txt", "snapshot_new_marker"), "change document during query snapshot");
            index::ContentSearchRequest latest; latest.needle = L"snapshot_new_marker";
            Check(AwaitHit(db, latest, 1), "writer commits while original query snapshot is open");
        }
        return true;
    });
    Check(snapshot_hits == count, "parallel workers use bodies from one SQLite snapshot");
    Check(Write(root / L"511.txt", original_body) && AwaitHit(db, request, count), "restore comparison fixture");
    index::ContentSearchRequest delta; delta.needle = L"delta_task_marker"; delta.subscribe = true;
    Search(db, delta, &final); delta.after_revision = final.index_revision;
    const auto delta_path = root / L"delta.txt";
    Check(Write(delta_path, "delta_task_marker") && AwaitHit(db, delta, 1), "create delta fixture");
    const auto added = Search(db, delta, &final);
    Check(final.delta && added.size() == 1 && !added[0].removed && added[0].path == delta_path.wstring(), "delta preserves added match");
    delta.after_revision = final.index_revision;
    Check(DeleteFileW(delta_path.c_str()) != FALSE, "delete delta fixture");
    index::ContentSearchRequest empty; empty.needle = delta.needle;
    Check(AwaitHit(db, empty, 0), "deleted delta fixture leaves current snapshot");
    const auto removed = Search(db, delta, &final);
    Check(final.delta && removed.size() == 1 && removed[0].removed, "delta preserves tombstone without body read");
    std::wprintf(L"Query fixture: %ls\n", base.c_str());
    return failed ? 1 : 0;
}

int RunTaskHintTest(const std::filesystem::path& base) {
    const auto root = base / L"hint-files";
    std::filesystem::create_directories(root);
    index::ContentIndex db((base / L"hints.sqlite").wstring());
    index::ContentIndexConfig config;
    config.roots = {{root.wstring(), text::Encoding::Auto}};
    Check(db.Configure(config) && db.WaitUntilIdle(10000), "initialize empty hint fixture cache");
    db.Pause(true);
    const auto phrase = root / L"phrase.rtf";
    const auto excluded = root / L"excluded.rtf";
    const auto boundary = root / L"boundary.rtf";
    Check(Write(phrase, "{\\rtf1\\ansi wrong " + std::string(16000, 'x') + " right phrase}") &&
        Write(excluded, "{\\rtf1\\ansi wanted " + std::string(16000, 'x') + " forbidden}") &&
        Write(boundary, "{\\rtf1\\ansi " + std::string(4090, 'x') + " tokenSuffix}"),
        "create real RTF hint semantic fixtures beyond first extraction block");
    std::wstring prefix;
    uint64_t bytes = 0;
    DWORD error = 0;
    {
        index::DocumentReadSession session;
        Check(index::ReadSearchableDocument(phrase.wstring(), 1024 * 1024, prefix, bytes, &error,
            text::Encoding::Auto, {}, L"wrong") && prefix.find(L"wrong") != std::wstring::npos &&
            prefix.find(L"right phrase") == std::wstring::npos,
            "wrong single-word hint demonstrably truncates before requested phrase");
        prefix.clear();
        Check(index::ReadSearchableDocument(boundary.wstring(), 1024 * 1024, prefix, bytes, &error,
            text::Encoding::Auto, {}, L"token") && prefix.ends_with(L"token"),
            "whole-word prefix lies exactly at a real extractor block boundary");
    }
    auto query = [&](index::ContentSearchRequest request, const std::filesystem::path& file, size_t expected, const char* name) {
        request.candidate_paths = {file.wstring()};
        std::atomic<bool> cancelled{false};
        size_t hits = 0, done = 0;
        index::ContentSearchProgress final;
        const bool ok = db.SearchTask(request, cancelled, [&](const auto& progress, auto batch) {
            hits += batch.size();
            done += progress.done ? 1 : 0;
            final = progress;
            return true;
        });
        if (!ok || hits != expected || final.error) printf("hint case hits=%zu expected=%zu error=%lu\n", hits, expected, final.error);
        Check(ok && hits == expected && done == 1 && !final.error, name);
    };
    index::ContentSearchRequest request;
    request.match_mode = index::ContentMatchMode::Phrase;
    request.needle = L"right phrase";
    request.needles = {L"wrong"};
    query(request, phrase, 1, "task phrase hint honors complete needle before conflicting needles vector");
    request = {};
    request.needle = L"wanted";
    request.excluded_needles = {L"forbidden"};
    query(request, excluded, 0, "tail exclusion is still read and prevents an early positive false hit");
    request = {};
    request.needle = L"token";
    request.whole_word = true;
    query(request, boundary, 0, "whole-word verification sees suffix beyond extraction block boundary");
    Check(db.Status().paused && db.Status().indexed_files == 0, "hint tests use supplemental extraction without populating cache");
    printf("Task hint fixture: %ls\n", base.c_str());
    return failed ? 1 : 0;
}

int RunTaskRootFairnessTest(const std::filesystem::path& base, bool same_root = false, const wchar_t* extension = L".pdf") {
    const bool fair = wcscmp(extension, L".pdf") == 0;
    const auto slow_root = base / L"root-fair-slow", small_root = base / L"root-fair-small";
    std::filesystem::create_directories(slow_root);
    std::filesystem::create_directories(small_root);
    index::ContentIndexConfig config;
    config.roots = {{slow_root.wstring()}, {(base / L"empty-root").wstring()}, {small_root.wstring()}};
    if (same_root) config.roots = {{base.wstring()}};
    index::ContentSearchRequest request; request.needle = L"root_fair_marker";
    constexpr unsigned slow_count = 48, small_count = 2;
    bool seeded = true;
    for (unsigned i = 0; i < slow_count + small_count; ++i) {
        const auto path = (i < slow_count ? slow_root : small_root) / (std::to_wstring(i) + extension);
        seeded &= Write(path, "fixture");
        request.candidate_paths.push_back(path.wstring());
    }
    request.candidate_paths.push_back(request.candidate_paths.back());
    Check(seeded, !fair ? "non-PDF FIFO fixture queues first root before second root" :
        same_root ? "directory fairness fixture queues large PDF subdirectory before small sibling" :
        "root fairness fixture queues large PDF root before small PDF root");
    index::ContentTaskCache cache;
    cache.version = [](const auto&) { return std::wstring(L"stable"); };
    cache.excluded = [](const auto&) { return false; };
    cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
    std::atomic<bool> cancel{false}, release{false};
    std::atomic<unsigned> active{0}, slow_completed{0}, first_small{UINT_MAX};
    index::ContentTaskReader reader = [&](const std::wstring& path, uint64_t, std::wstring& body,
        uint64_t& bytes, DWORD* error, text::Encoding, const auto& interrupted) {
        ++active;
        const bool slow = path.find(L"root-fair-slow") != std::wstring::npos;
        // All candidates must be admitted before any slow reader can complete;
        // the assertion then measures consumption order, not enumeration speed.
        while (!release && !interrupted()) Sleep(1);
        if (!slow) {
            unsigned unset = UINT_MAX;
            first_small.compare_exchange_strong(unset, slow_completed.load());
        }
        const auto until = GetTickCount64() + (slow ? 20 : 0);
        while (!interrupted() && GetTickCount64() < until) Sleep(1);
        --active;
        if (interrupted()) { *error = ERROR_CANCELLED; return false; }
        if (slow) ++slow_completed;
        body = L"root_fair_marker"; bytes = 16; return true;
    };
    std::set<std::wstring> hits;
    size_t emitted = 0;
    index::ContentSearchProgress final;
    const bool ok = index::RunContentTaskSupplement(config, request, cancel, cache, {}, 0,
        [&](const auto& progress, auto batch) {
            if (progress.total_files == slow_count + small_count) release = true;
            for (const auto& hit : batch) { hits.insert(hit.path); ++emitted; }
            final = progress; return true;
        }, reader);
    printf("%s_FAIR first_small_after_slow=%u total_slow=%u emitted=%zu\n",
        !fair ? "FIFO" : same_root ? "DIRECTORY" : "ROOT", first_small.load(), slow_completed.load(), emitted);
    if (fair)
        Check(first_small < slow_count / 2, same_root ? "small PDF subdirectory progresses before large sibling approaches exhaustion" :
            "small PDF root progresses before large root approaches exhaustion");
    else
        Check(first_small >= slow_count / 2, "Office and text keep global FIFO instead of root rotation");
    Check(ok && final.done && final.total_files == slow_count + small_count &&
        final.scanned_files == slow_count + small_count && emitted == slow_count + small_count && hits.size() == emitted,
        "root rotation retains every candidate once and skips empty configured roots");
    release = false;
    const auto started = GetTickCount64();
    const bool stopped = index::RunContentTaskSupplement(config, request, cancel, cache, {}, 0,
        [&](const auto& progress, auto) { if (active) cancel = true; final = progress; return true; }, reader);
    Check(!stopped && final.error == ERROR_CANCELLED && active == 0 && GetTickCount64() - started < 2000,
        "root rotation cancellation joins active readers and queued roots");
    return failed ? 1 : 0;
}

int RunTaskBalanceTest(const std::filesystem::path& base) {
    const auto root = base / L"balanced-lanes"; std::filesystem::create_directories(root);
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    index::ContentSearchRequest request; request.needle = L"balance_marker";
    for (unsigned i = 0; i < 36; ++i) {
        const auto path = root / (std::to_wstring(i) + L".pdf");
        Write(path, "fixture"); request.candidate_paths.push_back(path.wstring());
    }
    for (const auto* name : {L"office.rtf", L"plain.txt"}) {
        const auto path = root / name; Write(path, "fixture"); request.candidate_paths.push_back(path.wstring());
    }
    index::ContentTaskCache cache;
    cache.version = [](const auto&) { return std::wstring(L"stable"); };
    cache.excluded = [](const auto&) { return false; };
    cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
    std::atomic<bool> cancel{false}; std::atomic<unsigned> active{0}, peak{0}, pdf_active{0}, pdf_peak{0};
    auto update_peak = [](auto& value, unsigned next) {
        auto old = value.load(); while (old < next && !value.compare_exchange_weak(old, next)) {}
    };
    index::ContentTaskReader reader = [&](const std::wstring& path, uint64_t, std::wstring& body,
        uint64_t& bytes, DWORD* error, text::Encoding, const auto& interrupted) {
        const bool pdf = std::filesystem::path(path).extension() == L".pdf";
        update_peak(peak, ++active);
        if (pdf) update_peak(pdf_peak, ++pdf_active);
        const auto until = GetTickCount64() + (pdf ? 40 : 1);
        while (!interrupted() && GetTickCount64() < until) Sleep(1);
        if (pdf) --pdf_active;
        --active;
        if (interrupted()) { *error = ERROR_CANCELLED; return false; }
        body = L"balance_marker"; bytes = 14; return true;
    };
    const auto started = GetTickCount64();
    std::set<std::wstring> hits; index::ContentSearchProgress final;
    const bool ok = index::RunContentTaskSupplement(config, request, cancel, cache, {}, 0,
        [&](const auto& progress, auto batch) { final = progress; for (const auto& hit : batch) hits.insert(hit.path); return true; }, reader);
    printf("BALANCE elapsed_ms=%llu peak_pdf=%u peak_all=%u hits=%zu\n", GetTickCount64()-started, pdf_peak.load(), peak.load(), hits.size());
    Check(ok && hits.size()==38 && final.done && final.scanned_files==38 && final.total_files==38,
        "balanced workers retain every candidate exactly once with complete progress");
    Check(pdf_peak>=3 && peak<=6, "idle Office and text workers help PDF within the original total worker cap");
    cancel=false;
    const auto cancelled_at=GetTickCount64();
    const bool cancelled_ok=index::RunContentTaskSupplement(config, request, cancel, cache, {}, 0,
        [&](const auto& progress, auto) { final=progress; if (pdf_active>=3) cancel=true; return true; }, reader);
    Check(!cancelled_ok && final.error==ERROR_CANCELLED && active==0 && GetTickCount64()-cancelled_at<1000,
        "cancelling borrowed workers joins every active reader");
    return failed ? 1 : 0;
}

int RunTaskPriorityTest(const std::filesystem::path& base) {
    const auto root = base / L"priority-lanes";
    std::filesystem::create_directories(root);
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    index::ContentSearchRequest request; request.needle = L"priority_marker";
    for (unsigned i = 0; i < 24; ++i) {
        const auto path = root / (std::to_wstring(i) + L".pdf");
        Write(path, "fixture"); request.candidate_paths.push_back(path.wstring());
    }
    for (unsigned i = 0; i < 48; ++i) {
        const auto path = root / (std::to_wstring(i) + L".txt");
        Write(path, "fixture"); request.candidate_paths.push_back(path.wstring());
    }
    index::ContentTaskCache cache;
    cache.version = [](const auto&) { return std::wstring(L"stable"); };
    cache.excluded = [](const auto&) { return false; };
    cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
    std::atomic<bool> cancel{false};
    std::atomic<unsigned> text_active{0}, text_peak{0}, text_done{0};
    std::atomic<ULONGLONG> text_elapsed{0};
    const auto started = GetTickCount64();
    index::ContentTaskReader reader = [&](const std::wstring& path, uint64_t, std::wstring& body,
        uint64_t& bytes, DWORD*, text::Encoding, const auto&) {
        const bool text = std::filesystem::path(path).extension() == L".txt";
        if (text) {
            auto active = ++text_active, old = text_peak.load();
            while (old < active && !text_peak.compare_exchange_weak(old, active)) {}
        }
        Sleep(text ? 8 : 60);
        if (text) { --text_active; if (++text_done == 48) text_elapsed = GetTickCount64() - started; }
        body = L"priority_marker"; bytes = 15; return true;
    };
    std::set<std::wstring> hits;
    index::ContentSearchProgress final;
    const bool ok = index::RunContentTaskSupplement(config, request, cancel, cache, {}, 0,
        [&](const auto& progress, auto batch) { final = progress; for (const auto& hit : batch) hits.insert(hit.path); return true; }, reader);
    printf("PRIORITY text_ms=%llu total_ms=%llu text_peak=%u hits=%zu\n",
        text_elapsed.load(), GetTickCount64()-started, text_peak.load(), hits.size());
    Check(ok && hits.size() == 72 && final.done && final.scanned_files == 72 && final.total_files == 72,
        "priority scheduling retains every text and PDF candidate");
    Check(text_peak >= 3, "idle Office workers help text before queued PDFs");
    return failed ? 1 : 0;
}

int RunTaskLaneTest(const std::filesystem::path& base) {
    const auto root=base/L"lane-files";std::filesystem::create_directories(root);
    for(const auto* name:{L"a.pdf",L"b.pdf",L"c.rtf",L"d.txt"})Write(root/name,"fixture");
    index::ContentIndexConfig config;config.roots={{root.wstring(),text::Encoding::Auto}};
    index::ContentSearchRequest request;request.needle=L"lane_marker";
    index::ContentTaskCache cache;
    cache.version=[](const auto&){return std::wstring(L"stable-fixture-version");};
    cache.excluded=[](const auto&){return false;};
    cache.fresh=[](const auto&,uint64_t,uint64_t,const auto&){return false;};
    std::atomic<bool> cancel{false},release{false};std::atomic<unsigned> active_pdf{0};
    bool office=false,text=false,independent=false,known_before_done=false,serial=true;size_t hits=0;
    const auto thread=GetCurrentThreadId();index::ContentSearchProgress final,initial;initial.scanned_files=999;
    const auto started=GetTickCount64();
    index::ContentTaskReader reader=[&](const std::wstring& path,uint64_t,std::wstring& body,uint64_t& bytes,DWORD* error,text::Encoding,const auto& interrupted){
        if(std::filesystem::path(path).extension()==L".pdf"){
            ++active_pdf;const auto until=GetTickCount64()+3000;
            while(!release&&!interrupted()&&GetTickCount64()<until)Sleep(5);
            --active_pdf;if(interrupted()){*error=ERROR_CANCELLED;return false;}
        }else{const auto until=GetTickCount64()+500;while(!active_pdf&&!interrupted()&&GetTickCount64()<until)Sleep(1);}
        body=L"lane_marker";bytes=22;return true;
    };
    Check(index::RunContentTaskSupplement(config,request,cancel,cache,initial,0,[&](const auto& progress,auto batch){
        serial=serial&&GetCurrentThreadId()==thread;
        for(const auto& hit:batch){++hits;if(hit.name==L"c.rtf")office=true;if(hit.name==L"d.txt")text=true;}
        if(office&&text&&!release){independent=active_pdf.load()>0;release=true;}
        if(!progress.done&&progress.total_files==4)known_before_done=true;
        final=progress;return true;
    },reader),"category-isolated task scheduling completes");
    Check(independent&&office&&text&&GetTickCount64()-started<2500,"blocked PDF does not hold Office or text slots");
    Check(hits==4&&final.scanned_files==4&&final.total_files==4&&known_before_done&&serial,
        "enumeration publishes exact total and does not double-count cache progress");
    cancel=false;release=false;const auto cancelled_at=GetTickCount64();
    Check(!index::RunContentTaskSupplement(config,request,cancel,cache,{},0,[&](const auto& progress,auto){
        if(!progress.done&&active_pdf>0)cancel=true;final=progress;return true;
    },reader)&&final.error==ERROR_CANCELLED&&active_pdf==0&&GetTickCount64()-cancelled_at<2500,
        "cancellation joins blocked category workers and enumerator");
    printf("Task lane fixture: %ls\n",base.c_str());return failed?1:0;
}

int RunTaskMmapTest(const std::filesystem::path& base) {
    const auto root = base / L"mapped";
    std::filesystem::create_directories(root);
    const auto path = root / L"overflow.txt";
    const auto database = (base / L"mapped.sqlite").wstring();
    std::string body(512 * 1024, 'x'); body += " old__marker";
    Check(Write(path, body), "create small overflow-record fixture");
    index::ContentIndex owner(database);
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    Check(owner.Configure(config) && owner.WaitUntilIdle(10000), "cache overflow body and trailing file version");
    owner.Pause(true);
    WIN32_FILE_ATTRIBUTE_DATA original{};
    Check(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &original) != FALSE, "remember original file timestamp");
    Sleep(20);
    body.replace(body.size()-11, 11, "task_marker");
    Check(Write(path, body), "rewrite matching text without changing file size");
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE && SetFileTime(file, nullptr, nullptr, &original.ftLastWriteTime), "restore original last-write time");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    index::ContentIndex observer(database, true);
    const auto deadline = GetTickCount64() + 2000;
    while (observer.Configuration().roots.empty() && GetTickCount64() < deadline) Sleep(5);
    std::atomic<bool> cancel{false};
    for (const auto* needle : {L"old__marker", L"task_marker"}) {
        index::ContentSearchRequest request; request.needle = needle;
        size_t hits = 0; index::ContentSearchProgress final;
        const bool ok = observer.SearchTask(request, cancel, [&](const auto& progress, auto batch) {
            final = progress; hits += batch.size(); return true;
        });
        Check(ok && final.done && !final.error && hits == (request.needle == L"task_marker" ? 1u : 0u),
            "mapped snapshot validates trailing change identity despite unchanged size and timestamp");
    }
    Check(owner.Status().indexed_files == 1 && owner.Status().paused, "mapped task leaves persistent body cache paused and unchanged");
    return failed ? 1 : 0;
}

int RunTaskLiveTest() {
    index::ContentIndex observer({}, true);
    const auto ready_until = GetTickCount64() + 2000;
    while (observer.Configuration().roots.empty() && GetTickCount64() < ready_until) Sleep(10);
    const auto config = observer.Configuration();
    printf("LIVE_CONFIG roots=%zu shared=%d\n", config.roots.size(), config.shared_scope);
    index::ContentSearchRequest request; request.generation = GetTickCount64(); request.needle = L"3d3s";
    request.task_scan = true; request.maximum_hits = 0;
    std::atomic<bool> cancel{false};
    wchar_t seconds_text[16]{};
    GetEnvironmentVariableW(L"PULSE_TEST_TASK_LIVE_SECONDS", seconds_text, ARRAYSIZE(seconds_text));
    const uint64_t seconds = seconds_text[0] ? std::clamp(_wtoi(seconds_text), 15, 900) : 45;
    const uint64_t duration_ms = seconds * 1000;
    const auto start = GetTickCount64();
    std::jthread deadline([&](std::stop_token stop) {
        while (!stop.stop_requested() && GetTickCount64() - start < duration_ms) Sleep(25);
        if (!stop.stop_requested()) cancel = true;
    });
    size_t hits = 0; uint64_t first = UINT64_MAX, office = UINT64_MAX;
    std::set<std::wstring> identities;
    index::ContentSearchProgress final;
    observer.SearchTask(request, cancel, [&](const auto& progress, auto batch) {
        final = progress;
        for (const auto& hit : batch) {
            const auto elapsed = GetTickCount64() - start;
            if (first == UINT64_MAX) first = elapsed;
            const auto ext = index::ContentScopeKey(std::filesystem::path(hit.path).extension().wstring());
            if (office == UINT64_MAX && index::IsExtractedDocumentExtension(ext) && ext != L".pdf") office = elapsed;
            printf("LIVE_HIT ms=%llu ext=%ls\n", elapsed, ext.c_str());
            identities.insert(index::ContentScopeKey(hit.path));
            ++hits;
        }
        fflush(stdout);
        return true;
    });
    deadline.request_stop(); deadline.join();
    printf("LIVE_TASK first_ms=%llu office_ms=%llu elapsed_ms=%llu hits=%zu scanned=%llu total=%llu done=%d error=%lu\n",
        first, office, GetTickCount64()-start, hits, final.scanned_files, final.total_files, final.done, final.error);
    Check(final.done && GetTickCount64()-start < duration_ms+10000, "read-only full-scope probe completes or cancels within its bound");
    Check(identities.size() == hits, "read-only full-scope hits retain unique path identities");
    return failed ? 1 : 0;
}

int RunTaskManifestTest(const std::filesystem::path& base) {
    wchar_t manifest[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_TASK_MANIFEST", manifest, ARRAYSIZE(manifest));
    std::wstring lines; uint64_t bytes = 0; DWORD error = 0;
    Check(text::ReadFile(manifest, 1024 * 1024, lines, bytes, &error), "read original-file manifest without copying documents");
    index::ContentSearchRequest request; request.generation = 1; request.needle = L"3d3s";
    request.task_scan = true; request.paged_results = true; request.sort = index::ContentResultSort::Name;
    index::ContentIndexConfig config; config.excluded_directories.clear();
    size_t at = 0;
    while (at < lines.size()) {
        const auto end = lines.find(L'\n', at);
        auto path = lines.substr(at, end == std::wstring::npos ? end : end-at);
        if (!path.empty() && path.back() == L'\r') path.pop_back();
        if (!path.empty()) {
            request.candidate_paths.push_back(path);
            config.roots.push_back({std::filesystem::path(path).parent_path().wstring(), text::Encoding::Auto});
        }
        if (end == std::wstring::npos) break;
        at = end + 1;
    }
    Check(!request.candidate_paths.empty(), "manifest supplies explicit bounded original candidates");
    if (request.candidate_paths.empty()) return 1;
    index::ContentTaskCache cache;
    cache.native_version = true;
    std::atomic<uint64_t> version_calls{0};
    cache.version = [&](const std::wstring& path) {
        ++version_calls;
        const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return std::wstring{};
        BY_HANDLE_FILE_INFORMATION identity{}; FILE_BASIC_INFO info{};
        const bool ok = GetFileInformationByHandle(handle, &identity) &&
            GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info));
        CloseHandle(handle);
        return ok ? std::to_wstring(identity.dwVolumeSerialNumber) + L":" + std::to_wstring(identity.nFileIndexHigh) +
            L":" + std::to_wstring(identity.nFileIndexLow) + L":" + std::to_wstring(info.ChangeTime.QuadPart) : std::wstring{};
    };
    cache.excluded = [](const auto&) { return false; };
    cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
    std::atomic<bool> cancel{false}; index::ContentSearchSession session(request, nullptr, 0);
    std::set<std::wstring> found;
    const auto start = GetTickCount64(); uint64_t first = UINT64_MAX;
    index::ContentSearchProgress final;
    index::ContentSearchProgress initial; initial.generation = request.generation;
    const bool ok = index::RunContentTaskSupplement(config, request, cancel, cache, initial, 0,
        [&](const auto& progress, auto batch) {
            if (GetTickCount64()-start > 120000) cancel = true;
            for (const auto& hit : batch) {
                if (first == UINT64_MAX) first = GetTickCount64()-start;
                found.insert(index::ContentScopeKey(hit.path));
                printf("REAL_HIT ms=%llu ext=%ls\n", GetTickCount64()-start,
                    std::filesystem::path(hit.path).extension().c_str());
            }
            auto update = session.Accept({progress, std::move(batch), {}});
            if (update) final = update->progress;
            return true;
        });
    const auto elapsed = GetTickCount64()-start;
    auto store = session.Results();
    const auto settle = GetTickCount64()+5000;
    while ((store->Sorting() || store->Count()!=found.size()) && GetTickCount64()<settle) Sleep(5);
    std::set<std::wstring> displayed;
    for (size_t i=0; i<store->Count(); ++i) {
        index::ContentResultStore::Row row;
        while (!store->Get(i,row) && GetTickCount64()<settle) Sleep(5);
        if (!row.entry.full_path.empty()) displayed.insert(index::ContentScopeKey(row.entry.full_path));
    }
    printf("REAL_TASK candidates=%zu matched=%zu displayed=%zu first_ms=%llu complete_ms=%llu checked=%llu bytes=%llu error=%lu\n",
        request.candidate_paths.size(), found.size(), displayed.size(), first, elapsed, final.scanned_files, final.scanned_bytes, final.error);
    for (const auto& path : request.candidate_paths)
        if (!found.contains(index::ContentScopeKey(path))) std::wprintf(L"REAL_MISSING %ls\n", path.c_str());
    Check(ok && final.done && !final.error && found.size()==request.candidate_paths.size(), "all original manifest matches complete in the immediate task");
    uint64_t expected_versions = 0;
    for (const auto& path : request.candidate_paths)
        expected_versions += index::IsExtractedDocumentExtension(std::filesystem::path(path).extension().wstring()) ? 2 : 1;
    Check(version_calls == expected_versions, "native fast text reuses validated identity without a second version open");
    printf("REAL_VERSION_CALLS actual=%llu expected=%llu\n", version_calls.load(), expected_versions);
    Check(displayed==found && !store->Error(), "application session and visible-page store retain all actual task matches");
    store.reset();
    (void)base;
    return failed ? 1 : 0;
}

int RunTaskMetadataTest(const std::filesystem::path& base) {
    std::filesystem::create_directories(base);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{base};
    for (const auto* name : {L"a.txt", L"b.pdf", L"c.rtf", L"d.txt"}) Write(base / name, "fixture");
    index::ContentIndexConfig config; config.roots = {{base.wstring(), text::Encoding::Auto}};
    index::ContentSearchRequest request; request.needle = L"metadata_marker";
    std::atomic<bool> cancel{false}, release{false};
    index::ContentTaskCache cache;
    cache.version = [&](const auto&) {
        const auto deadline = GetTickCount64() + 1500;
        while (!release && !cancel && GetTickCount64() < deadline) Sleep(1);
        return std::wstring(L"stable-version");
    };
    cache.excluded = [](const auto&) { return false; };
    cache.fresh = [](const auto&, uint64_t, uint64_t, const auto&) { return false; };
    index::ContentTaskReader reader = [](const auto&, uint64_t, auto& body, auto& bytes, DWORD*,
                                         text::Encoding, const auto&) {
        body = L"metadata_marker"; bytes = 7; return true;
    };
    const auto started = GetTickCount64();
    uint64_t enumerated_ms = UINT64_MAX; size_t hits = 0;
    index::ContentSearchProgress final;
    const bool ok = index::RunContentTaskSupplement(config, request, cancel, cache, {}, 0,
        [&](const auto& progress, auto batch) {
            if (progress.total_files == 4 && enumerated_ms == UINT64_MAX) {
                enumerated_ms = GetTickCount64() - started; release = true;
            }
            hits += batch.size(); final = progress; return true;
        }, reader);
    printf("METADATA enumeration_ms=%llu completed_ms=%llu hits=%zu\n", enumerated_ms, GetTickCount64()-started, hits);
    Check(ok && hits == 4 && final.scanned_files == 4 && final.total_files == 4,
          "metadata scheduling keeps complete results and progress");
    Check(enumerated_ms < 1000, "slow file version reads do not block candidate enumeration");
    return failed ? 1 : 0;
}

int RunObserverTest(const std::filesystem::path& base) {
    const auto root=base/L"observer-files";
    std::filesystem::create_directories(root);
    const auto database=(base/L"observer.sqlite").wstring();
    Write(root/L"cached.txt","observer_marker");
    auto owner=std::make_unique<index::ContentIndex>(database);
    index::ContentIndexConfig config;config.roots={{root.wstring(),text::Encoding::Auto}};
    Check(owner->Configure(config)&&owner->WaitUntilIdle(10000),"observer fixture owner ready");
    index::ContentIndex observer(database,true);
    for(unsigned i=0;i<100&&observer.Configuration().roots.empty();++i) Sleep(20);
    owner->Pause(true);Sleep(150);
    const auto before=owner->Status();
    auto rejected=config;rejected.maximum_file_bytes=12345;
    Check(!observer.Configure(rejected),"observer rejects configuration writes");
    observer.Pause(false);observer.Rebuild();Sleep(150);
    const auto after=owner->Status();
    Check(after.paused&&after.indexed_files==before.indexed_files&&after.pending_files==before.pending_files&&
        owner->Configuration().maximum_file_bytes==config.maximum_file_bytes,"observer pause and rebuild do not alter live owner");
    owner->Pause(false);owner.reset();
    auto read=[](const std::wstring& path) {
        std::ifstream file(std::filesystem::path(path),std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
    };
    const auto configuration=read(database+L".config"),status=read(database+L".status"),cache=read(database);
    Check(!configuration.empty()&&!status.empty()&&!cache.empty(),"observer fixture durable bytes available");
    Write(root/L"new.rtf","{\\rtf1\\ansi observer_marker}");Write(root/L"new.txt","observer_marker");
    observer.Pause(true);observer.Rebuild();
    Sleep(350);
    index::ContentSearchRequest request;request.needle=L"observer_marker";
    Check(Search(observer,request).size()==1,"observer never takes writer ownership after owner exits");
    std::atomic<bool> cancel{false};size_t hits=0,done=0;
    Check(observer.SearchTask(request,cancel,[&](const auto& progress,auto batch){hits+=batch.size();done+=progress.done?1:0;return true;})&&hits==3&&done==1,
        "observer supplements uncached RTF and text without rebuilding");
    Check(Search(observer,request).size()==1&&read(database)==cache,"observer supplemental hits are not persisted in database");
    Check(read(database+L".config")==configuration&&read(database+L".status")==status,
        "observer leaves configuration and status sidecars byte-identical");
    printf("Observer fixture: %ls\n",base.c_str());return failed?1:0;
}

int RunTaskPrefetchTest(const std::filesystem::path& base) {
    struct SnapshotTrace {
        std::atomic<sqlite3*> positive{nullptr}, metadata{nullptr};
        std::atomic<bool> finished{false};
        std::atomic<size_t> rows{0};
    };
    static SnapshotTrace trace;
    auto extension = +[](sqlite3* connection, char**, const sqlite3_api_routines*) -> int {
        return sqlite3_trace_v2(connection, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE | SQLITE_TRACE_ROW,
            [](unsigned event, void* context, void* statement, void*) -> int {
                const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
                if (!sql) return 0;
                const std::string_view query(sql);
                if (event == SQLITE_TRACE_STMT && query == "SELECT size,modified,version FROM documents WHERE path=?1")
                    trace.positive = static_cast<sqlite3*>(context);
                if (query == "SELECT path,size,modified,version FROM documents") {
                    if (event == SQLITE_TRACE_STMT) trace.metadata = static_cast<sqlite3*>(context);
                    if (event == SQLITE_TRACE_ROW) ++trace.rows;
                    if (event == SQLITE_TRACE_PROFILE) trace.finished = true;
                }
                return 0;
            }, connection);
    };
    const auto root = base / L"prefetch-files", database = base / L"prefetch.sqlite";
    std::filesystem::create_directories(root);
    const std::string negative(8192, 'x');
    constexpr size_t negative_count = 256;
    bool seeded = true;
    for (size_t i = 0; i < negative_count; ++i)
        seeded &= Write(root / (L"negative-" + std::to_wstring(i) + L".txt"), negative);
    const auto positive = root / L"a-positive.txt", changed = root / L"z-changed.txt";
    seeded &= Write(positive, "prefetch_marker");
    seeded &= Write(changed, "oldcache_marker");
    index::ContentIndex db(database.wstring());
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    Check(seeded && db.Configure(config) && db.WaitUntilIdle(30000), "prefetch small cache fixture ready");
    db.Pause(true);
    const auto initializer = reinterpret_cast<void(*)()>(extension);
    Check(sqlite3_auto_extension(initializer) == SQLITE_OK, "install scoped snapshot observation");
    struct RemoveExtension {
        void(*initializer)();
        ~RemoveExtension() { sqlite3_cancel_auto_extension(initializer); }
    } remove_extension{initializer};
    index::ContentSearchRequest request; request.needle = L"prefetch_marker";
    std::atomic<bool> cancel{false};
    index::ContentSearchProgress final;
    size_t hits = 0;
    bool cached_first = false;
    Check(db.SearchTask(request, cancel, [&](const auto& progress, auto batch) {
        if (!batch.empty() && !hits) cached_first = progress.current_root.empty() && batch.front().path == positive;
        if (!progress.done && !progress.current_root.empty() && !trace.finished) {
            const auto deadline = GetTickCount64() + 3000;
            while (!trace.finished && GetTickCount64() < deadline) Sleep(1);
        }
        hits += batch.size(); final = progress; return true;
    }), "optional metadata prefetch completes");
    Check(cached_first && hits == 1 && final.done && final.scanned_files == negative_count + 2,
        "cached positive arrives first and all candidates are accounted for");
    Check(trace.finished && trace.rows == negative_count + 2 && trace.positive.load() == trace.metadata.load(),
        "optional version prefetch finishes on the cached query's exact connection");
    printf("PREFETCH read_bytes=%llu uncached_negative_bytes=%zu (read avoidance is scheduling dependent)\n",
        final.scanned_bytes, negative_count * negative.size());

    // Publish a newer cache row while SearchCached still owns its old WAL
    // snapshot. A new-connection prefetch would mistake this matching file for
    // a negative because only the old snapshot's hit query has run.
    bool published = false;
    auto publish_newer_cache = [&] {
        if (!Write(changed, "prefetch_marker")) return false;
        HANDLE file = CreateFileW(changed.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        BY_HANDLE_FILE_INFORMATION info{}; FILE_BASIC_INFO basic{};
        const bool measured = file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(file, &info) &&
            GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        const auto version = std::to_wstring(info.dwVolumeSerialNumber) + L":" + std::to_wstring(info.nFileIndexHigh) + L":" +
            std::to_wstring(info.nFileIndexLow) + L":" + std::to_wstring(basic.ChangeTime.QuadPart);
        sqlite3* writer = nullptr; sqlite3_stmt* statement = nullptr;
        bool ok = measured && sqlite3_open16(database.c_str(), &writer) == SQLITE_OK;
        auto tokenizer_api = [&](sqlite3* connection) {
            fts5_api* api = nullptr;
            if (connection && sqlite3_prepare_v2(connection, "SELECT fts5(?1)", -1, &statement, nullptr) == SQLITE_OK) {
                sqlite3_bind_pointer(statement, 1, &api, "fts5_api_ptr", nullptr); sqlite3_step(statement);
            }
            sqlite3_finalize(statement); statement = nullptr;
            return api;
        };
        auto* source_api = tokenizer_api(trace.positive.load());
        auto* writer_api = tokenizer_api(writer);
        fts5_tokenizer tokenizer{}; void* context = nullptr;
        ok = ok && source_api && writer_api && source_api->xFindTokenizer(source_api, "pulsegram", &context, &tokenizer) == SQLITE_OK &&
            writer_api->xCreateTokenizer(writer_api, "pulsegram", context, &tokenizer, nullptr) == SQLITE_OK;
        ok = ok && sqlite3_prepare_v2(writer,
            "UPDATE documents SET body='prefetch_marker',size=?1,modified=?2,version=?3 WHERE path=?4",
            -1, &statement, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(statement, 1, (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
            sqlite3_bind_int64(statement, 2, (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime);
            sqlite3_bind_text16(statement, 3, version.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text16(statement, 4, changed.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(writer) == 1;
        }
        if (!ok) printf("prefetch concurrent fixture write: %s\n", writer ? sqlite3_errmsg(writer) : "open failed");
        sqlite3_finalize(statement); if (writer) sqlite3_close(writer);
        return ok;
    };
    hits = 0;
    Check(db.SearchTask(request, cancel, [&](const auto& progress, auto batch) {
        if (!published && !batch.empty()) published = publish_newer_cache();
        hits += batch.size(); final = progress; return true;
    }) && published && hits == 2, "concurrent newer cache version cannot hide a newly matching file");

    // Temporarily exclude a cached positive using its availability attribute,
    // then restore its exact identity/timestamps during the cached callback.
    HANDLE file = CreateFileW(positive.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    FILE_BASIC_INFO original{};
    bool unavailable = file != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file, FileBasicInfo, &original, sizeof(original));
    auto offline = original; offline.FileAttributes |= FILE_ATTRIBUTE_OFFLINE;
    unavailable = unavailable && SetFileInformationByHandle(file, FileBasicInfo, &offline, sizeof(offline));
    Check(unavailable, "temporarily unavailable cached positive fixture prepared");
    size_t callbacks = 0; bool restored = false; bool recovered = false;
    db.SearchTask(request, cancel, [&](const auto&, auto batch) {
        if (++callbacks == 2 && unavailable)
            restored = SetFileInformationByHandle(file, FileBasicInfo, &original, sizeof(original)) != FALSE;
        for (const auto& hit : batch) recovered |= hit.path == positive;
        return true;
    });
    if (file != INVALID_HANDLE_VALUE) {
        SetFileInformationByHandle(file, FileBasicInfo, &original, sizeof(original)); CloseHandle(file);
    }
    Check(restored && recovered, "cached positive rejected by temporary availability is scanned after recovery");

    // Stop after entering the supplemental scan, when prefetch may be active.
    bool stopped = false;
    const auto start = GetTickCount64();
    const bool cancelled = db.SearchTask(request, cancel, [&](const auto& progress, auto) {
        if (!progress.current_root.empty() && !progress.done) { stopped = true; cancel = true; }
        final = progress; return true;
    });
    Check(stopped && !cancelled && final.error == ERROR_CANCELLED && GetTickCount64() - start < 3000,
        "cancel interrupts and joins optional prefetch");
    cancel = false;
    size_t rejected_callbacks = 0;
    Check(!db.SearchTask(request, cancel, [&](const auto&, auto) { return ++rejected_callbacks < 2; }) && rejected_callbacks == 2,
        "cached callback rejection is latched without later callbacks");
    bool released = true;
    for (int i = 0; i < 5; ++i) {
        request.maximum_hits = 1; hits = 0;
        released &= db.SearchTask(request, cancel, [&](const auto&, auto batch) { hits += batch.size(); return true; }) && hits == 1;
    }
    Check(released && GetTickCount64() - start < 5000, "cancelled and capped tasks release caller-owned reader slots");
    sqlite3* checkpoint = nullptr;
    int log_pages = 0, checkpointed = 0;
    const bool closed = sqlite3_open16(database.c_str(), &checkpoint) == SQLITE_OK &&
        sqlite3_wal_checkpoint_v2(checkpoint, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &log_pages, &checkpointed) == SQLITE_OK;
    if (checkpoint) sqlite3_close(checkpoint);
    Check(closed, "completed and cancelled tasks leave no pinned WAL snapshot");
    return failed ? 1 : 0;
}

int RunSearchTaskTest(const std::filesystem::path& base) {
    const auto root=base/L"task-files";
    std::filesystem::create_directories(root/L"hidden");
    // Admission locks belong to this fixture, not the user's parser slots.
    SetEnvironmentVariableW(L"PULSE_DOCUMENT_ADMISSION_DIR", base.c_str());
    Write(root/L"cached.txt","task_marker"); Write(root/L"stale.txt","old_marker"); Write(root/L"removed.txt","task_marker");
    index::ContentIndex db((base/L"task.sqlite").wstring());
    index::ContentIndexConfig config; config.roots={{root.wstring(),text::Encoding::Auto}};
    config.excluded_directories.push_back(L"hidden");
    Check(db.Configure(config)&&db.WaitUntilIdle(10000),"task initial cache ready");
    db.Pause(true);
    Write(root/L"stale.txt","task_marker"); Write(root/L"removed.txt","gone");
    Write(root/L"new.rtf","{\\rtf1\\ansi task_marker}"); Write(root/L"new.txt","task_marker");
    Write(root/L"hidden"/L"secret.txt","task_marker");
    struct FeedHost {
        PROCESS_INFORMATION process{};
        ~FeedHost() {
            if (!process.hProcess) return;
            wchar_t pipe[256]{}; GetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE",pipe,256);
            HANDLE handle=CreateFileW(pipe,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            if(handle!=INVALID_HANDLE_VALUE) {const auto header=index::MakeIndexHdr(index::REQ_IDX_TEST_SHUTDOWN,0,0);DWORD written=0;WriteFile(handle,&header,sizeof(header),&written,nullptr);CloseHandle(handle);}
            if(WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0) TerminateProcess(process.hProcess,ERROR_CANCELLED);
            CloseHandle(process.hThread);CloseHandle(process.hProcess);
        }
    } host;
    if(GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_TASK_SHARED",nullptr,0)) {
        wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
        const auto token=std::to_wstring(GetCurrentProcessId())+L"-task";
        SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE",(L"\\\\.\\pipe\\PulseIndex.Test."+token).c_str());
        const auto executable=std::filesystem::path(module).parent_path()/L"Pulse.Index.exe";
        auto command=L"\""+executable.wstring()+L"\" --test-host "+token+L" \""+root.wstring()+L"\" \""+(base/L"task-feed").wstring()+L"\"";
        STARTUPINFOW startup{};startup.cb=sizeof(startup);
        const bool started=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&host.process)!=FALSE;
        Check(started,"start isolated task filename feed");
        bool ready=false;
        for(unsigned i=0;started&&i<200&&!ready;++i) {index::IndexFeedConnection feed(nullptr);index::FileFeedPage page;ready=feed.Request(false,L"",0,0,page)&&page.ready;Sleep(25);}
        Check(ready,"isolated task filename feed ready");
        config.shared_scope=true;Check(db.Configure(config),"switch paused fixture to shared supplemental scope");
    }
    index::ContentSearchRequest request; request.needle=L"task_marker";
    std::atomic<bool> cancel{false}; std::vector<index::ContentHit> hits;
    size_t done=0; bool after_done=false; index::ContentSearchProgress final;
    const auto thread=GetCurrentThreadId(); bool serial=true;
    Check(db.SearchTask(request,cancel,[&](const auto& progress,auto batch) {
        serial=serial&&GetCurrentThreadId()==thread;
        if(done&&!batch.empty()) after_done=true;
        if(progress.done) {++done;final=progress;}
        hits.insert(hits.end(),std::make_move_iterator(batch.begin()),std::make_move_iterator(batch.end())); return true;
    }),"task supplement completes while background indexing paused");
    Check(hits.size()==4&&std::all_of(hits.begin(),hits.end(),[](const auto& hit){return hit.file_id==0;}),"fresh cache plus uncached and stale files produce four path identities");
    Check(!hits.empty()&&hits.front().name==L"cached.txt","cached hit emitted before supplemental parsing");
    Check(done==1&&!after_done&&serial&&final.total_files==final.scanned_files,"one final completion and serial truthful progress");
    Check(db.Status().paused&&db.Status().indexed_files==3,"supplement does not write results into paused cache");
    request.candidate_paths = {(root/L"new.txt").wstring(), (root/L"new.txt").wstring()};
    request.maximum_hits = 1; size_t selected_hits = 0; bool selected_only = true;
    db.SearchTask(request,cancel,[&](const auto&,auto batch) {
        for (const auto& hit:batch) { ++selected_hits; selected_only = selected_only && hit.name==L"new.txt"; }
        return true;
    });
    Check(selected_hits==1 && selected_only, "explicit candidates exclude unrelated cached hits and deduplicate before limit");
    request.candidate_paths.clear(); request.maximum_hits = 0;
    request.maximum_hits=2; size_t limited=0; final={};
    db.SearchTask(request,cancel,[&](const auto& progress,auto batch){limited+=batch.size();final=progress;return true;});
    Check(limited==2&&final.truncated,"task hit cap includes cache and supplement");
    request.maximum_hits=0; const auto start=GetTickCount64();
    const bool cancelled=db.SearchTask(request,cancel,[&](const auto& progress,auto){if(!progress.done)cancel=true;final=progress;return true;});
    Check(!cancelled&&final.error==ERROR_CANCELLED&&GetTickCount64()-start<2000,"task cancellation exits and releases resources");
    cancel=false;bool saw_document=false;
    const auto active_start=GetTickCount64();
    const bool active=db.SearchTask(request,cancel,[&](const auto& progress,auto batch){
        for(const auto& hit:batch) if(hit.name==L"new.rtf") {saw_document=true;cancel=true;}
        final=progress;return true;
    });
    Check(saw_document&&!active&&final.error==ERROR_CANCELLED&&GetTickCount64()-active_start<3000,
        "cancellation after parser result joins task workers and document session");
    cancel=false;
    std::filesystem::create_directories(root/L"overlap");
    Write(root/L"overlap"/L"one.txt","overlap_marker");
    Write(root/L"overlap"/L"two.txt","overlap_marker");
    config.shared_scope=false;config.roots.push_back({(root/L"overlap").wstring(),text::Encoding::Auto});
    Check(db.Configure(config),"configure overlapping paused local roots");
    request.needle=L"overlap_marker";request.maximum_hits=3;
    std::vector<std::wstring> overlap_hits;
    db.SearchTask(request,cancel,[&](const auto& progress,auto batch){for(const auto& hit:batch)overlap_hits.push_back(hit.path);final=progress;return true;});
    std::sort(overlap_hits.begin(),overlap_hits.end());
    Check(overlap_hits.size()==2&&std::adjacent_find(overlap_hits.begin(),overlap_hits.end())==overlap_hits.end()&&!final.truncated,
        "overlapping roots neither duplicate task hits nor consume hit limit twice");
    printf("Search task fixture: %ls\n",base.c_str()); return failed?1:0;
}

int RunExtractorUpgradeTest(const std::filesystem::path& base) {
    const auto root = base / L"upgrade-files";
    std::filesystem::create_directories(root);
    const auto database = (base / L"upgrade.sqlite").wstring();
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    config.maximum_file_bytes = 1234567; config.maximum_document_bytes = 2345678;
    config.excluded_directories.push_back(L"upgrade-excluded");
    {
        index::ContentIndex db(database);
        Check(db.Configure(config) && db.WaitUntilIdle(10000), "initialize empty upgrade database and durable settings");
    }
    const auto document = root / L"legacy.rtf", plain = root / L"unchanged.txt";
    Check(Write(document, "{\\rtf1\\ansi upgraded_document_marker}") &&
        Write(plain, "unchanged_text_marker"), "create unchanged on-disk document and text fixtures");
    sqlite3* raw = nullptr;
    bool seeded = sqlite3_open16(database.c_str(), &raw) == SQLITE_OK;
    // Only empty bodies are inserted: any tokenizer emits zero tokens. The
    // production connection registers its real pulsegram implementation later.
    fts5_api* api = nullptr; sqlite3_stmt* statement = nullptr;
    if (seeded && sqlite3_prepare_v2(raw, "SELECT fts5(?1)", -1, &statement, nullptr) == SQLITE_OK) {
        sqlite3_bind_pointer(statement, 1, &api, "fts5_api_ptr", nullptr); sqlite3_step(statement);
    }
    sqlite3_finalize(statement); statement = nullptr;
    fts5_tokenizer tokenizer{}; void* context = nullptr;
    seeded = seeded && api && api->xFindTokenizer(api, "unicode61", &context, &tokenizer) == SQLITE_OK &&
        api->xCreateTokenizer(api, "pulsegram", context, &tokenizer, nullptr) == SQLITE_OK;
    int id = 100;
    for (const auto& file : {document, plain}) {
        HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{}; FILE_BASIC_INFO basic{};
        const bool metadata = handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &info) &&
            GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic));
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        // Exact pre-extractor-version format, derived from unchanged file identity
        // and ChangeTime; an arbitrary stale version would not catch this regression.
        const auto version = std::to_wstring(info.dwVolumeSerialNumber) + L":" + std::to_wstring(info.nFileIndexHigh) + L":" +
            std::to_wstring(info.nFileIndexLow) + L":" + std::to_wstring(basic.ChangeTime.QuadPart);
        bool inserted = seeded && metadata && sqlite3_prepare_v2(raw,
            "INSERT INTO documents(id,path,root,size,modified,body,epoch,version) VALUES(?1,?2,?3,?4,?5,'',1,?6)",
            -1, &statement, nullptr) == SQLITE_OK;
        if (inserted) {
            sqlite3_bind_int(statement, 1, ++id);
            sqlite3_bind_text16(statement, 2, file.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text16(statement, 3, root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(statement, 4, (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
            sqlite3_bind_int64(statement, 5, (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime);
            sqlite3_bind_text16(statement, 6, version.c_str(), -1, SQLITE_TRANSIENT);
            inserted = sqlite3_step(statement) == SQLITE_DONE;
        }
        seeded = seeded && inserted; sqlite3_finalize(statement); statement = nullptr;
    }
    if (!seeded) printf("legacy cache seed error: %s\n", raw ? sqlite3_errmsg(raw) : "open failed");
    if (raw) sqlite3_close(raw);
    Check(seeded, "seed exact old-format empty document and text cache rows");
    if (!seeded) return 1;
    {
        index::ContentIndex db(database);
        Check(db.WaitUntilIdle(30000), "automatic startup reconciliation completes without Configure or Rebuild");
        index::ContentSearchRequest query; query.needle = L"upgraded_document_marker";
        Check(Search(db, query).size() == 1, "old empty document cache automatically re-extracted");
        query.needle = L"unchanged_text_marker";
        Check(Search(db, query).empty(), "unchanged ordinary text cache is not forcibly re-extracted");
        const auto actual = db.Configuration();
        Check(actual.roots.size() == 1 && actual.roots[0].path == config.roots[0].path &&
            actual.maximum_file_bytes == config.maximum_file_bytes && actual.maximum_document_bytes == config.maximum_document_bytes &&
            actual.excluded_directories == config.excluded_directories, "extractor upgrade preserves durable user configuration");
    }
    raw = nullptr;
    bool stable = sqlite3_open16(database.c_str(), &raw) == SQLITE_OK && sqlite3_prepare_v2(raw,
        "SELECT count(*) FROM documents WHERE (id=101 AND body<>'' AND version LIKE '%:extract2') OR (id=102 AND body='' AND version NOT LIKE '%:extract2')",
        -1, &statement, nullptr) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 2;
    sqlite3_finalize(statement); if (raw) sqlite3_close(raw);
    Check(stable, "document id preserved and extractor version changes only for document");
    printf("Extractor upgrade fixture: %ls\n", base.c_str());
    return failed ? 1 : 0;
}

int RunSharedFairnessTest(const std::filesystem::path& base) {
    const auto files = base / L"files", slow = files / L"slow", fast = files / L"fast";
    const auto cache = base / L"name-cache";
    for (const auto& path : {slow, fast, cache}) std::filesystem::create_directories(path);
    constexpr size_t slow_count = 192;
    const std::string body = "fairness_slow_marker " + std::string(256 * 1024, 'x');
    bool seeded = true;
    for (size_t i = 0; i < slow_count; ++i) seeded &= Write(slow / (std::to_wstring(i) + L".txt"), body);
    seeded &= Write(fast / L"sentinel.txt", "fairness_fast_marker");
    Check(seeded, "seed independent shared roots");
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto exe = std::filesystem::path(module).parent_path() / L"Pulse.Index.exe";
    const auto token = std::to_wstring(GetCurrentProcessId()) + L"-fairness";
    const auto pipe = L"\\\\.\\pipe\\PulseIndex.Test." + token;
    SetEnvironmentVariableW(L"PULSE_INDEX_FEED_PIPE", pipe.c_str());
    std::wstring command = L"\"" + exe.wstring() + L"\" --test-host " + token + L" \"" + files.wstring() + L"\" \"" + cache.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION host{};
    Check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &host) != FALSE,
          "start independent shared filename feed");
    if (!host.hProcess) return 1;
    CloseHandle(host.hThread);
    const auto ready_until = GetTickCount64() + 15000;
    bool pipe_ready = false;
    while (!(pipe_ready = WaitNamedPipeW(pipe.c_str(), 10) != FALSE) && GetTickCount64() < ready_until) Sleep(10);
    Check(pipe_ready, "isolated filename feed available");
    {
        index::ContentIndex db((base / L"fairness.sqlite").wstring());
        index::ContentIndexConfig config; config.roots = {{slow.wstring()}, {fast.wstring()}}; config.shared_scope = true;
        Check(db.Configure(config), "configure two shared roots");
        index::ContentSearchRequest query; query.needle = L"fairness_fast_marker";
        const auto start = GetTickCount64(); ULONGLONG first_fast = 0, first_count = 0;
        bool fast_before_slow_done = false, accurate_live_count = false, done = false;
        while (GetTickCount64() - start < 120000) {
            const auto status = db.Status();
            if (status.indexing && status.indexed_files > 0 && status.indexed_files < slow_count + 1) {
                accurate_live_count = true;
                if (!first_count) first_count = GetTickCount64() - start;
            }
            if (!first_fast && Search(db, query).size() == 1) {
                first_fast = GetTickCount64() - start;
                index::ContentSearchRequest slow_query; slow_query.needle = L"fairness_slow_marker";
                fast_before_slow_done = Search(db, slow_query).size() < slow_count;
            }
            if (first_fast && db.WaitUntilIdle(10)) { done = true; break; }
            Sleep(25);
        }
        printf("SHARED first_fast_ms=%llu first_nonzero_count_ms=%llu total_ms=%llu final_count=%llu early_fast=%d live_counts=%d\n",
            first_fast, first_count, GetTickCount64()-start, db.Status().indexed_files, fast_before_slow_done, accurate_live_count);
        Check(done && db.Status().indexed_files == slow_count + 1, "all shared files eventually indexed without loss");
        Check(fast_before_slow_done, "small second root progresses before first root finishes");
        Check(accurate_live_count, "committed counts visible while shared scan runs");
    }
    HANDLE control = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (control != INVALID_HANDLE_VALUE) {
        const auto header = index::MakeIndexHdr(index::REQ_IDX_TEST_SHUTDOWN, 0, 0); DWORD written = 0;
        WriteFile(control, &header, sizeof(header), &written, nullptr); CloseHandle(control);
    }
    const bool exited = WaitForSingleObject(host.hProcess, 5000) == WAIT_OBJECT_0;
    Check(exited, "isolated shared host shuts down");
    if (!exited) { TerminateProcess(host.hProcess, ERROR_CANCELLED); WaitForSingleObject(host.hProcess, 5000); }
    CloseHandle(host.hProcess);
    std::wprintf(L"Shared fairness fixture: %ls\n", base.c_str());
    return failed ? 1 : 0;
}

int RunZeroRevisionSubscriptionTest(const std::filesystem::path& base) {
    const auto root = base / L"empty-root", profile = base / L"profile", logs = base / L"logs";
    for (const auto& path : {root, profile, logs}) std::filesystem::create_directories(path);
    struct RestoreEnvironment {
        const wchar_t* name;
        std::wstring previous;
        explicit RestoreEnvironment(const wchar_t* key) : name(key), previous(32768, L'\0') {
            const DWORD count = GetEnvironmentVariableW(name, previous.data(), static_cast<DWORD>(previous.size()));
            previous.resize(count < previous.size() ? count : 0);
        }
        ~RestoreEnvironment() { SetEnvironmentVariableW(name, previous.empty() ? nullptr : previous.c_str()); }
    } restore_profile(L"LOCALAPPDATA"), restore_logs(L"PULSE_CONTENT_TIMING_DIR");
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    SetEnvironmentVariableW(L"PULSE_CONTENT_TIMING_DIR", logs.c_str());
    index::ContentSearchClient client;
    client.Start(nullptr, 0);
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Auto}};
    client.Configure(config);
    auto wait = [](auto ready, DWORD timeout = 10000) {
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline) {
            if (ready()) return true;
            Sleep(10);
        }
        return false;
    };
    const bool ready = wait([&] {
        const auto status = client.GetStatus();
        return client.ConfigurationReady() && status.root_status.size() == 1 &&
            status.root_status.front().available && !status.indexing && !status.pending_files;
    });
    Check(ready, "isolated empty index is ready for its first content subscription");
    if (!ready) { client.Stop(); return 1; }
    // Schema initialization inserts a sentinel at sequence 1 even for an empty
    // root. Remove it only in this isolated fixture to exercise a real zero cursor.
    const auto database = profile / L"Pulse" / L"ContentIndex" / L"content-v1.sqlite";
    sqlite3* raw = nullptr;
    bool seeded_zero = sqlite3_open16(database.c_str(), &raw) == SQLITE_OK;
    sqlite3_int64 before_sequence = -1;
    sqlite3_stmt* state = nullptr;
    if (seeded_zero) {
        sqlite3_busy_timeout(raw, 3000);
        seeded_zero = sqlite3_prepare_v2(raw,
            "SELECT coalesce(max(seq),0), (SELECT count(*) FROM documents), "
            "(SELECT count(*) FROM content_changes WHERE path<>'') FROM content_changes",
            -1, &state, nullptr) == SQLITE_OK && sqlite3_step(state) == SQLITE_ROW;
        if (seeded_zero) {
            before_sequence = sqlite3_column_int64(state, 0);
            seeded_zero = sqlite3_column_int64(state, 1) == 0 && sqlite3_column_int64(state, 2) == 0;
        }
        sqlite3_finalize(state);
        if (seeded_zero) seeded_zero = sqlite3_exec(raw,
            "BEGIN IMMEDIATE; DELETE FROM content_changes; DELETE FROM sqlite_sequence WHERE name='content_changes'; COMMIT;",
            nullptr, nullptr, nullptr) == SQLITE_OK;
        if (!seeded_zero) printf("ZERO_SEED error=%s\n", sqlite3_errmsg(raw));
    }
    if (raw) sqlite3_close(raw);
    printf("ZERO_SEED schema_sequence=%lld cleared=%d\n", before_sequence, seeded_zero);
    // A real configuration commit refreshes the agent's published cursor after
    // the fixture-only database edit, avoiding a stale in-memory sequence.
    --config.maximum_file_bytes;
    client.Configure(config);
    const bool published_zero = seeded_zero && wait([&] {
        const auto status = client.GetStatus();
        return status.change_sequence == 0 && !status.indexing && !status.pending_files &&
            client.GetConfig().maximum_file_bytes == config.maximum_file_bytes;
    });
    Check(published_zero, "isolated zero cursor is published before the first subscription");
    if (!published_zero) { client.Stop(); return 1; }
    index::ContentSearchRequest request;
    request.generation = 700001; request.session_id = 700000;
    request.root = root.wstring(); request.needle = L"zero_revision_marker";
    request.task_scan = true; request.subscribe = true; request.paged_results = true;
    client.SearchAsync(request);
    std::shared_ptr<index::ContentResultStore> original, current;
    bool initial_done = false, delta_done = false, stale = false;
    uint64_t initial_revision = UINT64_MAX;
    size_t initial_completions = 0;
    auto drain = [&] {
        index::ContentSearchUpdate update;
        while (client.TakeUpdate(update)) {
            stale |= update.progress.generation != request.generation;
            if (update.results) current = update.results;
            if (update.progress.done && !update.progress.error) {
                if (update.progress.delta) delta_done = true;
                else {
                    ++initial_completions; initial_done = true;
                    initial_revision = update.progress.index_revision;
                    original = current;
                }
            }
        }
    };
    const bool completed = wait([&] { drain(); return initial_done; });
    printf("ZERO_INITIAL completed=%d initial_revision=%llu original=%d count=%zu\n",
        completed, initial_revision, original != nullptr, original ? original->Count() : 0);
    Check(completed && initial_revision == 0 && original && original->Count() == 0,
        "initial empty task scan completes at revision zero");
    const auto file = root / L"first.txt";
    Check(Write(file, "zero_revision_marker"), "create the first matching file after revision-zero completion");
    const bool appeared = wait([&] { drain(); return delta_done && current && current->Count() == 1; });
    index::ContentResultStore::Row row;
    const bool row_ready = appeared && wait([&] { return current->Get(0, row); }, 3000);
    Check(appeared && row_ready && row.entry.full_path == file.wstring() && current == original &&
        !stale && initial_completions == 1 && client.CurrentGeneration() == request.generation,
        "first change after revision zero is an accepted delta in the same generation and result store");
    client.Stop();
    size_t task_starts = 0;
    for (const auto& entry : std::filesystem::directory_iterator(logs)) {
        if (entry.path().filename().wstring().starts_with(L"content-timing-") && entry.path().extension() == L".jsonl") {
            std::ifstream input(entry.path());
            for (std::string line; std::getline(input, line); )
                if (line.find("\"event\":\"start\"") != std::string::npos &&
                    line.find("\"generation\":700001,") != std::string::npos) ++task_starts;
        }
    }
    Check(task_starts == 1, "revision-zero subscription performs exactly one initial SearchTask scan");
    return failed ? 1 : 0;
}

int RunTaskTextIoBenchmark(const std::filesystem::path& base) {
    wchar_t input[32768]{};
    const auto length = GetEnvironmentVariableW(L"PULSE_TEST_TASK_TEXT_BENCH_ROOT", input, ARRAYSIZE(input));
    if (!length || length >= ARRAYSIZE(input)) return 2;
    const auto root = std::filesystem::path(input);
    index::ContentSearchRequest request; request.needle = L"3d3s"; request.generation = 881001;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.is_regular_file() && entry.path().extension() == L".txt") request.candidate_paths.push_back(entry.path().wstring());
    std::sort(request.candidate_paths.begin(), request.candidate_paths.end());
    if (request.candidate_paths.empty()) return 2;
    std::filesystem::create_directories(base);
    index::ContentIndex db((base / L"text-io.sqlite").wstring());
    db.Pause(true);
    index::ContentIndexConfig config; config.roots = {{root.wstring()}};
    if (!db.Configure(config)) return 3;
    std::atomic<bool> cancel{false};
    for (int run = 0; run < 3; ++run) {
        LARGE_INTEGER frequency{}, start{}, end{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        size_t hits = 0; uint64_t name_hash = 14695981039346656037ull; index::ContentSearchProgress final;
        std::set<std::wstring> names;
        const bool ok = db.SearchTask(request, cancel, [&](const auto& progress, auto batch) {
            final = progress;
            for (const auto& hit : batch) { ++hits; names.insert(hit.name); }
            return true;
        });
        QueryPerformanceCounter(&end);
        for (const auto& name : names) for (wchar_t ch : name) { name_hash ^= ch; name_hash *= 1099511628211ull; }
        printf("TEXT_IO run=%d elapsed_ms=%.3f scanned=%llu hits=%zu hash=%llu bytes=%llu error=%lu\n", run,
            static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart,
            final.scanned_files, hits, name_hash, final.scanned_bytes, final.error);
        if (!ok || !final.done || final.error || final.scanned_files != request.candidate_paths.size()) return 1;
    }
    return 0;
}

int RunDeltaCacheTest(const std::filesystem::path& base) {
    const auto root = base / L"delta-files";
    std::filesystem::create_directories(root);
    const auto selected = root / L"selected.txt";
    Write(selected, "3d3s cached selected");
    const std::string body = std::string(64 * 1024, 'x') + " 3d3s unrelated";
    for (int i = 0; i < 512; ++i) Write(root / (std::to_wstring(i) + L".txt"), body);
    index::ContentIndexConfig config; config.roots = {{root.wstring()}};
    index::ContentIndex db((base / L"delta.sqlite").wstring());
    Check(db.Configure(config) && db.WaitUntilIdle(30000), "delta fixture has 513 cached matching files");
    db.Pause(true);
    Write(selected, "3d3s changed selected content");
    index::ContentSearchRequest request; request.needle = L"3d3s"; request.generation = 880001;
    request.candidate_paths = {selected.wstring(), selected.wstring()};
    std::atomic<bool> cancelled{false};
    std::vector<double> milliseconds;
    for (int run = 0; run < 5; ++run) {
        LARGE_INTEGER frequency{}, start{}, finish{};
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        size_t hits = 0; bool selected_only = true, bounded_progress = true; index::ContentSearchProgress final;
        const bool ok = db.SearchTask(request, cancelled, [&](const auto& state, auto batch) {
            final = state;
            bounded_progress &= state.scanned_files <= 1;
            for (const auto& hit : batch) { ++hits; selected_only &= hit.name == L"selected.txt"; }
            return true;
        });
        QueryPerformanceCounter(&finish);
        const double elapsed = static_cast<double>(finish.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
        milliseconds.push_back(elapsed);
        printf("DELTA run=%d elapsed_ms=%.3f hits=%zu scanned=%llu error=%lu\n", run, elapsed, hits, final.scanned_files, final.error);
        Check(ok && final.done && !final.error && hits == 1 && selected_only && bounded_progress && final.scanned_files == 1,
            "single changed file ignores unrelated cache hits and duplicate candidates");
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    printf("DELTA median_ms=%.3f unrelated_cached_files=512\n", milliseconds[2]);
    Write(selected, "no longer matching");
    size_t hits = 0; index::ContentSearchProgress final;
    Check(db.SearchTask(request, cancelled, [&](const auto& state, auto batch) {
        final = state; hits += batch.size(); return true;
    }) && !final.error && hits == 0, "changed nonmatching file cannot reuse its stale cached hit");
    request.candidate_paths = {(root / L"uncached.txt").wstring()};
    Write(root / L"uncached.txt", "3d3s new file");
    hits = 0;
    Check(db.SearchTask(request, cancelled, [&](const auto&, auto batch) { hits += batch.size(); return true; }) && hits == 1,
        "uncached changed file remains searchable");
    cancelled = true; hits = 0;
    Check(!db.SearchTask(request, cancelled, [&](const auto&, auto batch) { hits += batch.size(); return true; }) && hits == 0,
        "cancelled delta never emits stale results");
    cancelled = false;
    size_t callbacks = 0;
    Check(!db.SearchTask(request, cancelled, [&](const auto&, auto) { ++callbacks; return false; }) && callbacks == 1,
        "delta callback cancellation is respected");
    Check(db.Status().paused && db.Status().indexed_files == 513, "delta queries do not update persistent body cache");
    return failed ? 1 : 0;
}

int RunEncodingCacheTest(const std::filesystem::path& base) {
    const auto root = base / L"encoding-files";
    std::filesystem::create_directories(root);
    Write(root / L"encoded.txt", std::string("\xb1\xa8\xb8\xe6", 4));
    const auto cache = (base / L"encoding.sqlite").wstring();
    index::ContentIndexConfig config; config.roots={{root.wstring(), text::Encoding::Gb18030}};
    config.default_encoding=text::Encoding::Gb18030;
    {
        index::ContentIndex legacy(cache);
        Check(legacy.Configure(config) && legacy.WaitUntilIdle(10000), "legacy encoding fixture builds");
        index::ContentSearchRequest request; request.needle=L"报告";
        Check(Search(legacy,request).size()==1, "legacy cache contains decoded GB18030 body");
    }
    index::ContentIndex instant(cache,index::ContentAgentMode::Instant);
    config.roots.front().encoding=text::Encoding::Utf8; config.default_encoding=text::Encoding::Utf8;
    Check(instant.Configure(config), "instant encoding changes without rewriting old body cache");
    index::ContentSearchRequest request; request.needle=L"报告"; request.root=root.wstring();
    std::atomic<bool> cancel{false}; size_t hits=0; bool done=false;
    instant.SearchTask(request,cancel,[&](const auto& progress,auto batch) { hits+=batch.size(); done|=progress.done; return true; });
    Check(done && hits==0, "unchanged file cannot reuse body decoded with previous encoding");
    return failed ? 1:0;
}

int RunSessionBatchAudit() {
    using namespace index;
    for (const auto sort : {ContentResultSort::Index, ContentResultSort::Name, ContentResultSort::Size,
                           ContentResultSort::Mtime, ContentResultSort::Path, ContentResultSort::Type}) {
        for (const bool descending : {false, true}) {
            ContentSearchRequest request; request.generation = 700; request.paged_results = request.task_scan = true;
            request.sort = sort; request.sort_desc = descending;
            ContentSearchSession session(request, nullptr, 0);
            ContentHit z{L"C:\\fixture\\z.zzz", L"z.zzz"}, a{L"C:\\fixture\\a.aaa", L"a.aaa"};
            z.size = z.modified = 9; a.size = a.modified = 1;
            ContentSearchUpdate first; first.progress.generation = 700; first.hits = {z};
            Check(session.Accept(std::move(first)).has_value() && session.Results()->RawCount() == 1,
                  "task session exposes first cached/uncached hit immediately");
            ContentSearchUpdate later; later.progress.generation = 700; later.progress.done = true; later.hits = {a};
            session.Accept(std::move(later));
            ContentResultStore::Row row;
            const bool expect_z = sort == ContentResultSort::Index ? !descending : descending;
            const auto deadline = GetTickCount64() + 2000;
            while (!session.Results()->Get(0, row) && GetTickCount64() < deadline) Sleep(1);
            Check(row.entry.name == (expect_z ? L"z.zzz" : L"a.aaa") && session.Results()->Count() == 2,
                  "task session restores requested global ordering regardless of arrival order");
        }
    }
    for (const size_t count : {1000u, 2000u, 4000u, 8000u}) {
        ContentSearchRequest request; request.generation = count; request.paged_results = request.task_scan = true;
        request.sort = ContentResultSort::Name;
        ContentSearchSession session(request, nullptr, 0);
        const auto started = GetTickCount64(); size_t publications = 0, rewritten_positions = 0; ULONGLONG first_ms = 0;
        uint64_t revision = 0;
        for (size_t i = 0; i < count; ++i) {
            ContentSearchUpdate update; update.progress.generation = count;
            ContentHit hit; hit.name = std::to_wstring(count - i) + L".txt"; hit.path = L"C:\\fixture\\" + hit.name; hit.file_id = i + 1;
            update.hits.push_back(std::move(hit));
            session.Accept(std::move(update));
            const auto store = session.Results();
            if (store->Revision() != revision) {
                revision = store->Revision(); ++publications; rewritten_positions += store->Count();
                if (publications == 1) first_ms = GetTickCount64() - started;
            }
        }
        ContentSearchUpdate done; done.progress.generation = count; done.progress.done = true; session.Accept(std::move(done));
        if (session.Results()->Revision() != revision) { ++publications; rewritten_positions += session.Results()->Count(); }
        printf("BATCH rows=%zu publications=%zu rebuilt_positions=%zu per_hit_baseline=%zu first_ms=%llu last_ms=%llu cached=%zu\n",
            count, publications, rewritten_positions, count * (count + 1) / 2, first_ms, GetTickCount64() - started, session.Results()->CachedRows());
        Check(session.Results()->Count() == count && publications < count / 4 && rewritten_positions < count * (count + 1) / 8,
              "single-hit production session avoids rebuilding the display order for each row");
        Check(session.Results()->CachedRows() <= ContentResultStore::kPageSize * ContentResultStore::kCachePages,
              "batched publication preserves bounded hot pages");
    }
    ContentSearchRequest request; request.generation = 900; request.task_scan = request.paged_results = true;
    ContentSearchSession session(request, nullptr, 0);
    for (int i = 0; i < 11; ++i) {
        ContentSearchUpdate update; update.progress.generation = 900;
        ContentHit hit; hit.path = L"C:\\fixture\\" + std::to_wstring(i) + L".txt"; hit.name = std::to_wstring(i);
        update.hits.push_back(std::move(hit)); session.Accept(std::move(update));
    }
    const auto cancel_start = GetTickCount64(); const auto cancelled = session.Fail(ERROR_CANCELLED);
    Check(cancelled && cancelled->progress.error == ERROR_CANCELLED && session.Results()->Count() == 11 &&
          GetTickCount64() - cancel_start < 1000, "cancellation drains pending partial results without waiting for batch timer");
    return failed ? 1 : 0;
}

int RunTaskScopeAudit(const std::filesystem::path& base) {
    const auto root = base / L"scope", wanted = root / L"wanted", deep = wanted / L"deep";
    std::filesystem::create_directories(deep);
    Write(wanted / L"yes.txt", "scope_marker"); Write(deep / L"nested.txt", "scope_marker");
    index::ContentIndexConfig config; config.roots = {{root.wstring()}, {(base / L"missing-outside").wstring()}};
    index::ContentSearchRequest request; request.root = wanted.wstring(); request.needle = L"scope_marker";
    request.recursive = false; request.generation = 123;
    index::ContentTaskCache cache;
    cache.version = [](const auto&) { return std::wstring(L"fixture"); };
    cache.excluded = [](const auto&) { return false; };
    cache.fresh = [](const auto&, auto, auto, const auto&) { return false; };
    std::atomic<bool> cancelled = false;
    auto run = [&](size_t expected) {
        size_t hits = 0; index::ContentSearchProgress final;
        const bool ok = index::RunContentTaskSupplement(config, request, cancelled, cache, {}, 0,
            [&](const auto& state, auto batch) { final = state; hits += batch.size(); return true; });
        Check(ok && !final.error && final.done && hits == expected,
              "actual task enumeration remains complete within requested coverage");
    };
    // Deny directory listing only on this owned temporary child. Nonrecursive
    // enumeration must never open it; restore its DACL before fixture cleanup.
    PSECURITY_DESCRIPTOR denied = nullptr, allowed = nullptr;
    const bool descriptors = ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(D;;0x1;;;WD)(A;;FA;;;OW)", SDDL_REVISION_1, &denied, nullptr) &&
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;FA;;;WD)", SDDL_REVISION_1, &allowed, nullptr);
    const bool secured = descriptors && SetFileSecurityW(deep.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, denied);
    Check(secured, "isolated deep directory denies list access");
    if (secured) {
        WIN32_FIND_DATAW find{}; HANDLE h = FindFirstFileW((deep.wstring() + L"\\*").c_str(), &find);
        Check(h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED, "fixture verifies real directory access denial");
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }
    Check(index::ContentTaskRoots(config, request) == std::vector<std::wstring>{index::ContentScopeKey(wanted.wstring())},
          "effective roots drop unrelated configured roots before directory opens");
    run(1);
    if (secured) Check(SetFileSecurityW(deep.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, allowed) != FALSE,
                      "restore isolated directory permissions");
    if (denied) LocalFree(denied); if (allowed) LocalFree(allowed);
    request.recursive = true; run(2);
    request.root = base.wstring(); config.roots = {{wanted.wstring()}, {deep.wstring()}};
    Check(index::ContentTaskRoots(config, request).size() == 1, "ancestor query collapses overlapping configured coverage"); run(2);
    request.root = (base / L"other").wstring(); run(0);
    cancelled = true;
    Check(!index::RunContentTaskSupplement(config, request, cancelled, cache, {}, 0, [](const auto&, auto) { return true; }),
          "scope-restricted task respects cancellation");
    return failed ? 1 : 0;
}

int RunInstantLifecycleTests(const std::filesystem::path& base);
int RunInstantScopeResumeTest(const std::filesystem::path& base);
int RunInstantGapTests(const std::filesystem::path& base);
int RunInstantReconcileAudit(const std::filesystem::path& base);
int wmain() {
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr, module, ARRAYSIZE(module));
    const auto base = std::filesystem::path(module).parent_path().parent_path() / L"bench_data" /
        (L"content-index-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    struct CleanupFixture {
        std::filesystem::path path;
        ~CleanupFixture() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup_fixture{base};
    if (GetEnvironmentVariableW(L"PULSE_TEST_SESSION_BATCH_AUDIT", nullptr, 0)) return RunSessionBatchAudit();
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_SCOPE_AUDIT", nullptr, 0)) return RunTaskScopeAudit(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_ENCODING_CACHE", nullptr, 0)) return RunEncodingCacheTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_TEXT_BENCH_ROOT", nullptr, 0)) return RunTaskTextIoBenchmark(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_DELTA_CACHE", nullptr, 0)) return RunDeltaCacheTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_RECONCILE_AUDIT", nullptr, 0)) return RunInstantReconcileAudit(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_GAPS", nullptr, 0)) return RunInstantGapTests(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_SCOPE_RESUME", nullptr, 0)) return RunInstantScopeResumeTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_INSTANT_MODE", nullptr, 0)) return RunInstantLifecycleTests(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_ZERO_REVISION", nullptr, 0)) return RunZeroRevisionSubscriptionTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_LIVE", nullptr, 0)) return RunTaskLiveTest();
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_MMAP", nullptr, 0)) return RunTaskMmapTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_PREFETCH", nullptr, 0)) return RunTaskPrefetchTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_PRIORITY", nullptr, 0)) return RunTaskPriorityTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_BALANCE", nullptr, 0)) return RunTaskBalanceTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_ROOT_FAIRNESS", nullptr, 0)) return RunTaskRootFairnessTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_DIRECTORY_FAIRNESS", nullptr, 0)) return RunTaskRootFairnessTest(base, true);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_NONPDF_FIFO", nullptr, 0)) {
        const auto office = RunTaskRootFairnessTest(base / L"office", false, L".rtf");
        const auto plain = RunTaskRootFairnessTest(base / L"text", false, L".txt");
        return office || plain;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_MANIFEST", nullptr, 0)) return RunTaskManifestTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_METADATA", nullptr, 0)) return RunTaskMetadataTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_HINTS", nullptr, 0)) return RunTaskHintTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_TASK_LANES", nullptr, 0)) return RunTaskLaneTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_OBSERVER", nullptr, 0)) return RunObserverTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_TASK", nullptr, 0)) return RunSearchTaskTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_EXTRACTOR_UPGRADE", nullptr, 0)) return RunExtractorUpgradeTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SHARED_FAIRNESS", nullptr, 0)) return RunSharedFairnessTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_QUERY", nullptr, 0)) {
        wchar_t fixture[32768]{};
        const auto n = GetEnvironmentVariableW(L"PULSE_TEST_QUERY_FIXTURE", fixture, ARRAYSIZE(fixture));
        return RunQueryTaskTest(n && n < ARRAYSIZE(fixture) ? std::filesystem::path(fixture) : base);
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_LATENCY", nullptr, 0)) return RunLatencyTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_AVAILABILITY", nullptr, 0)) return RunAvailabilityTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_PARTIAL_ROOT", nullptr, 0)) return RunPartialRootTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_SHARED_SCOPE", nullptr, 0)) return RunSharedScopeTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_RECOVERY", nullptr, 0)) return RunRecoveryTest(base);
    if (GetEnvironmentVariableW(L"PULSE_TEST_CONTENT_INTERNAL", nullptr, 0)) return RunInternalPathTest(base);
    const auto root = base / L"files";
    std::filesystem::create_directories(root / L"node_modules");
    std::filesystem::create_directories(root / L"many");
    Check(Write(root / L"one.txt", "Alpha beta\nreport reportable\n\xe6\x8a\xa5\xe5\x91\x8a\xe4\xb8\xad\xe6\x96\x87 ++ punctuation\n"), "create UTF-8 fixture");
    Write(root / L"two.md", "gamma Alpha\n");
    Write(root / L"node_modules" / L"hidden.txt", "hidden_excluded_word");
    Write(root / L"binary.txt", std::string("binary\0marker", 13));
    Write(root / L"legacy.txt", std::string("\xb1\xa8\xb8\xe6", 4));
    Write(root / L"utf16.txt", std::string("\xff\xfe\xa5\x62\x4a\x54", 6));
    for (int i = 0; i < 10017; ++i) Write(root / L"many" / (L"f" + std::to_wstring(i) + L".txt"), i == 10016 ? "common beyond_ten_thousand" : "common noise");
    const auto database = (base / L"fixture.sqlite").wstring();
    index::ContentIndexConfig config; config.roots = {{root.wstring(), text::Encoding::Gb18030}};
    // UTF-8 fixtures use automatic detection; explicit legacy decoding is tested separately below.
    config.roots[0].encoding = text::Encoding::Auto;
    {
        index::ContentIndex db(database);
        Check(db.Configure(config), "configure local fixture root");
        Check(db.WaitUntilIdle(120000), "initial enumeration completes");
        const auto status = db.Status();
        printf("indexed=%llu skipped=%llu errors=%llu code=%lu\n", status.indexed_files, status.skipped_files, status.errors, status.error);
        Check(status.errors == 0 && status.indexed_files >= 10020, "coverage counts files and skips binary/excluded directories");
        index::ContentSearchRequest r; r.needle = L"beyond_ten_thousand";
        Check(Search(db, r).size() == 1, "no 10000 filename candidate cap");
        r.needle = L"common"; r.maximum_hits = 13; index::ContentSearchProgress final;
        Check(Search(db, r, &final).size() == 13 && final.truncated, "only final hits are capped with truncation");
        r.maximum_hits = 10000; r.needle = L"Alpha beta";
        Check(Search(db, r).size() == 1, "all words literal verification");
        r.match_mode = index::ContentMatchMode::AnyWord; r.needle = L"beta gamma";
        Check(Search(db, r).size() == 2, "any-word union candidates");
        r.match_mode = index::ContentMatchMode::Phrase; r.needle = L"Alpha beta";
        Check(Search(db, r).size() == 1, "phrase verification");
        r.needle = L"alpha beta"; r.case_sensitive = true;
        Check(Search(db, r).empty(), "case-sensitive post verification");
        r.case_sensitive = false; r.needle = L"报告";
        Check(Search(db, r).size() >= 2, "CJK bigram and UTF-16 BOM");
        r.needle = L"报";
        Check(Search(db, r).size() >= 2, "CJK unigram");
        r.needle = L"++";
        Check(Search(db, r).size() == 1, "punctuation is indexed");
        r.needle = L"reporta"; r.whole_word = true;
        Check(Search(db, r).empty(), "whole-word boundary verification");
        r.whole_word = false; r.needle = L"Alpha"; r.excluded_needles = {L"gamma"};
        Check(Search(db, r).size() == 1, "exclusions verified against cached body");
        r.excluded_needles.clear(); r.filename_query = L"ext:md";
        Check(Search(db, r).size() == 1, "filename extension filter after FTS candidates");
        r.filename_query.clear(); r.needle = L"hidden_excluded_word";
        Check(Search(db, r).empty(), "default directory exclusions");
        r.needle = L"Alpha"; r.root = (root / L"many").wstring();
        Check(Search(db, r).empty(), "scope root boundaries"); r.root.clear();
        HANDLE locked = CreateFileW((root / L"one.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        r.needle = L"punctuation";
        Check(locked != INVALID_HANDLE_VALUE && Search(db, r).size() == 1, "queries use cached body with original file locked");
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
        r.needle = L"watched_creation"; Write(root / L"created.txt", "watched_creation");
        Check(AwaitHit(db, r, 1), "watch creation reconciles");
        Write(root / L"created.txt", "watched_modified_changed"); r.needle = L"watched_modified_changed";
        Check(AwaitHit(db, r, 1), "watch modification replaces body atomically");
        DeleteFileW((root / L"created.txt").c_str());
        Check(AwaitHit(db, r, 0), "watch deletion removes cached result");
        db.Pause(true); Check(db.Status().paused, "pause status");
        std::atomic<bool> cancel{true}; bool cancelled = false;
        db.Search(r, cancel, [&](const auto& progress, auto) { cancelled = progress.done && progress.error == ERROR_CANCELLED; return true; });
        Check(cancelled, "generation cancellation interrupts readonly query");
        db.Pause(false);
    }
    DeleteFileW((root / L"two.md").c_str());
    {
        index::ContentIndex reopened(database);
        Check(reopened.WaitUntilIdle(120000), "restart reconciles persistent database");
        Check(reopened.Configuration().roots.size() == 1, "configuration persists");
        index::ContentSearchRequest r; r.needle = L"gamma";
        Check(Search(reopened, r).empty(), "restart prunes offline deletions across epochs");
        reopened.Pause(true);
        const auto moved = base / L"temporarily-unavailable";
        std::filesystem::rename(root, moved);
        r.needle = L"common";
        Check(Search(reopened, r).empty(), "inaccessible root does not expose live results");
        std::filesystem::rename(moved, root);
        reopened.Pause(false);
    }
    {
        wchar_t previous[32768]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", previous, ARRAYSIZE(previous));
        const auto profile = base / L"isolated-profile";
        const auto ipc_root = base / L"ipc-files";
        std::filesystem::create_directories(profile); std::filesystem::create_directories(ipc_root);
        Write(ipc_root / L"cached.txt", "persistent_ipc_word");
        SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
        index::ContentSearchClient client; client.Start(nullptr, 0);
        index::ContentIndexConfig ipc_config; ipc_config.roots = {{ipc_root.wstring(), text::Encoding::Auto}};
        client.Configure(ipc_config);
        bool ready = false;
        for (int i = 0; i < 200; ++i) { const auto status = client.GetStatus(); if (status.indexed_files == 1 && !status.indexing) { ready = true; break; } Sleep(50); }
        Check(ready, "persistent agent config/status protocol");
        index::ContentSearchRequest r; r.generation = 100; r.needle = L"persistent_ipc_word";
        client.SearchAsync(r);
        size_t hits = 0; bool done = false; DWORD error = 0;
        for (int i = 0; i < 200 && !done; ++i) {
            index::ContentSearchUpdate update;
            while (client.TakeUpdate(update)) { hits += update.hits.size(); done = update.progress.done; error = update.progress.error; }
            if (!done) Sleep(25);
        }
        Check(done && !error && hits == 1, "persistent agent cached search protocol");
        const ULONGLONG start = GetTickCount64();
        for (int i = 0; i < 20; ++i) { r.generation = 200 + i; client.SearchAsync(r); client.Cancel(); }
        Check(GetTickCount64() - start < 250, "rapid search/cancel never joins the UI thread");
        r.generation = 300; client.SearchAsync(r); done = false; hits = 0; bool stale = false;
        for (int i = 0; i < 200 && !done; ++i) {
            index::ContentSearchUpdate update;
            while (client.TakeUpdate(update)) { stale = stale || update.progress.generation != 300; hits += update.hits.size(); done = update.progress.done; }
            if (!done) Sleep(25);
        }
        Check(done && hits == 1 && !stale, "only latest generation is delivered");
        client.Pause(true);
        for (int i = 0; i < 100 && !client.GetStatus().paused; ++i) Sleep(25);
        Check(client.GetStatus().paused, "persistent pause protocol");
        client.Pause(false); client.Rebuild();
        for (int i = 0; i < 100 && client.GetStatus().paused; ++i) Sleep(25);
        Check(!client.GetStatus().paused, "persistent resume/rebuild protocol");
        index::ContentSearchClient second_client; second_client.Start(nullptr, 0);
        for (int i = 0; i < 200 && second_client.GetStatus().indexed_files != 1; ++i) Sleep(25);
        Check(second_client.GetStatus().indexed_files == 1, "second agent process reads shared coverage");
        client.Stop();
        r.generation = 400; second_client.SearchAsync(r); done = false; hits = 0;
        for (int i = 0; i < 200 && !done; ++i) {
            index::ContentSearchUpdate update;
            while (second_client.TakeUpdate(update)) { hits += update.hits.size(); done = update.progress.done; }
            if (!done) Sleep(25);
        }
        Check(done && hits == 1, "second agent survives original agent shutdown");
        second_client.Stop();
        Check(index::LoadContentIndexConfig().roots.size() == 1, "agent persists isolated per-user configuration");
        SetEnvironmentVariableW(L"LOCALAPPDATA", n ? previous : nullptr);
    }    {
        const auto small = base / L"shared-files";
        const auto alternate = base / L"alternate-files";
        std::filesystem::create_directories(small); std::filesystem::create_directories(alternate);
        Write(small / L"A.txt", "sort_marker A");
        Write(small / L"b.txt", "sort_marker bbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        Write(small / L"c.txt", "sort_marker ccccccccc");
        Write(alternate / L"replacement.txt", "alternate_marker");
        const auto shared_path = (base / L"shared.sqlite").wstring();
        index::ContentIndexConfig shared_config; shared_config.roots = {{small.wstring(), text::Encoding::Auto}};
        auto owner = std::make_unique<index::ContentIndex>(shared_path);
        Check(owner->Configure(shared_config) && owner->WaitUntilIdle(10000), "shared writer initial cache");
        owner->Pause(true);
        Check(owner->Configure(shared_config) && owner->WaitUntilIdle(100), "same configuration does not rebuild cached bodies");
        owner->Pause(false);
        auto follower = std::make_unique<index::ContentIndex>(shared_path);
        Check(follower->WaitUntilIdle(10000), "second instance attaches as readonly follower");
        index::ContentSearchRequest r; r.needle = L"sort_marker"; r.sort = index::ContentResultSort::Name;
        auto sorted = Search(*follower, r);
        Check(sorted.size() == 3 && sorted.front().name == L"A.txt" && sorted.back().name == L"c.txt", "content name sort uses ordinal case folding");
        r.sort_desc = true; sorted = Search(*follower, r);
        Check(sorted.size() == 3 && sorted.front().name == L"c.txt", "descending content sort");
        r.sort = index::ContentResultSort::Size; sorted = Search(*follower, r);
        Check(sorted.size() == 3 && sorted.front().name == L"b.txt", "content size sort before final hit cap");
        r.maximum_hits = 1; sorted = Search(*follower, r);
        Check(sorted.size() == 1 && sorted.front().name == L"b.txt", "sort applies before final cap");
        r.maximum_hits = 10000; r.sort = index::ContentResultSort::Index; r.sort_desc = false;
        follower->Pause(true);
        for (int i = 0; i < 100 && !owner->Status().paused; ++i) Sleep(25);
        Check(owner->Status().paused, "second instance pause reaches writer");
        follower->Pause(false);
        shared_config.roots = {{alternate.wstring(), text::Encoding::Auto}};
        Check(follower->Configure(shared_config), "second instance saves shared configuration");
        r.needle = L"alternate_marker";
        Check(AwaitHit(*owner, r, 1), "writer applies other instance configuration");
        owner.reset();
        Check(follower->WaitUntilIdle(10000), "readonly instance takes writer ownership after exit");
        Write(alternate / L"takeover.txt", "takeover_marker"); r.needle = L"takeover_marker";
        Check(AwaitHit(*follower, r, 1), "promoted writer watches new changes");
        follower.reset();
        Check(Write(shared_path, "malformed cache fixture"), "corrupt only isolated rebuildable database");
        {
            index::ContentIndex recovered(shared_path);
            Check(recovered.WaitUntilIdle(10000), "malformed cache rebuilds from independent configuration");
            Check(recovered.Configuration().roots.size() == 1 && recovered.Configuration().roots.front().path == alternate.wstring(), "cache corruption preserves configured roots");
            Check(Search(recovered, r).size() == 1, "recovered cache contains configured content");
        }
        index::ContentSearchRequest transient; transient.indexed = false; transient.root = small.wstring(); transient.needle = L"sort_marker"; transient.filename_query = L"A.txt";
        std::atomic<bool> cancel{false}; size_t count = 0;
        index::RunContentSearch(transient, cancel, [&](const auto&, auto hits) { count += hits.size(); return true; });
        Check(count == 1, "explicit transient scan honors filename query");
    }    std::wstring decoded;
    Check(!text::Decode({0xb1, 0xa8, 0xb8, 0xe6}, decoded, text::Encoding::Utf8), "strict UTF-8 rejects invalid bytes");
    Check(text::Decode({0xb1, 0xa8, 0xb8, 0xe6}, decoded, text::Encoding::Gb18030) && decoded == L"报告", "explicit GB18030 decoding");
    Check(text::Decode({0xef, 0xbb, 0xbf}, decoded) && decoded.empty(), "empty UTF-8 BOM");
    printf("%d passed, %d failed\n", passed, failed);
    // This PID/tick-specific directory was created by this executable only.
    if (!failed) std::filesystem::remove_all(base);
    else wprintf(L"Fixture retained: %s\n", base.c_str());
    return failed ? 1 : 0;
}
