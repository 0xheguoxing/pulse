// Isolated algorithm/heap regression: no production pipe, journal, or preferences.
// Include the implementation to compare its private helpers with the old rollup.
#include "../index/change_tracking.cpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <tuple>

namespace {
std::atomic<size_t> live_bytes{0}, peak_bytes{0}, allocations{0};
struct alignas(std::max_align_t) Allocation { size_t bytes; };
void* Allocate(size_t bytes) {
    if (bytes > SIZE_MAX - sizeof(Allocation)) throw std::bad_alloc();
    auto* h = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + bytes));
    if (!h) throw std::bad_alloc();
    h->bytes = bytes;
    const size_t live = live_bytes.fetch_add(bytes) + bytes;
    auto peak = peak_bytes.load();
    while (peak < live && !peak_bytes.compare_exchange_weak(peak, live)) {}
    ++allocations;
    return h + 1;
}
void Release(void* p) noexcept {
    if (!p) return;
    auto* h = static_cast<Allocation*>(p) - 1;
    live_bytes.fetch_sub(h->bytes);
    std::free(h);
}
}
void* operator new(size_t n) { return Allocate(n); }
void* operator new[](size_t n) { return Allocate(n); }
void operator delete(void* p) noexcept { Release(p); }
void operator delete[](void* p) noexcept { Release(p); }
void operator delete(void* p, size_t) noexcept { Release(p); }
void operator delete[](void* p, size_t) noexcept { Release(p); }

namespace pulse::index {
struct ChangeTrackerTestAccess {
    static void Set(ChangeTracker& tracker, const std::vector<ChangeRecord>& records, uint64_t now) {
        auto& j = tracker.journals_[L"memory-test"];
        j.loaded = true; j.active = true; j.expiry = now + 3600;
        j.records = records; j.dirty = true; ++j.revision;
    }
    static const auto& JournalOf(const ChangeTracker& tracker) { return tracker.journals_.at(L"memory-test"); }
    // Events arriving between flushes: appended in place, like Record().
    static void AppendRecords(ChangeTracker& tracker, const std::vector<ChangeRecord>& more) {
        auto& j = tracker.journals_[L"memory-test"];
        j.records.insert(j.records.end(), more.begin(), more.end()); ++j.revision;
    }
    static void EraseFront(ChangeTracker& tracker, size_t count) {
        auto& j = tracker.journals_[L"memory-test"];
        j.records.erase(j.records.begin(), j.records.begin() + (std::min)(count, j.records.size())); ++j.revision;
    }
    // Runs only the incremental step; true when it applied (no rebuild).
    static bool Extend(ChangeTracker& tracker, uint64_t since, uint64_t now) {
        auto& j = tracker.journals_[L"memory-test"];
        if (!ChangeTracker::ExtendSummaries(j, since, now)) return false;
        j.cache_revision = j.revision; return true;
    }
    static void Rebuild(ChangeTracker& tracker, uint64_t since, uint64_t now) {
        ChangeTracker::RebuildSummaries(tracker.journals_[L"memory-test"], since, now);
    }
    static bool Incremental(const ChangeTracker& tracker) { return JournalOf(tracker).cache_incremental; }
    static const auto& SummaryMap(const ChangeTracker& tracker) { return JournalOf(tracker).summaries; }
    static bool SaveOnly(ChangeTracker& tracker) { return tracker.Save(L"memory-test", JournalOf(tracker)); }
    // Steady state after a compaction: a minute of new events on a full journal.
    static void Append(ChangeTracker& tracker, uint32_t count, uint64_t now) {
        auto& j = tracker.journals_[L"memory-test"];
        for (uint32_t i = 0; i < count; ++i) {
            ChangeRecord e; e.id = 200001 + i; e.file_id = e.id; e.time = now - 10;
            e.path = L"C:\\perf\\company\\department\\year\\month\\workspace\\appended\\item" + std::to_wstring(i) + L".txt";
            j.records.push_back(std::move(e));
        }
        j.unsaved += count; j.dirty = true; ++j.revision;
    }
};
}
using namespace pulse::index;
namespace {
using Summaries = std::unordered_map<std::wstring, ChangeSummary>;
// Pre-optimization reference: keep separate alias logic so the fast path is checked.
Summaries Reference(const std::vector<ChangeRecord>& records, uint64_t since, uint64_t now) {
    Aliases aliases;
    for (const auto& e : records) if (e.source == ChangeSource::Event && e.time >= since && e.time + kRetention >= now) {
        auto key = Identity(e);
        const auto existing = aliases.find(Normalize(e.path));
        const auto old = aliases.find(Normalize(e.old_path));
        if (!e.file_id && existing != aliases.end()) key = existing->second;
        else if (!e.file_id && old != aliases.end()) key = old->second;
        aliases[Normalize(e.path)] = key;
        if (!e.old_path.empty()) aliases[Normalize(e.old_path)] = key;
    }
    struct Rollup { uint64_t time = 0; ChangeKind kind = ChangeKind::Modified; };
    std::unordered_map<std::wstring, std::unordered_map<uint32_t, Rollup>> rollups;
    std::unordered_map<std::wstring, uint32_t> identities;
    std::unordered_set<std::wstring> initial;
    Summaries summaries;
    for (const auto& e : records) {
        if (e.time < since || e.time + kRetention < now) continue;
        if (e.source == ChangeSource::InitialMtime && aliases.contains(Normalize(e.path))) continue;
        if (e.source == ChangeSource::InitialMtime) {
            if (!initial.insert(Normalize(e.path)).second) continue;
            VisitAncestors(e, [&](const auto& path) {
                auto& s = summaries[path]; ++s.count; ++s.initial_count;
                s.last_change = (std::max)(s.last_change, e.time);
            });
            continue;
        }
        const auto [it, added] = identities.try_emplace(Key(e, aliases), static_cast<uint32_t>(identities.size()));
        (void)added;
        VisitAncestors(e, [&](const auto& path) {
            const auto kind = e.kind == ChangeKind::Renamed ? Relative(e, path).kind : e.kind;
            auto& item = rollups[path][it->second];
            item.time = (std::max)(item.time, e.time);
            if (Priority(kind) >= Priority(item.kind) || (item.kind == ChangeKind::Deleted && kind == ChangeKind::Created)) item.kind = kind;
        });
    }
    for (const auto& [path, items] : rollups) {
        auto& s = summaries[path];
        for (const auto& [id, item] : items) {
            (void)id; ++s.count; s.last_change = (std::max)(s.last_change, item.time);
            ++s.counts[static_cast<uint32_t>(item.kind)]; s.has_deleted |= item.kind == ChangeKind::Deleted;
        }
    }
    return summaries;
}
bool Equal(const ChangeSummary& a, const ChangeSummary& b) {
    return a.count == b.count && a.last_change == b.last_change && a.initial_count == b.initial_count &&
        a.has_deleted == b.has_deleted && std::equal(std::begin(a.counts), std::end(a.counts), std::begin(b.counts));
}
struct Metric {
    size_t baseline = live_bytes.load(), count = allocations.load();
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    Metric() { peak_bytes = baseline; }
    void Print(const char* phase) const {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        const size_t peak = peak_bytes.load(), live = live_bytes.load();
        std::printf("[METRIC] %s ms=%.3f peak_extra_bytes=%zu retained_delta_bytes=%lld allocations=%zu\n", phase, ms,
            peak >= baseline ? peak - baseline : 0, static_cast<long long>(live) - static_cast<long long>(baseline), allocations.load() - count);
    }
};
std::vector<ChangeRecord> Fixture(uint64_t now, bool fallback) {
    std::vector<ChangeRecord> records; records.reserve(100000);
    for (uint64_t i = 0; i < 100000; ++i) {
        ChangeRecord e; e.id = i + 1; e.file_id = fallback ? 0 : i + 1; e.time = now - 20;
        e.path = L"C:\\perf\\company\\department\\year\\month\\workspace\\folder" + std::to_wstring(i % 128) + L"\\item" + std::to_wstring(i) + L".txt";
        records.push_back(std::move(e));
    }
    return records;
}
}
int main(int argc, char** argv) {
    const uint64_t now = ChangeTracker::Now();
    const std::string mode = argc > 1 ? argv[1] : "--check";
    int failures = 0;
    auto check = [&](bool ok, const char* label) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; };
    if (mode == "--summary-append") {
        // A full history, then new events between flushes (badges poll every 2 s).
        ChangeTracker tracker;
        { auto records = Fixture(now, false); ChangeTrackerTestAccess::Set(tracker, records, now); }
        std::vector<std::wstring> paths;
        for (uint32_t i = 0; i < 128; ++i) paths.push_back(L"C:\\perf\\company\\department\\year\\month\\workspace\\folder" + std::to_wstring(i));
        ChangeResponse result;
        { Metric m; result = tracker.Summaries(L"memory-test", paths, now - 86400); m.Print("summary-cold"); }
        uint64_t id = 300000;
        auto batch = [&](uint32_t count, bool repeat) {
            std::vector<ChangeRecord> more;
            for (uint32_t i = 0; i < count; ++i) {
                ChangeRecord e; e.id = ++id; e.time = now - 5; e.kind = ChangeKind::Modified;
                // repeat: files already in the history change again; otherwise new files.
                e.file_id = repeat ? 1 + (i * 37) % 100000 : id;
                e.path = repeat ? L"C:\\perf\\company\\department\\year\\month\\workspace\\folder" + std::to_wstring((e.file_id - 1) % 128) +
                                      L"\\item" + std::to_wstring(e.file_id - 1) + L".txt"
                                : L"C:\\perf\\company\\department\\year\\month\\workspace\\folder" + std::to_wstring(i % 128) + L"\\new" + std::to_wstring(id) + L".txt";
                more.push_back(std::move(e));
            }
            return more;
        };
        for (const auto& [count, repeat, label] : {std::tuple{1u, false, "append-1-new"}, std::tuple{1u, true, "append-1-repeat"},
                                                   std::tuple{3000u, false, "append-3000-new"}, std::tuple{3000u, true, "append-3000-repeat"}}) {
            ChangeTrackerTestAccess::AppendRecords(tracker, batch(count, repeat));
            Metric m; result = tracker.Summaries(L"memory-test", paths, now - 86400); m.Print(label);
        }
        uint32_t total = 0; for (const auto& row : result.summaries) total += row.count;
        check(total == 100000 + 1 + 3000, "appended identities are counted once each");
        check(ChangeTrackerTestAccess::Incremental(tracker), "file-ID history stays incremental");
        return failures ? 1 : 0;
    }
    if (mode == "--summary-age") {
        // The 7-day window slides while the journal is not full: 1000 of the
        // 100000 events expire before the next poll.
        auto records = Fixture(now, false);
        for (uint32_t i = 0; i < 1000; ++i) records[i * 100].time = now - kRetention + 30;
        ChangeTracker tracker; ChangeTrackerTestAccess::Set(tracker, records, now);
        { Metric m; (void)tracker.Summaries(L"memory-test", {L"C:\\perf"}, 0); m.Print("summary-cold"); }
        bool extended = false;
        { Metric m; extended = ChangeTrackerTestAccess::Extend(tracker, 0, now + 60); m.Print("age-1000-expired"); }
        check(extended, "expiry is applied incrementally");
        { Metric m; extended = ChangeTrackerTestAccess::Extend(tracker, 0, now + 61); m.Print("age-nothing-due"); }
        check(extended, "a poll with nothing due stays incremental");
        const auto& map = ChangeTrackerTestAccess::SummaryMap(tracker);
        const auto key = Normalize(L"C:\\perf");
        const auto it = map.find(key);
        const auto expected = Reference(records, 0, now + 61);
        const auto want = expected.find(key);
        check(it != map.end() && it->second.count == 99000, "expired identities leave the rollup");
        check(it != map.end() && want != expected.end() && Equal(it->second, want->second), "aged rollup matches the legacy rollup");
        ChangeTracker fresh; ChangeTrackerTestAccess::Set(fresh, records, now);
        { Metric m; ChangeTrackerTestAccess::Rebuild(fresh, 0, now + 60); m.Print("rebuild-for-comparison"); }
        return failures ? 1 : 0;
    }
    if (mode == "--summary" || mode == "--summary-no-id" || mode == "--save" || mode == "--flush" || mode == "--flush-append") {
        ChangeTracker tracker;
        { auto records = Fixture(now, mode == "--summary-no-id"); ChangeTrackerTestAccess::Set(tracker, records, now); }
        if (mode == "--save" || mode == "--flush" || mode == "--flush-append") {
            const auto root = std::filesystem::temp_directory_path() / (L"pulse-memory-bench-" + std::to_wstring(GetCurrentProcessId()));
            std::filesystem::create_directory(root); tracker.Open(root.wstring());
            if (mode == "--flush-append") { tracker.Flush(); ChangeTrackerTestAccess::Append(tracker, 3000, now); }
            { Metric m; if (mode == "--flush") { tracker.Flush(); m.Print("flush"); }
              else if (mode == "--flush-append") { tracker.Flush(); m.Print("flush-append"); }
              else { const bool ok = ChangeTrackerTestAccess::SaveOnly(tracker); m.Print("save"); check(ok, "save succeeds"); } }
            check(std::filesystem::exists(root / L"changes-memory-test.bin"), "isolated journal exists");
            if (mode == "--flush-append") check(std::filesystem::exists(root / L"changes-memory-test.bin.log"), "steady flush appends to the log");
            std::filesystem::remove_all(root);
        } else {
            std::vector<std::wstring> paths;
            for (uint32_t i = 0; i < 128; ++i) paths.push_back(L"C:\\perf\\company\\department\\year\\month\\workspace\\folder" + std::to_wstring(i));
            ChangeResponse result;
            { Metric m; result = tracker.Summaries(L"memory-test", paths, now - 86400); m.Print("summary-cold"); }
            uint32_t count = 0; for (const auto& row : result.summaries) count += row.count;
            check(count == 100000, "100000 file identities are preserved");
            { Metric m; result = tracker.Summaries(L"memory-test", paths, now - 86400 + 1); m.Print("summary-warm"); }
        }
        return failures ? 1 : 0;
    }
    std::mt19937 random(7241);
    for (uint32_t trial = 0; trial < 40; ++trial) {
        std::vector<ChangeRecord> records;
        for (uint32_t i = 0; i < 1200; ++i) {
            ChangeRecord e; e.id = i + 1; e.file_id = random() % 40;
            e.time = now - random() % (8 * 86400);
            e.kind = static_cast<ChangeKind>(random() % 6); e.is_dir = random() % 4 == 0;
            e.source = random() % 9 == 0 ? ChangeSource::InitialMtime : ChangeSource::Event;
            e.path = (random() % 2 ? L"C:\\root\\" : L"\\\\server\\share\\") + std::wstring(L"folder") + std::to_wstring(random() % 8) + L"\\item" + std::to_wstring(random() % 60);
            if (random() % 3 == 0) e.old_path = L"C:\\root\\other\\item" + std::to_wstring(random() % 60);
            if (trial % 4 == 0) { e.file_id = i + 1; e.source = ChangeSource::Event; }
            records.push_back(std::move(e));
        }
        ChangeTracker tracker; ChangeTrackerTestAccess::Set(tracker, records, now);
        for (uint64_t since : {uint64_t(0), now - 3 * 86400, now - 86400, uint64_t(0)}) {
            const auto expected = Reference(records, since, now);
            std::vector<std::wstring> paths{L"C:\\absent", L"C:\\", L"\\\\server\\share"};
            for (const auto& [path, value] : expected) { (void)value; paths.push_back(path); }
            const auto actual = tracker.Summaries(L"memory-test", paths, since);
            bool equal = actual.summaries.size() == paths.size();
            for (const auto& item : actual.summaries) {
                const auto it = expected.find(Normalize(item.path));
                equal &= Equal(item, it == expected.end() ? ChangeSummary{} : it->second);
            }
            if (!equal) { check(false, "randomized legacy rollup equivalence"); return 1; }
        }
    }
    check(true, "40 seeded histories / 160 rolling windows match legacy rollups");
    {
        const std::vector<std::wstring> variants{
            L"C:\\Root\\Folder\\Item", L"c:/root/folder/renamed", L"\\\\?\\C:\\ROOT\\Other\\Item",
            L"C:\\Root\\Folder-neighbor\\Item", L"\\\\?\\UNC\\Server\\Share\\Folder\\Item",
            L"\\\\server\\share\\other\\ITEM", L"C:\\", L"", L"relative", L"C:\\Root\\Folder\\"};
        bool equal = true;
        for (const auto& current : variants) for (const auto& previous : variants) for (bool directory : {false, true}) {
            ChangeRecord event; event.path = current; event.old_path = previous;
            event.kind = ChangeKind::Renamed; event.is_dir = directory;
            std::vector<std::pair<std::wstring, ChangeKind>> expected, actual;
            VisitAncestors(event, [&](const auto& path) { expected.emplace_back(path, Relative(event, path).kind); });
            VisitSummaryAncestors(event, [&](const auto& path, ChangeKind kind) { actual.emplace_back(path, kind); });
            equal &= actual == expected;
        }
        check(equal, "normalized summary traversal preserves rename kinds for 200 path/directory combinations");
    }
    {
        // A single identity visiting many directories must not turn the compact
        // accumulator into a quadratic scan, or duplicate common ancestors.
        std::vector<ChangeRecord> moves;
        for (uint64_t i = 0; i < 4096; ++i) {
            ChangeRecord e; e.id = i + 1; e.file_id = 77; e.time = now - 20;
            e.kind = ChangeKind::Renamed;
            e.path = L"C:\\moves\\folder" + std::to_wstring(i) + L"\\file.txt";
            if (!moves.empty()) e.old_path = moves.back().path;
            moves.push_back(std::move(e));
        }
        const auto expected = Reference(moves, 0, now);
        std::vector<std::wstring> paths;
        for (const auto& [path, value] : expected) { (void)value; paths.push_back(path); }
        ChangeTracker tracker; ChangeTrackerTestAccess::Set(tracker, moves, now);
        const auto actual = tracker.Summaries(L"memory-test", paths);
        bool equal = actual.summaries.size() == paths.size();
        for (const auto& row : actual.summaries) equal &= Equal(row, expected.at(Normalize(row.path)));
        check(equal, "4096 cross-directory moves preserve per-identity ancestor rollups");
        ChangeRecord one; one.id = 1; one.file_id = 99; one.time = now - 20; one.path = L"C:\\small\\file.txt";
        ChangeTrackerTestAccess::Set(tracker, {one}, now);
        const auto small = tracker.Summaries(L"memory-test", {L"C:\\small", L"C:\\moves"});
        check(small.summaries[0].count == 1 && small.summaries[1].count == 0,
            "large-history cache rebuild does not leak previous ancestor counts");
    }
    {
        // Appends between flushes extend the cached rollups instead of a
        // rebuild; every step must equal the legacy rollup of the whole history.
        std::mt19937 rng(9137);
        uint32_t steps = 0, mismatches = 0, refused = 0, aging = 0;
        auto make = [&](uint64_t id, uint64_t clock) {
            ChangeRecord e; e.id = id; e.file_id = 1 + rng() % 40;
            e.time = clock - 60 - rng() % (6 * 86400);
            e.kind = static_cast<ChangeKind>(rng() % 6); e.is_dir = rng() % 5 == 0;
            e.path = (rng() % 3 ? L"C:\\root\\" : (rng() % 2 ? L"D:\\data\\" : L"\\\\server\\share\\")) + std::wstring(L"folder") +
                std::to_wstring(rng() % 8) + L"\\sub" + std::to_wstring(rng() % 3) + L"\\item" + std::to_wstring(rng() % 60);
            if (rng() % 3 == 0) e.old_path = L"C:\\root\\other" + std::to_wstring(rng() % 4) + L"\\item" + std::to_wstring(rng() % 60);
            return e;
        };
        // The whole cached table, keys included: folders that lost their last
        // identity must disappear as they do in a rebuild.
        auto same = [](const Summaries& actual, const Summaries& expected) {
            if (actual.size() != expected.size()) return false;
            for (const auto& [path, value] : actual) {
                const auto it = expected.find(path);
                if (it == expected.end() || !Equal(value, it->second)) return false;
            }
            return true;
        };
        for (uint32_t trial = 0; trial < 40; ++trial) {
            uint64_t id = 0, clock = now + 10;
            std::vector<ChangeRecord> history;
            for (uint32_t i = 0; i < 300; ++i) history.push_back(make(++id, now));
            const uint64_t since = trial % 2 ? now - 3 * 86400 : 0;
            ChangeTracker tracker; ChangeTrackerTestAccess::Set(tracker, history, now);
            (void)tracker.Summaries(L"memory-test", {L"C:\\"}, since);
            for (uint32_t round = 0; round < 30; ++round) {
                // The clock moves up to 12 hours per round: about 7.5 days per
                // trial, so most of the first history ages out on the way.
                const uint64_t before = clock;
                clock += rng() % (12 * 3600);
                std::vector<ChangeRecord> more;
                const uint32_t count = rng() % (round % 5 == 0 ? 60 : 6);
                for (uint32_t i = 0; i < count; ++i) more.push_back(make(++id, clock));
                history.insert(history.end(), more.begin(), more.end());
                ChangeTrackerTestAccess::AppendRecords(tracker, more);
                aging += std::any_of(history.begin(), history.end(), [&](const ChangeRecord& e) {
                    return e.time + kRetention >= before && e.time + kRetention < clock && e.time >= since;
                });
                if (!ChangeTrackerTestAccess::Extend(tracker, since, clock)) { ++refused; break; }
                mismatches += !same(ChangeTrackerTestAccess::SummaryMap(tracker), Reference(history, since, clock));
                ++steps;
            }
        }
        std::printf("incremental steps=%u aging_steps=%u refused=%u mismatches=%u\n", steps, aging, refused, mismatches);
        check(refused == 0 && mismatches == 0 && steps == 1200 && aging > 300,
            "1200 incremental appends and 7-day expiries match legacy rollups exactly");
        // Not exact incrementally: an ID-less event, a pruned front, a moved window.
        ChangeTracker tracker;
        std::vector<ChangeRecord> history;
        for (uint64_t i = 1; i <= 50; ++i) history.push_back(make(i, now));
        ChangeTrackerTestAccess::Set(tracker, history, now);
        (void)tracker.Summaries(L"memory-test", {L"C:\\"}, 0);
        check(ChangeTrackerTestAccess::Incremental(tracker), "file-ID history builds the incremental cache");
        ChangeRecord bare = make(51, now); bare.file_id = 0;
        ChangeTrackerTestAccess::AppendRecords(tracker, {bare}); history.push_back(bare);
        check(!ChangeTrackerTestAccess::Extend(tracker, 0, now + 10), "an ID-less event is not applied incrementally");
        auto matches = [&](const std::vector<ChangeRecord>& all, uint64_t since) {
            const auto expected = Reference(all, since, now);
            std::vector<std::wstring> query;
            for (const auto& [path, value] : expected) { (void)value; query.push_back(path); }
            const auto actual = tracker.Summaries(L"memory-test", query, since);
            bool equal = actual.summaries.size() == query.size();
            for (const auto& row : actual.summaries) equal &= Equal(row, expected.at(Normalize(row.path)));
            return equal;
        };
        check(matches(history, 0) && !ChangeTrackerTestAccess::Incremental(tracker), "ID-less history falls back to the full rebuild");
        history.pop_back(); ChangeTrackerTestAccess::Set(tracker, history, now);
        (void)tracker.Summaries(L"memory-test", {L"C:\\"}, 0);
        ChangeTrackerTestAccess::EraseFront(tracker, 5); history.erase(history.begin(), history.begin() + 5);
        check(!ChangeTrackerTestAccess::Extend(tracker, 0, now + 10), "a pruned journal is not extended");
        check(matches(history, 0), "a pruned journal rebuilds to the same rollups");
        check(matches(history, now - 86400) && matches(history, 0), "moving the window rebuilds both ways");
    }
    {
        class FailedBuffer : public std::streambuf {
            std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
        } buffer;
        std::ostream stream(&buffer);
        ChangeJournalWriter writer(stream); writer.PutU32(123);
        check(!writer.Flush(), "buffered write failure is reported");
        writer.PutString(std::wstring(40000, L'x'));
        check(!writer.Flush(), "failed stream remains failed on later writes");
    }
    // Check persistence bytes, including a record larger than the writer buffer.
    const auto root = std::filesystem::temp_directory_path() / (L"pulse-memory-check-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(root);
    {
        ChangeTracker tracker; tracker.Open(root.wstring());
        ChangeRecord a; a.id = 1; a.time = now; a.path = L"C:\\unicode\\\u4e2d\u6587.txt";
        ChangeRecord b = a; b.id = 2; b.path = L"C:\\" + std::wstring(32760, L'x'); b.old_path = L"C:\\" + std::wstring(32760, L'y'); b.kind = ChangeKind::Renamed;
        ChangeTrackerTestAccess::Set(tracker, {a, b}, now);
        check(ChangeTrackerTestAccess::SaveOnly(tracker), "stream persistence accepts long UTF-16 records");
        const auto& j = ChangeTrackerTestAccess::JournalOf(tracker);
        pulse::ipc::PayloadWriter w; w.PutU32(0x33484350); w.PutU32(static_cast<uint32_t>(j.records.size()));
        w.PutU64(j.tracking_since); w.PutU64(j.paused_at); w.PutU64(j.gap_end); w.PutU64(j.gap_until); w.PutU32(j.active ? 1u : 0u);
        for (const auto& e : j.records) {
            w.PutU64(e.id); w.PutU64(e.time); w.PutU64(e.file_id); w.PutU32(static_cast<uint32_t>(e.kind));
            w.PutU32(e.is_dir ? 1u : 0u); w.PutU32(static_cast<uint32_t>(e.source)); w.PutString(e.path); w.PutString(e.old_path);
        }
        std::ifstream in(root / L"changes-memory-test.bin", std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(bytes == w.data(), "journal bytes exactly match existing v3 writer");
        ChangeTracker loaded; loaded.Open(root.wstring());
        const auto decoded = loaded.Details(L"memory-test", L"C:\\", 0, 0, 200);
        check(decoded.records.size() == 2, "streamed journal reloads without losing records");
    }
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
