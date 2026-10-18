#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace pulse::index {
class IndexMemoryProbe;
std::wstring NormalizeChangePath(std::wstring path);
bool IsChangeJournalName(std::wstring_view name);
enum class ChangeKind : uint32_t { Created, Modified, Deleted, Renamed, MovedIn, MovedOut };
enum class ChangeState : uint32_t { Available, Gap, Unavailable, Scanning, Offline, NotCovered };
enum class ChangeSource : uint32_t { Event, InitialMtime };
struct ChangeRecord {
    uint64_t id = 0, time = 0;
    uint64_t file_id = 0;
    ChangeKind kind = ChangeKind::Modified;
    bool is_dir = false;
    ChangeSource source = ChangeSource::Event;
    std::wstring path, old_path;
};
struct ChangeSummary {
    std::wstring path;
    uint64_t last_change = 0;
    uint32_t count = 0;
    uint32_t counts[6]{};
    uint32_t initial_count = 0;
    bool has_deleted = false, incomplete = true;
    ChangeState state = ChangeState::Unavailable;
};
// Incremental summaries: the newest record of one file identity (volume
// prefix + file ID, the same identity Key() builds as text).
struct ChangeIdentitySlot {
    uint64_t file_id = 0;
    uint32_t prefix = 0;
    uint32_t last = UINT32_MAX;  // UINT32_MAX: empty slot
};
struct ChangeResponse {
    ChangeState state = ChangeState::Unavailable;
    std::vector<ChangeSummary> summaries;
    std::vector<ChangeRecord> records;
    uint64_t next_cursor = 0;
};
class ChangeTracker {
public:
    static uint64_t Now();
    void Open(const std::wstring& directory);
    bool Lease(const std::wstring& owner, bool enabled);
    void Record(ChangeRecord record);
    void Seed(const std::wstring& owner, std::vector<ChangeRecord> records);
    void Gap();
    void Flush(bool force = true, IndexMemoryProbe* memory = nullptr);
    ChangeResponse Summaries(const std::wstring& owner, const std::vector<std::wstring>& paths, uint64_t since = 0);
    ChangeResponse Details(const std::wstring& owner, const std::wstring& path,
                           uint64_t since, uint64_t before, uint32_t limit, uint32_t kind_filter = UINT32_MAX);
private:
    friend struct ChangeTrackerTestAccess;
    // Identity of the base file an append log extends. A log written against
    // another base (crash after compaction, older build rewrote it) is ignored.
    struct BaseIdentity {
        uint64_t bytes = 0, last_id = 0, tracking_since = 0, paused_at = 0, gap_end = 0, gap_until = 0;
        uint32_t active = 0;
        bool operator==(const BaseIdentity&) const = default;
    };
    struct Journal {
        uint64_t start = 0, expiry = 0, tracking_since = 0, paused_at = 0, gap_end = 0, gap_until = 0;
        bool gap = false, loaded = false, seeded = false, dirty = false, active = false;
        uint64_t saved = 0, revision = 0, cache_revision = UINT64_MAX, cache_since = 0, cache_expires = 0, cache_oldest = UINT64_MAX;
        std::unordered_map<std::wstring, ChangeSummary> summaries;
        std::vector<ChangeRecord> records;
        // Summaries built from a window where every event carries a file ID can
        // take later appends without a rebuild: an open-addressing table of
        // identities plus, per record, the previous record of its identity.
        bool cache_incremental = false;
        size_t cache_records = 0;
        uint64_t cache_first_id = 0, cache_last_id = 0, cache_now = 0;
        std::vector<ChangeIdentitySlot> identity_slots;
        std::vector<uint32_t> identity_prev;
        size_t identity_live = 0;
        // Persistence: a complete base plus a log of later flushes. unsaved counts
        // the newest records not yet written; compact forces a full base rewrite.
        BaseIdentity base;
        uint64_t unsaved = 0, log_records = 0, log_bytes = 0;
        bool compact = true;
    };
    void Load(const std::wstring& owner, Journal& journal);
    void ReplayLog(const std::wstring& owner, Journal& journal, std::vector<ChangeRecord>& records, uint64_t now);
    bool Save(const std::wstring& owner, const Journal& journal);
    bool WriteBase(const std::wstring& owner, const std::vector<uint8_t>& bytes);
    bool AppendLog(const std::wstring& owner, const std::vector<uint8_t>& bytes, uint64_t offset);
    static void RebuildSummaries(Journal& journal, uint64_t since, uint64_t now);
    static bool ExtendSummaries(Journal& journal, uint64_t since, uint64_t now);
    static std::vector<uint8_t> SerializeBase(const Journal& journal, BaseIdentity& identity);
    static std::vector<uint8_t> SerializeLog(const Journal& journal, uint64_t fresh);
    void Prune(Journal& journal);
    std::mutex flush_mutex_;
    std::mutex mutex_;
    std::wstring directory_;
    std::unordered_map<std::wstring, Journal> journals_;
    uint64_t next_id_ = 1;
    uint64_t last_flush_tick_ = 0;
};
}
