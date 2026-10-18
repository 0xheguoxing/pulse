#include "change_tracking.h"
#include "change_journal_writer.h"
#include "change_summary_accumulator.h"
#include "index_memory_probe.h"
#include "../ipc/protocol.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>
namespace pulse::index {
namespace {
constexpr uint64_t kRetention = 7 * 86400;
constexpr size_t kMaxRecords = 100000;
constexpr uint32_t kBaseMagic = 0x33484350;  // "PCH3": complete history image
constexpr uint32_t kLegacyMagic = 0x32484350;
constexpr uint32_t kLogMagic = 0x4C484350;   // "PCHL": batches appended after the base
constexpr uint32_t kLogVersion = 1;
constexpr size_t kLogHeaderBytes = 60, kLogEntryHeaderBytes = 8, kLogStateBytes = 40;
// The base is rewritten (compacted) once the log carries this much history;
// between compactions a flush only appends the records added since the last one.
constexpr uint64_t kCompactRecords = kMaxRecords / 2;
constexpr uint64_t kCompactBytes = 16ull * 1024 * 1024;
constexpr uint64_t kMaxLogBytes = 64ull * 1024 * 1024;
constexpr size_t kMaxJournalBytes = 32 * 1024 * 1024;
std::wstring LogFile(const std::wstring& base) { return base + L".log"; }
size_t RecordBytes(const ChangeRecord& e) { return 44 + (e.path.size() + e.old_path.size()) * sizeof(wchar_t); }
// Number of oldest records that fall outside the persisted size budget.
size_t OverBudget(const std::vector<ChangeRecord>& records) {
    size_t bytes = 44, keep = records.size();
    while (keep > 0) {
        const auto cost = RecordBytes(records[keep - 1]);
        if (bytes + cost > kMaxJournalBytes) break;
        bytes += cost; --keep;
    }
    return keep;
}
// Same little-endian / UTF-16 layout as ChangeJournalWriter, into one buffer.
class JournalBytes {
public:
    explicit JournalBytes(size_t reserve) { data_.reserve(reserve); }
    void U32(uint32_t value) { Put(&value, sizeof(value)); }
    void U64(uint64_t value) { Put(&value, sizeof(value)); }
    void String(const std::wstring& value) {
        static_assert(sizeof(wchar_t) == 2);
        U32(static_cast<uint32_t>(value.size())); Put(value.data(), value.size() * sizeof(wchar_t));
    }
    void Record(const ChangeRecord& e) {
        U64(e.id); U64(e.time); U64(e.file_id); U32(static_cast<uint32_t>(e.kind));
        U32(e.is_dir ? 1u : 0u); U32(static_cast<uint32_t>(e.source)); String(e.path); String(e.old_path);
    }
    void Put(const void* bytes, size_t size) {
        const auto* first = static_cast<const uint8_t*>(bytes); data_.insert(data_.end(), first, first + size);
    }
    std::vector<uint8_t>& data() { return data_; }
private:
    std::vector<uint8_t> data_;
};
uint32_t Checksum(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}
bool GetRecord(ipc::PayloadReader& r, ChangeRecord& e, uint32_t& source) {
    uint32_t kind = 0, dir = 0;
    if (!r.GetU64(e.id) || !r.GetU64(e.time) || !r.GetU64(e.file_id) || !r.GetU32(kind) || kind > 5 ||
        !r.GetU32(dir) || !r.GetU32(source) || source > 1 || !r.GetString(e.path) || !r.GetString(e.old_path)) return false;
    e.kind = static_cast<ChangeKind>(kind); e.is_dir = dir != 0;
    return true;
}
bool ReadWhole(const std::wstring& path, uint64_t limit, std::vector<uint8_t>& data, bool& present) {
    present = false;
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) return false;
    present = true;
    input.seekg(0, std::ios::end); const auto size = input.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > limit) return false;
    input.seekg(0); data.resize(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(data.data()), size);
    return static_cast<bool>(input);
}
bool WriteAll(HANDLE file, const std::vector<uint8_t>& bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - done, static_cast<size_t>(1u << 30)));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + done, chunk, &written, nullptr) || written != chunk) return false;
        done += written;
    }
    return true;
}
std::wstring Normalize(std::wstring value) {
    for (auto& c : value) { if (c == L'/') c = L'\\'; c = static_cast<wchar_t>(towlower(c)); }
    if (value.rfind(L"\\\\?\\unc\\", 0) == 0) value = L"\\\\" + value.substr(8);
    else if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
    while (value.size() > 3 && value.back() == L'\\') value.pop_back();
    return value;
}
bool Under(const std::wstring& file, const std::wstring& folder) {
    auto a = Normalize(file), b = Normalize(folder);
    if (b.empty()) return false;
    return a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 &&
                     (b.back() == L'\\' || a[b.size()] == L'\\'));
}
bool RecyclePath(const std::wstring& path) {
    const auto value = Normalize(path);
    // Windows owns this directory only at a local volume root, not an arbitrary
    // folder containing the same name or a similarly named network share.
    if (value.size() < 24 || value[1] != L':' || value.substr(2, 14) != L"\\$recycle.bin\\") return false;
    const auto end = value.find(L'\\', 16);
    if (end == std::wstring::npos || end + 1 == value.size() || value.compare(16, 4, L"s-1-") != 0) return false;
    return std::all_of(value.begin() + 20, value.begin() + end, [](wchar_t c) { return (c >= L'0' && c <= L'9') || c == L'-'; });
}
bool NormalizeRecycleEvent(ChangeRecord& event) {
    const bool current = RecyclePath(event.path), old = RecyclePath(event.old_path);
    if (event.kind == ChangeKind::Renamed && current && !old && !event.old_path.empty()) {
        event.path = event.old_path; event.old_path.clear(); event.kind = ChangeKind::Deleted;
    } else if (event.kind == ChangeKind::Renamed && old && !current) {
        event.old_path.clear(); event.kind = ChangeKind::Created;
    } else if (current || old) return false;
    return true;
}
int Priority(ChangeKind kind) {
    switch (kind) {
    case ChangeKind::Deleted: return 5;
    case ChangeKind::Created: return 4;
    case ChangeKind::MovedIn: case ChangeKind::MovedOut: return 3;
    case ChangeKind::Renamed: return 2;
    default: return 1;
    }
}
using Aliases = std::unordered_map<std::wstring, std::wstring>;
std::wstring Identity(const ChangeRecord& e) {
    const auto path = Normalize(e.path);
    return e.file_id ? path.substr(0, (std::min)(path.size(), size_t(2))) + L":" + std::to_wstring(e.file_id) : path;
}
Aliases EventAliases(const std::vector<ChangeRecord>& records, uint64_t since, uint64_t now) {
    Aliases aliases;
    // Stable file IDs already identify NTFS events. Path aliases are only
    // needed for ID-less events or legacy initial-mtime suppression.
    if (std::none_of(records.begin(), records.end(), [&](const ChangeRecord& e) {
            return e.time >= since && e.time + kRetention >= now &&
                (!e.file_id || e.source == ChangeSource::InitialMtime);
        })) return aliases;
    for (const auto& e : records) if (e.source == ChangeSource::Event && e.time >= since && e.time + kRetention >= now) {
        auto key = Identity(e);
        const auto existing = aliases.find(Normalize(e.path));
        const auto old = aliases.find(Normalize(e.old_path));
        if (!e.file_id && existing != aliases.end()) key = existing->second;
        else if (!e.file_id && old != aliases.end()) key = old->second;
        aliases[Normalize(e.path)] = key;
        if (!e.old_path.empty()) aliases[Normalize(e.old_path)] = key;
    }
    return aliases;
}
std::wstring Key(const ChangeRecord& e, const Aliases& aliases) {
    if (e.file_id) return Identity(e);
    auto found = aliases.find(Normalize(e.path));
    return found == aliases.end() ? Identity(e) : found->second;
}
ChangeRecord Relative(ChangeRecord e, const std::wstring& path) {
    if (e.kind == ChangeKind::Renamed && !e.old_path.empty()) {
        const bool current = Under(e.path, path), old = Under(e.old_path, path);
        if (current != old) e.kind = current ? ChangeKind::MovedIn : ChangeKind::MovedOut;
        else if (current) {
            const auto current_path = Normalize(e.path), old_path = Normalize(e.old_path);
            if (current_path.substr(0, current_path.find_last_of(L'\\')) != old_path.substr(0, old_path.find_last_of(L'\\')))
                e.kind = ChangeKind::MovedIn;
        }
    }
    return e;
}
void Merge(ChangeRecord& into, const ChangeRecord& event) {
    const auto kind = into.kind == ChangeKind::Deleted && event.kind == ChangeKind::Created ? event.kind :
        (Priority(into.kind) >= Priority(event.kind) ? into.kind : event.kind);
    const auto time = (std::max)(into.time, event.time);
    const auto old_path = !event.old_path.empty() ? event.old_path : into.old_path;
    if (event.id >= into.id) into = event;
    into.kind = kind; into.time = time; into.old_path = old_path;
}
template <class Visitor>
void VisitAncestors(const ChangeRecord& event, Visitor visit) {
    const auto current = Normalize(event.path);
    auto walk = [&](std::wstring path, bool old) {
        auto emit = [&] {
            // Common ancestors of old and new paths only receive this event once.
            if (old && (current == path || (current.size() > path.size() && current.compare(0, path.size(), path) == 0 &&
                (path.back() == L'\\' || current[path.size()] == L'\\')))) return;
            visit(path);
        };
        if (path.empty()) return;
        if (event.is_dir) emit();
        while (path.size() > 3) {
            const auto slash = path.find_last_of(L'\\');
            if (slash == std::wstring::npos || slash < 2) break;
            path.resize(slash == 2 && path[1] == L':' ? 3 : slash);
            emit();
        }
    };
    walk(current, false);
    if (!event.old_path.empty()) walk(Normalize(event.old_path), true);
}
template <class Visitor>
void VisitSummaryAncestors(const ChangeRecord& event, Visitor visit) {
    if (event.kind != ChangeKind::Renamed || event.old_path.empty()) {
        VisitAncestors(event, [&](const std::wstring& path) { visit(path, event.kind); });
        return;
    }
    const auto current = Normalize(event.path), previous = Normalize(event.old_path);
    auto under = [](std::wstring_view file, std::wstring_view folder) {
        return !folder.empty() && (file == folder ||
            (file.size() > folder.size() && file.compare(0, folder.size(), folder) == 0 &&
             (folder.back() == L'\\' || file[folder.size()] == L'\\')));
    };
    const bool moved = std::wstring_view(current).substr(0, current.find_last_of(L'\\')) !=
        std::wstring_view(previous).substr(0, previous.find_last_of(L'\\'));
    auto walk = [&](std::wstring path, bool old) {
        auto emit = [&] {
            // The current branch already visited all shared ancestors. Compare
            // normalized paths once per ancestor without copying the event.
            if (old && under(current, path)) return;
            const auto kind = old ? ChangeKind::MovedOut :
                (!under(previous, path) || moved ? ChangeKind::MovedIn : ChangeKind::Renamed);
            visit(path, kind);
        };
        if (path.empty()) return;
        if (event.is_dir) emit();
        while (path.size() > 3) {
            const auto slash = path.find_last_of(L'\\');
            if (slash == std::wstring::npos || slash < 2) break;
            path.resize(slash == 2 && path[1] == L':' ? 3 : slash);
            emit();
        }
    };
    walk(current, false);
    if (!event.old_path.empty()) walk(previous, true);
}
constexpr uint32_t kNoRecord = UINT32_MAX;
bool InWindow(const ChangeRecord& e, uint64_t since, uint64_t now) {
    return e.time >= since && e.time + kRetention >= now;
}
// Observed events with a stable file ID: Key() is Identity(), never an alias.
bool Identified(const ChangeRecord& e) { return e.file_id && e.source == ChangeSource::Event; }
// The two leading characters Identity() takes from the normalized path.
uint32_t IdentityPrefix(const ChangeRecord& e) {
    const auto path = Normalize(e.path);
    uint32_t prefix = path.empty() ? 0u : static_cast<uint16_t>(path[0]);
    if (path.size() > 1) prefix |= static_cast<uint32_t>(static_cast<uint16_t>(path[1])) << 16;
    return prefix;
}
// Linear probing in a power-of-two table: the slot of this identity, or the
// empty slot where it belongs.
size_t SlotIndex(const std::vector<ChangeIdentitySlot>& slots, uint64_t file_id, uint32_t prefix) {
    uint64_t x = file_id ^ (static_cast<uint64_t>(prefix) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    const size_t mask = slots.size() - 1;
    for (size_t i = static_cast<size_t>(x) & mask;; i = (i + 1) & mask) {
        const auto& slot = slots[i];
        if (slot.last == kNoRecord || (slot.file_id == file_id && slot.prefix == prefix)) return i;
    }
}
// Room for `live` identities at no more than 80% load.
void ReserveSlots(std::vector<ChangeIdentitySlot>& slots, size_t live) {
    size_t size = slots.empty() ? 1024 : slots.size();
    while (live * 5 > size * 4) size *= 2;
    if (size == slots.size()) return;
    std::vector<ChangeIdentitySlot> grown(size);
    for (const auto& slot : slots)
        if (slot.last != kNoRecord) grown[SlotIndex(grown, slot.file_id, slot.prefix)] = slot;
    slots.swap(grown);
}
// Accumulates one identity's records still inside the 7 days at `now`,
// oldest first, as the rebuild does. `visited` collects their ancestors.
void RollUp(const std::vector<ChangeRecord>& records, const std::vector<uint32_t>& prev, uint32_t last, uint64_t now,
            std::vector<uint32_t>& chain, ChangeSummaryAccumulator& rollups,
            std::unordered_map<std::wstring, ChangeSummary>& summaries, std::vector<std::wstring>* visited = nullptr) {
    chain.clear();
    for (uint32_t index = last; index != kNoRecord; index = prev[index])
        if (records[index].time + kRetention >= now) chain.push_back(index);
    rollups.Reset();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& event = records[*it];
        VisitSummaryAncestors(event, [&](const std::wstring& ancestor, ChangeKind kind) {
            rollups.Add(summaries[ancestor], kind, event.time, Priority(kind));
            if (visited) visited->push_back(ancestor);
        });
    }
}
std::wstring File(const std::wstring& dir, std::wstring owner) {
    for (auto& c : owner) if (!iswalnum(c) && c != L'-') c = L'_';
    return dir + L"\\changes-" + owner + L".bin";
}
}
std::wstring NormalizeChangePath(std::wstring path) { return Normalize(std::move(path)); }
bool IsChangeJournalName(std::wstring_view name) {
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) name.remove_prefix(slash + 1);
    if (name.size() < 12 || _wcsnicmp(name.data(), L"changes-", 8) != 0) return false;
    const auto dot = name.find(L".bin", 8);
    return dot != std::wstring_view::npos && (dot + 4 == name.size() ||
        (dot + 8 == name.size() && (_wcsnicmp(name.data() + dot + 4, L".tmp", 4) == 0 ||
                                    _wcsnicmp(name.data() + dot + 4, L".log", 4) == 0)));
}
uint64_t ChangeTracker::Now() {
    FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
    return (((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000000) - 11644473600ULL;
}
void ChangeTracker::Open(const std::wstring& directory) {
    std::lock_guard lock(mutex_); directory_ = directory;
}
void ChangeTracker::Load(const std::wstring& owner, Journal& j) {
    if (j.loaded) return;
    j.loaded = true;
    const auto now = Now();
    auto corrupt = [&] { j.gap = true; j.gap_end = now; j.gap_until = now + kRetention; j.dirty = true; j.compact = true; };
    std::vector<uint8_t> data; bool present = false;
    if (!ReadWhole(File(directory_, owner), kMaxJournalBytes, data, present)) {
        if (present) corrupt();
        return;
    }
    ipc::PayloadReader r(data.data(), data.size()); uint32_t magic = 0, count = 0;
    if (!r.GetU32(magic) || (magic != kLegacyMagic && magic != kBaseMagic) ||
        !r.GetU32(count) || count > kMaxRecords) { corrupt(); return; }
    uint64_t origin = 0, paused = 0, gap_end = 0, gap_until = 0; uint32_t active = 0;
    if (magic == kBaseMagic && (!r.GetU64(origin) || !r.GetU64(paused) || !r.GetU64(gap_end) ||
        !r.GetU64(gap_until) || !r.GetU32(active) || active > 1)) { corrupt(); return; }
    const BaseIdentity identity_fields{0, 0, origin, paused, gap_end, gap_until, active};
    std::vector<ChangeRecord> records; uint64_t last_id = 0;
    for (uint32_t i = 0; i < count; ++i) {
        ChangeRecord e; uint32_t source = 0;
        if (!GetRecord(r, e, source)) { corrupt(); return; }
        last_id = (std::max)(last_id, e.id);
        if (source != static_cast<uint32_t>(ChangeSource::Event)) continue;
        if (!NormalizeRecycleEvent(e)) continue;
        next_id_ = (std::max)(next_id_, e.id + 1);
        if (magic == kLegacyMagic && (!origin || e.time < origin)) origin = e.time;
        if (e.time + kRetention >= now) records.push_back(std::move(e));
    }
    if (r.remaining()) { corrupt(); return; }
    j.tracking_since = origin; j.paused_at = paused; j.active = active != 0;
    j.gap_end = gap_end; j.gap_until = gap_until; j.gap = gap_until != 0;
    // v2 had no lifecycle metadata. Preserve captured events, but do not claim
    // continuity across the upgrade/restart or import filesystem mtime history.
    if (magic == kLegacyMagic && origin) {
        j.gap = true; j.gap_end = now; j.gap_until = now + kRetention;
    }
    j.compact = magic == kLegacyMagic;
    if (magic == kBaseMagic) {
        j.base = identity_fields; j.base.bytes = data.size(); j.base.last_id = last_id;
        data = {};
        ReplayLog(owner, j, records, now);
    }
    j.records = std::move(records); ++j.revision; j.dirty = j.compact;
}

void ChangeTracker::ReplayLog(const std::wstring& owner, Journal& j, std::vector<ChangeRecord>& records, uint64_t now) {
    j.log_records = 0; j.log_bytes = 0;
    std::vector<uint8_t> data; bool present = false;
    if (!ReadWhole(LogFile(File(directory_, owner)), kMaxLogBytes, data, present)) {
        // An empty or oversized log is rewritten into the base on the next flush.
        if (present) j.compact = true;
        return;
    }
    ipc::PayloadReader header(data.data(), (std::min)(data.size(), kLogHeaderBytes));
    uint32_t magic = 0, version = 0; BaseIdentity identity;
    if (data.size() < kLogHeaderBytes || !header.GetU32(magic) || magic != kLogMagic || !header.GetU32(version) ||
        version != kLogVersion || !header.GetU64(identity.bytes) || !header.GetU64(identity.last_id) ||
        !header.GetU64(identity.tracking_since) || !header.GetU64(identity.paused_at) || !header.GetU64(identity.gap_end) ||
        !header.GetU64(identity.gap_until) || !header.GetU32(identity.active) || !(identity == j.base)) {
        // Written against another base: a crash between compaction and log
        // removal, or an older build rewrote the base without knowing the log.
        j.compact = true;
        return;
    }
    size_t at = kLogHeaderBytes; uint64_t applied = 0; bool torn = false;
    while (at < data.size()) {
        uint32_t length = 0, checksum = 0;
        if (data.size() - at < kLogEntryHeaderBytes) { torn = true; break; }
        std::memcpy(&length, data.data() + at, sizeof(length)); std::memcpy(&checksum, data.data() + at + 4, sizeof(checksum));
        const auto* payload = data.data() + at + kLogEntryHeaderBytes;
        if (length < kLogStateBytes || length > data.size() - at - kLogEntryHeaderBytes || Checksum(payload, length) != checksum) {
            torn = true; break;
        }
        ipc::PayloadReader r(payload, length);
        uint64_t since = 0, paused = 0, gap_end = 0, gap_until = 0; uint32_t active = 0, count = 0;
        if (!r.GetU64(since) || !r.GetU64(paused) || !r.GetU64(gap_end) || !r.GetU64(gap_until) ||
            !r.GetU32(active) || active > 1 || !r.GetU32(count) || count > kMaxRecords) { torn = true; break; }
        std::vector<ChangeRecord> batch; batch.reserve(count); bool bad = false;
        for (uint32_t i = 0; i < count; ++i) {
            ChangeRecord e; uint32_t source = 0;
            if (!GetRecord(r, e, source)) { bad = true; break; }
            if (source != static_cast<uint32_t>(ChangeSource::Event) || !NormalizeRecycleEvent(e)) continue;
            next_id_ = (std::max)(next_id_, e.id + 1);
            if (e.time + kRetention >= now) batch.push_back(std::move(e));
        }
        if (bad || r.remaining()) { torn = true; break; }
        records.insert(records.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
        j.tracking_since = since; j.paused_at = paused; j.active = active != 0;
        j.gap_end = gap_end; j.gap_until = gap_until; j.gap = gap_until != 0;
        applied += count; at += kLogEntryHeaderBytes + length;
    }
    // Same bounds Prune applied before each batch was written; the persisted
    // gap fields already describe that trimming.
    if (records.size() > kMaxRecords) records.erase(records.begin(), records.begin() + (records.size() - kMaxRecords));
    if (const auto over = OverBudget(records)) records.erase(records.begin(), records.begin() + over);
    j.log_records = applied; j.log_bytes = at;
    // A torn tail (crash or failed append) is folded into a fresh base.
    if (torn) j.compact = true;
}

void ChangeTracker::Prune(Journal& j) {
    const auto old_count = j.records.size();
    const auto now = Now();
    std::erase_if(j.records, [now](const auto& e) { return e.time + kRetention < now; });
    if (j.records.size() > kMaxRecords) {
        j.records.erase(j.records.begin(), j.records.begin() + (j.records.size() - kMaxRecords)); j.gap = true; j.gap_end = Now(); j.gap_until = j.gap_end + kRetention;
    }
    if (const auto keep = OverBudget(j.records)) { j.records.erase(j.records.begin(), j.records.begin() + keep); j.gap = true; j.gap_end = Now(); j.gap_until = j.gap_end + kRetention; }
    j.unsaved = (std::min)(j.unsaved, static_cast<uint64_t>(j.records.size()));
    if (j.records.size() != old_count) { ++j.revision; j.dirty = true; }
}

std::vector<uint8_t> ChangeTracker::SerializeBase(const Journal& j, BaseIdentity& identity) {
    size_t bytes = 44;
    for (const auto& e : j.records) bytes += RecordBytes(e);
    JournalBytes out(bytes);
    out.U32(kBaseMagic); out.U32(static_cast<uint32_t>(j.records.size()));
    out.U64(j.tracking_since); out.U64(j.paused_at); out.U64(j.gap_end); out.U64(j.gap_until); out.U32(j.active ? 1u : 0u);
    identity = {0, 0, j.tracking_since, j.paused_at, j.gap_end, j.gap_until, j.active ? 1u : 0u};
    for (const auto& e : j.records) { out.Record(e); identity.last_id = (std::max)(identity.last_id, e.id); }
    identity.bytes = out.data().size();
    return std::move(out.data());
}

std::vector<uint8_t> ChangeTracker::SerializeLog(const Journal& j, uint64_t fresh) {
    const auto first = j.records.size() - static_cast<size_t>(fresh);
    size_t bytes = kLogHeaderBytes + kLogEntryHeaderBytes + kLogStateBytes;
    for (size_t i = first; i < j.records.size(); ++i) bytes += RecordBytes(j.records[i]);
    JournalBytes out(bytes);
    if (!j.log_bytes) {
        out.U32(kLogMagic); out.U32(kLogVersion); out.U64(j.base.bytes); out.U64(j.base.last_id);
        out.U64(j.base.tracking_since); out.U64(j.base.paused_at); out.U64(j.base.gap_end); out.U64(j.base.gap_until); out.U32(j.base.active);
    }
    const auto entry = out.data().size();
    out.U32(0); out.U32(0);
    out.U64(j.tracking_since); out.U64(j.paused_at); out.U64(j.gap_end); out.U64(j.gap_until); out.U32(j.active ? 1u : 0u);
    out.U32(static_cast<uint32_t>(fresh));
    for (size_t i = first; i < j.records.size(); ++i) out.Record(j.records[i]);
    auto& data = out.data();
    const auto length = static_cast<uint32_t>(data.size() - entry - kLogEntryHeaderBytes);
    const auto checksum = Checksum(data.data() + entry + kLogEntryHeaderBytes, length);
    std::memcpy(data.data() + entry, &length, sizeof(length)); std::memcpy(data.data() + entry + 4, &checksum, sizeof(checksum));
    return std::move(data);
}

bool ChangeTracker::WriteBase(const std::wstring& owner, const std::vector<uint8_t>& bytes) {
    if (directory_.empty()) return false;
    const auto path = File(directory_, owner), temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = WriteAll(file, bytes);
    CloseHandle(file);
    if (!written || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    // A log that survives this (crash, sharing violation) no longer matches the
    // new base identity and is ignored on load.
    DeleteFileW(LogFile(path).c_str());
    return true;
}

bool ChangeTracker::AppendLog(const std::wstring& owner, const std::vector<uint8_t>& bytes, uint64_t offset) {
    if (directory_.empty()) return false;
    const auto path = LogFile(File(directory_, owner));
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        offset ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    // Append only onto exactly what this tracker wrote; anything else compacts.
    LARGE_INTEGER size{}, end{}; end.QuadPart = static_cast<LONGLONG>(offset);
    const bool written = GetFileSizeEx(file, &size) && static_cast<uint64_t>(size.QuadPart) == offset &&
        SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && WriteAll(file, bytes);
    CloseHandle(file);
    return written;
}

bool ChangeTracker::Save(const std::wstring& owner, const Journal& j) {
    BaseIdentity identity;
    return WriteBase(owner, SerializeBase(j, identity));
}

bool ChangeTracker::Lease(const std::wstring& owner, bool enabled) {
    if (owner.empty()) return false;
    std::lock_guard lock(mutex_); auto& j = journals_[owner]; Load(owner, j);
    const auto now = Now();
    if (enabled) {
        if (j.active && j.expiry >= now && !j.paused_at) {
            j.expiry = now + 90;
            return false;
        }
        if (!j.tracking_since) { j.tracking_since = now; ++j.revision; }
        else if (j.expiry < now && (j.active || j.paused_at)) {
            j.gap = true; j.gap_end = now; j.gap_until = now + kRetention;
        }
        if (j.expiry < now) j.start = now;
        j.active = true; j.expiry = now + 90; j.paused_at = 0;
    } else {
        if (j.active) j.paused_at = now;
        j.active = false; j.expiry = 0;
    }
    j.seeded = true; j.dirty = true;
    // Only observed events after tracking starts belong to this journal.
    // Returning false avoids scheduling a full-index mtime baseline scan.
    return false;
}
void ChangeTracker::Seed(const std::wstring& owner, std::vector<ChangeRecord> records) {
    (void)owner; (void)records;
}
void ChangeTracker::Record(ChangeRecord e) {
    if (e.source != ChangeSource::Event || !NormalizeRecycleEvent(e)) return;
    if (IsChangeJournalName(e.path) || IsChangeJournalName(e.old_path)) return;
    if (e.path.empty() || e.path.size() > 32767 || e.old_path.size() > 32767) return;
    std::lock_guard lock(mutex_); const auto now = Now(); if (!e.time) e.time = now;
    for (auto& [owner, j] : journals_) {
        if (j.expiry < now || e.time < j.start || e.time > now + 60) continue;
        bool duplicate = false;
        for (auto it = j.records.rbegin(); it != j.records.rend() && it->time + 2 >= e.time; ++it) {
            if (e.file_id && it->file_id == e.file_id && it->kind != e.kind) break;
            if (it->source == ChangeSource::Event && it->kind == e.kind && (!e.file_id || it->file_id == e.file_id) && Normalize(it->path) == Normalize(e.path) &&
                Normalize(it->old_path) == Normalize(e.old_path)) { duplicate = true; break; }
        }
        if (duplicate) continue;
        e.id = next_id_++; j.records.push_back(e); ++j.revision;
        ++j.unsaved; j.dirty = true;
    }
}
void ChangeTracker::Flush(bool force, IndexMemoryProbe* memory) {
    if (!IndexDiagnosticsEnabled()) memory = nullptr;
    struct PendingSave {
        std::wstring owner;
        std::vector<uint8_t> bytes;
        BaseIdentity identity;
        uint64_t records = 0, offset = 0;
        bool compact = false;
    };
    // Appends must reach the log in serialization order.
    std::lock_guard flush_lock(flush_mutex_);
    std::vector<PendingSave> pending;
    {
        std::lock_guard lock(mutex_);
        const auto tick = GetTickCount64();
        // Queries read live records. Batch persistence and pruning instead of
        // writing the retained history on every journal notification.
        if (!force && last_flush_tick_ && tick - last_flush_tick_ < 60000) return;
        last_flush_tick_ = tick;
        if (memory) {
            memory->Capture(IndexMemoryPoint::ChangeFlushBefore);
            memory->retained.history_records = 0;
            memory->retained.history_vector_capacity_bytes = 0;
            memory->retained.flush_snapshot_records = 0;
            memory->retained.history_tick = tick;
        }
        for (auto& [owner, journal] : journals_) {
            Prune(journal);
            if (memory) {
                memory->retained.history_records += journal.records.size();
                memory->retained.history_vector_capacity_bytes += journal.records.capacity() * sizeof(ChangeRecord);
            }
            if (!journal.dirty) continue;
            PendingSave save; save.owner = owner;
            const auto fresh = (std::min)(journal.unsaved, static_cast<uint64_t>(journal.records.size()));
            save.compact = journal.compact || journal.log_records + fresh > kCompactRecords || journal.log_bytes > kCompactBytes;
            if (save.compact) {
                save.bytes = SerializeBase(journal, save.identity); save.records = journal.records.size();
            } else {
                save.bytes = SerializeLog(journal, fresh); save.records = fresh; save.offset = journal.log_bytes;
            }
            if (memory) memory->retained.flush_snapshot_records += save.records;
            journal.unsaved = 0; journal.dirty = false;
            pending.push_back(std::move(save));
        }
    }
    // Disk I/O holds neither the index lock nor the tracker lock.
    if (memory) memory->Capture(IndexMemoryPoint::ChangeFlushSnapshot);
    for (const auto& save : pending) {
        const bool written = save.compact ? WriteBase(save.owner, save.bytes) : AppendLog(save.owner, save.bytes, save.offset);
        std::lock_guard lock(mutex_);
        auto& journal = journals_[save.owner];
        if (written && save.compact) {
            journal.base = save.identity; journal.log_records = 0; journal.log_bytes = 0; journal.compact = false;
        } else if (written) {
            journal.log_records += save.records; journal.log_bytes = save.offset + save.bytes.size();
        } else {
            journal.gap = true; journal.gap_end = Now(); journal.gap_until = journal.gap_end + kRetention;
            journal.dirty = true; journal.compact = true;
        }
    }
    if (memory) {
        pending.clear();
        memory->Capture(IndexMemoryPoint::ChangeFlushAfter);
    }
}

void ChangeTracker::Gap() {
    std::lock_guard lock(mutex_); const auto now = Now();
    for (auto& [owner, j] : journals_) {
        if (!j.active || j.expiry < now) continue;
        j.gap = true; j.gap_end = now; j.gap_until = now + kRetention; j.dirty = true;
    }
}
ChangeResponse ChangeTracker::Details(const std::wstring& owner, const std::wstring& path,
                                     uint64_t since, uint64_t before, uint32_t limit, uint32_t kind_filter) {
    std::lock_guard lock(mutex_); ChangeResponse out;
    if (path.empty()) return out;
    auto& j = journals_[owner]; Load(owner, j); const auto now = Now();
    since = (std::max)(since, j.tracking_since);
    out.state = j.expiry < now ? ChangeState::Unavailable :
        (j.gap && now < j.gap_until && since <= j.gap_end ? ChangeState::Gap : ChangeState::Available);
    limit = (std::clamp)(limit, 1u, 200u);
    const auto aliases = EventAliases(j.records, since, now);
    std::unordered_map<std::wstring, ChangeRecord> collapsed;
    for (const auto& event : j.records) {
        if (event.time < since || event.time + kRetention < now) continue;
        if (event.source == ChangeSource::InitialMtime && aliases.contains(Normalize(event.path))) continue;
        if (!Under(event.path, path) && (event.old_path.empty() || !Under(event.old_path, path))) continue;
        auto e = Relative(event, path);
        const auto key = Key(e, aliases);
        auto [it, added] = collapsed.try_emplace(key, e);
        if (!added) Merge(it->second, e);
    }
    std::vector<ChangeRecord> records;
    for (auto& [key, e] : collapsed) {
        if ((before && e.id >= before) || (kind_filter != UINT32_MAX && static_cast<uint32_t>(e.kind) != kind_filter)) continue;
        records.push_back(std::move(e));
    }
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.id > b.id; });
    if (records.size() > limit) { records.resize(limit); out.next_cursor = records.back().id; }
    out.records = std::move(records); return out;
}
void ChangeTracker::RebuildSummaries(Journal& j, uint64_t since, uint64_t now) {
    j.cache_expires = now + kRetention; j.cache_oldest = UINT64_MAX;
    j.summaries.clear();
    j.identity_slots.clear(); j.identity_prev.clear(); j.identity_live = 0;
    j.cache_incremental = std::all_of(j.records.begin(), j.records.end(),
        [&](const ChangeRecord& e) { return !InWindow(e, since, now) || Identified(e); });
    j.cache_records = j.records.size();
    j.cache_first_id = j.records.empty() ? 0 : j.records.front().id;
    j.cache_last_id = j.records.empty() ? 0 : j.records.back().id;
    j.cache_now = now;
    if (j.cache_incremental && j.records.size() < kNoRecord) {
        // Every event has a file ID: Key() is the identity, no aliases apply.
        j.identity_prev.assign(j.records.size(), kNoRecord);
        for (size_t index = 0; index < j.records.size(); ++index) {
            const auto& event = j.records[index];
            if (!InWindow(event, since, now)) continue;
            j.cache_expires = (std::min)(j.cache_expires, event.time + kRetention + 1);
            j.cache_oldest = (std::min)(j.cache_oldest, event.time);
            const uint32_t prefix = IdentityPrefix(event);
            ReserveSlots(j.identity_slots, j.identity_live + 1);
            auto& slot = j.identity_slots[SlotIndex(j.identity_slots, event.file_id, prefix)];
            if (slot.last == kNoRecord) { slot.file_id = event.file_id; slot.prefix = prefix; ++j.identity_live; }
            j.identity_prev[index] = slot.last;
            slot.last = static_cast<uint32_t>(index);
        }
        ChangeSummaryAccumulator rollups;
        std::vector<uint32_t> chain;
        for (const auto& slot : j.identity_slots) {
            if (slot.last == kNoRecord) continue;
            RollUp(j.records, j.identity_prev, slot.last, now, chain, rollups, j.summaries);
            rollups.Commit();
        }
        return;
    }
    j.cache_incremental = false;
    j.identity_slots.shrink_to_fit(); j.identity_prev.shrink_to_fit();
    const auto aliases = EventAliases(j.records, since, now);
    struct Chain { size_t first, last; };
    std::unordered_map<std::wstring, Chain> identities;
    std::vector<size_t> next(j.records.size(), SIZE_MAX);
    std::unordered_set<std::wstring> initial_paths;
    for (const auto& event : j.records) {
        if (event.time < since || event.time + kRetention < now) continue;
        if (event.source == ChangeSource::InitialMtime && aliases.contains(Normalize(event.path))) continue;
        j.cache_expires = (std::min)(j.cache_expires, event.time + kRetention + 1);
        j.cache_oldest = (std::min)(j.cache_oldest, event.time);
        if (event.source == ChangeSource::InitialMtime) {
            if (!initial_paths.insert(Normalize(event.path)).second) continue;
            VisitAncestors(event, [&](const std::wstring& ancestor) {
                auto& summary = j.summaries[ancestor];
                ++summary.count; ++summary.initial_count;
                summary.last_change = (std::max)(summary.last_change, event.time);
            });
            continue;
        }
        const auto key = Key(event, aliases);
        const size_t index = static_cast<size_t>(&event - j.records.data());
        const auto [identity, inserted] = identities.try_emplace(key, Chain{index, index});
        if (!inserted) {
            next[identity->second.last] = index;
            identity->second.last = index;
        }
    }
    // Aggregate one file identity at a time, in original event order. The
    // scratch vector scales with its ancestors, not all files x ancestors.
    // unordered_map rehash preserves the summary pointers stored here.
    ChangeSummaryAccumulator rollups;
    for (const auto& [key, chain] : identities) {
        (void)key;
        rollups.Reset();
        for (size_t index = chain.first; index != SIZE_MAX; index = next[index]) {
            const auto& event = j.records[index];
            VisitSummaryAncestors(event, [&](const std::wstring& ancestor, ChangeKind kind) {
                rollups.Add(j.summaries[ancestor], kind, event.time, Priority(kind));
            });
        }
        rollups.Commit();
    }
}

bool ChangeTracker::ExtendSummaries(Journal& j, uint64_t since, uint64_t now) {
    const size_t old_count = j.cache_records, count = j.records.size();
    if (!j.cache_incremental || !old_count || count < old_count || count >= kNoRecord || now < j.cache_now ||
        j.identity_prev.size() != old_count || j.records.front().id != j.cache_first_id ||
        j.records[old_count - 1].id != j.cache_last_id) return false;
    // Records that changed the window: those of the cached window that aged
    // past the 7 days since, and the in-window events appended after it.
    struct Touch { uint64_t file_id; uint32_t prefix; uint32_t index; bool fresh; };
    std::vector<Touch> touched;
    uint64_t expires = j.cache_expires, oldest = j.cache_oldest;
    const bool aging = now >= j.cache_expires;
    if (aging) {
        expires = now + kRetention; oldest = UINT64_MAX;
        for (size_t index = 0; index < old_count; ++index) {
            const auto& event = j.records[index];
            if (event.time < since || event.time + kRetention < j.cache_now) continue;  // never in the cached window
            if (event.time + kRetention < now) {
                touched.push_back({event.file_id, IdentityPrefix(event), static_cast<uint32_t>(index), false});
                continue;
            }
            expires = (std::min)(expires, event.time + kRetention + 1);
            oldest = (std::min)(oldest, event.time);
        }
    }
    for (size_t index = old_count; index < count; ++index) {
        const auto& event = j.records[index];
        if (!InWindow(event, since, now)) continue;
        if (!Identified(event)) return false;
        touched.push_back({event.file_id, IdentityPrefix(event), static_cast<uint32_t>(index), true});
        expires = (std::min)(expires, event.time + kRetention + 1);
        oldest = (std::min)(oldest, event.time);
    }
    // One group per identity, its records still in record order.
    std::stable_sort(touched.begin(), touched.end(), [](const Touch& a, const Touch& b) {
        return a.file_id != b.file_id ? a.file_id < b.file_id : a.prefix < b.prefix;
    });
    const auto group_end = [&](size_t i) {
        size_t end = i + 1;
        while (end < touched.size() && touched[end].file_id == touched[i].file_id && touched[end].prefix == touched[i].prefix) ++end;
        return end;
    };
    // A touched identity is rolled up again over its older records. Past a
    // quarter of the history a rebuild costs about the same; take it instead.
    size_t budget = (std::max)(size_t(4096), count / 4);
    if (!j.identity_slots.empty()) {
        for (size_t i = 0; i < touched.size(); i = group_end(i)) {
            const auto& slot = j.identity_slots[SlotIndex(j.identity_slots, touched[i].file_id, touched[i].prefix)];
            for (uint32_t index = slot.last; index != kNoRecord; index = j.identity_prev[index]) {
                if (!budget) return false;
                --budget;
            }
        }
    }
    j.identity_prev.resize(count, kNoRecord);
    ChangeSummaryAccumulator rollups;
    std::vector<uint32_t> chain;
    std::vector<std::wstring> emptied;  // ancestors that may have lost their last identity
    for (size_t i = 0; i < touched.size();) {
        const size_t end = group_end(i);
        const bool aged = std::any_of(touched.begin() + i, touched.begin() + end, [](const Touch& t) { return !t.fresh; });
        ReserveSlots(j.identity_slots, j.identity_live + 1);
        auto& slot = j.identity_slots[SlotIndex(j.identity_slots, touched[i].file_id, touched[i].prefix)];
        if (slot.last == kNoRecord) {
            slot.file_id = touched[i].file_id; slot.prefix = touched[i].prefix; ++j.identity_live;
        } else {
            RollUp(j.records, j.identity_prev, slot.last, j.cache_now, chain, rollups, j.summaries, aged ? &emptied : nullptr);
            rollups.Retract();
        }
        for (size_t k = i; k < end; ++k) {
            if (!touched[k].fresh) continue;
            j.identity_prev[touched[k].index] = slot.last;
            slot.last = touched[k].index;
        }
        RollUp(j.records, j.identity_prev, slot.last, now, chain, rollups, j.summaries);
        rollups.Commit();
        i = end;
    }
    // A rebuild has no entry for a folder without identities.
    for (const auto& ancestor : emptied) {
        const auto found = j.summaries.find(ancestor);
        if (found != j.summaries.end() && !found->second.count) j.summaries.erase(found);
    }
    j.cache_records = count;
    j.cache_last_id = j.records.back().id;
    j.cache_expires = expires; j.cache_oldest = oldest; j.cache_now = now;
    return true;
}

ChangeResponse ChangeTracker::Summaries(const std::wstring& owner, const std::vector<std::wstring>& paths, uint64_t since) {
    std::lock_guard lock(mutex_); ChangeResponse out;
    auto& j = journals_[owner]; Load(owner, j); const auto now = Now();
    since = (std::max)(since, j.tracking_since);
    out.state = j.expiry < now ? ChangeState::Unavailable :
        (j.gap && now < j.gap_until && since <= j.gap_end ? ChangeState::Gap : ChangeState::Available);
    const bool since_moved = since < j.cache_since || since > j.cache_oldest;
    if (j.cache_revision != j.revision || since_moved || now >= j.cache_expires) {
        // Between flushes the journal only gains events at the end and loses
        // the oldest to the 7-day limit; apply both to the cached rollups and
        // rebuild only when that would not be exact.
        if (since_moved || !ExtendSummaries(j, since, now)) RebuildSummaries(j, since, now);
        j.cache_revision = j.revision; j.cache_since = since;
    }
    j.cache_since = since;
    for (const auto& path : paths) {
        ChangeSummary summary;
        const auto found = j.summaries.find(Normalize(path));
        if (found != j.summaries.end()) summary = found->second;
        summary.path = path; summary.state = out.state;
        summary.incomplete = out.state != ChangeState::Available;
        out.summaries.push_back(std::move(summary));
    }
    return out;
}
}
