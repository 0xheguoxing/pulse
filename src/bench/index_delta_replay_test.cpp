#include "../index/index_engine.h"
#include "../index/index_delta.h"
#include "../index/index_paths.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>

namespace pulse::index {
struct EngineTestAccess {
    static bool NotifyRestart() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << "\n"; ok &= value;
        };
        const auto root = std::filesystem::absolute(std::filesystem::path("bench_data") /
            ("notify-restart-" + std::to_string(GetCurrentProcessId())));
        const auto data = root / "files", cache = root / "cache";
        std::filesystem::create_directories(data); std::filesystem::create_directories(cache);
        SetMachineIndexScope(false); SetActiveIndexDirectory(cache.wstring());
        auto write = [&](const wchar_t* name, const char* value) { std::ofstream(data / name) << value; };
        auto notify = [&](Engine& engine, std::initializer_list<std::pair<DWORD, std::wstring>> events) {
            std::vector<BYTE> packet;
            for (const auto& [action, name] : events) {
                const auto offset = packet.size();
                const auto length = (offsetof(FILE_NOTIFY_INFORMATION, FileName) + name.size() * sizeof(wchar_t) + 3) & ~size_t{3};
                packet.resize(offset + length);
                auto event = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data() + offset);
                event->NextEntryOffset = static_cast<DWORD>(length);
                event->Action = action; event->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
                memcpy(event->FileName, name.data(), event->FileNameLength);
            }
            size_t offset = 0;
            while (offset < packet.size()) {
                auto event = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data() + offset);
                if (offset + event->NextEntryOffset == packet.size()) { event->NextEntryOffset = 0; break; }
                offset += event->NextEntryOffset;
            }
            return engine.ApplyNotifyLocked(data.wstring(), packet.data(), static_cast<DWORD>(packet.size()));
        };
        write(L"old.txt", "old"); write(L"remove.txt", "remove");
        {
            Engine engine; engine.running_ = true; engine.ready_ = true; engine.built_unix_ = 123456;
            engine.AddForTest((data / L"old.txt").wstring(), L"old.txt", false, 3);
            engine.AddForTest((data / L"remove.txt").wstring(), L"remove.txt", false, 6);
            check(PublishBase(engine), "publish initial walk snapshot");
            engine.OpenDeltasLocked();
            write(L"added.txt", "12345678");
            notify(engine, {{FILE_ACTION_ADDED, L"added.txt"}});
            std::filesystem::rename(data / L"old.txt", data / L"renamed.txt");
            notify(engine, {{FILE_ACTION_RENAMED_OLD_NAME, L"old.txt"}, {FILE_ACTION_RENAMED_NEW_NAME, L"renamed.txt"}});
            std::filesystem::remove(data / L"remove.txt");
            notify(engine, {{FILE_ACTION_REMOVED, L"remove.txt"}});
            engine.Stop();
        }
        for (int restart = 0; restart < 2; ++restart) {
            Engine engine; engine.running_ = true;
            check(engine.TryLoadCache(), "restart loads saved base and production notification WAL");
            Query q; q.needle = L"ext:txt"; q.path_prefix = data.wstring(); q.limit = 100;
            const auto result = engine.Search(q);
            bool added = false, renamed = false, stale = false;
            for (const auto& hit : result.hits) {
                added |= hit.name == L"added.txt" && hit.size == 8;
                renamed |= hit.name == L"renamed.txt";
                stale |= hit.name == L"old.txt" || hit.name == L"remove.txt";
            }
            check(result.total == 2 && added && renamed && !stale, "two immediate restarts retain add/rename/delete changes");
            engine.Stop();
        }
        write(L"offline.txt", "offline"); std::filesystem::remove(data / L"added.txt");
        {
            Engine engine; engine.StartFixture(nullptr, 0, data.wstring());
            bool reconciled = false;
            for (int attempt = 0; attempt < 300 && !reconciled; ++attempt) {
                Query q; q.needle = L"ext:txt"; q.path_prefix = data.wstring(); q.limit = 100;
                const auto result = engine.Search(q);
                bool offline = false, added = false;
                for (const auto& hit : result.hits) { offline |= hit.name == L"offline.txt"; added |= hit.name == L"added.txt"; }
                reconciled = offline && !added && result.total == 2;
                if (!reconciled) Sleep(50);
            }
            check(reconciled, "actual worker startup reconciles changes made while stopped");
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        std::filesystem::remove_all(root);
        return ok;
    }
    static bool PublishBase(Engine& engine) {
        Engine::Store store;
        std::vector<Engine::VolState> volumes;
        const auto path = CacheFilePath();
        return engine.FlattenLocked(store, volumes) && engine.WriteIndexFile(path, store, volumes, engine.built_unix_) &&
            MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    static void MakeLegacy(const std::wstring& path) {
        std::ifstream input(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        input.close();
        // A genuine v1 Add has no ID after its one-byte opcode.
        bytes.erase(bytes.begin() + 17, bytes.begin() + 21);
        bytes[4] = 1;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    static void Init(Engine& engine) {
        engine.running_ = true;
        engine.ready_ = true;
        engine.built_unix_ = 123456;
        for (wchar_t letter : {L'C', L'D'}) {
            Engine::VolState volume;
            volume.letter = letter;
            volume.volume_id = std::wstring(L"replay-fixture-") + letter;
            volume.root_idx = engine.AddNodeLocked(engine.live_, -1, std::wstring(1, letter) + L":",
                Engine::kFlagDir, 0, 0, 0, true);
            volume.first_idx = volume.root_idx;
            engine.vols_.push_back(std::move(volume));
        }
    }
    static bool Run() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"delta-replay-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        SetMachineIndexScope(false);
        auto select = [&](const wchar_t* name) {
            const auto directory = root / name;
            std::filesystem::create_directories(directory);
            SetActiveIndexDirectory(directory.wstring());
        };
        for (bool reversed : {false, true}) {
            select(reversed ? L"reverse" : L"interleaved");
            {
                Engine engine; Init(engine);
                DeltaLog c, d;
                check(c.Open(DeltaFilePathForVolume(engine.vols_[0].volume_id), engine.built_unix_) &&
                      d.Open(DeltaFilePathForVolume(engine.vols_[1].volume_id), engine.built_unix_), "open isolated volume WALs");
                d.QueueAdd(2, 1, Engine::kFlagDir, 0, 0, L"directory", 100);
                c.QueueAdd(3, 0, 0, 0, 1, L"file.txt", 200);
                d.QueueAdd(4, 2, 0, 0, 2, L"child.txt", 101);
                c.QueueAdd(5, 0, 0, 0, 3, L"deleted.txt", 201);
                d.QueuePatch(2, static_cast<uint8_t>(PatchBits::Name), 0, 0, 0, 0, L"renamed");
                d.QueuePatch(4, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 42, 987, {});
                c.QueueTomb(5);
                c.QueueUsn(91, 111); d.QueueUsn(92, 222);
                check(c.Flush() && d.Flush(), "flush interleaved additions, patches, tombstone and cursors");
            }
            {
                Engine engine; Init(engine);
                if (reversed) std::reverse(engine.vols_.begin(), engine.vols_.end());
                check(engine.ReplayDeltasLocked(), "replay accepts stable IDs independent of volume order");
                check(engine.LiveCount() == 6 && engine.NodeAt(4).parent == 2 &&
                      (engine.NodeAt(2).flags & Engine::kFlagDir) && engine.NameOf(2) == L"renamed" &&
                      engine.NameOf(3) == L"file.txt", "child retains directory parent and no cross-volume reassignment");
                check(engine.AttrAt(4).size == 987 && engine.AttrAt(4).mtime == 42 &&
                      engine.IsTomb(5) && !engine.IsTomb(4), "patches and tombstones retain original targets");
                const auto& d = engine.vols_[reversed ? 0 : 1];
                check(engine.FindByFrnLocked(d, 100) == 2 && engine.FindByFrnLocked(d, 101) == 4 &&
                      d.journal_id == 92 && d.next_usn == 222, "volume FRN mapping and journal checkpoint survive replay");
            }
        }
        for (const auto* mode : {L"legacy", L"gap", L"duplicate", L"truncated", L"bad-parent"}) {
            select(mode);
            Engine engine; Init(engine);
            const auto path = DeltaFilePathForVolume(engine.vols_[0].volume_id);
            {
                DeltaLog log; check(log.Open(path, engine.built_unix_), "open rejection fixture");
                if (std::wstring_view(mode) == L"gap") log.QueueAdd(3, 0, 0, 0, 0, L"gap", 300);
                else {
                    log.QueueAdd(2, 0, 0, 0, 0, L"file", 300);
                    if (std::wstring_view(mode) == L"duplicate") log.QueueAdd(2, 0, 0, 0, 0, L"duplicate", 301);
                    if (std::wstring_view(mode) == L"bad-parent") log.QueueAdd(3, 2, 0, 0, 0, L"child", 301);
                }
                check(log.Flush(), "flush rejection fixture");
            }
            if (std::wstring_view(mode) == L"legacy") {
                MakeLegacy(path);
            }
            if (std::wstring_view(mode) == L"truncated") std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
            check(!engine.ReplayDeltasLocked(), "reject legacy, noncontiguous, duplicate, truncated or invalid hierarchy WAL");
            if (std::wstring_view(mode) != L"bad-parent") check(engine.LiveCount() == 2, "failed preflight leaves original snapshot untouched");
            check(std::filesystem::exists(path), "rejected WAL remains available for diagnosis");
        }
        select(L"restart");
        {
            Engine engine; Init(engine);
            check(PublishBase(engine), "publish valid mapped baseline for legacy recovery");
            const auto path = DeltaFilePathForVolume(engine.vols_[0].volume_id);
            {
                DeltaLog log; check(log.Open(path, engine.built_unix_), "open legacy recovery WAL");
                log.QueueAdd(2, 0, 0, 0, 0, L"legacy-file", 300);
                check(log.Flush(), "flush legacy recovery WAL");
            }
            MakeLegacy(path);
        }
        {
            Engine rejected;
            check(!rejected.TryLoadCache() && !rejected.ready_ && rejected.folder_size_gap_ && rejected.LiveCount() == 0,
                "legacy cache load requests rebuild without exposing partial or complete stale results");
        }
        {
            // Exercise the publication/open-WAL sequence used after FullRebuild,
            // without enumerating any real volume or starting the worker.
            Engine recovered; Init(recovered); recovered.built_unix_ = 654321;
            check(PublishBase(recovered), "publish replacement baseline after legacy rejection");
            recovered.OpenDeltasLocked();
            auto* log = recovered.DeltaFor(L'C');
            check(log && log->BaseBuilt() == 654321, "replacement baseline opens v2 WAL instead of legacy records");
            if (log) {
                log->QueueAdd(2, 0, 0, 0, 7, L"after-recovery.txt", 400);
                check(log->Flush(), "persist stable-ID addition after recovery");
            }
        }
        for (int restart = 0; restart < 2; ++restart) {
            Engine restored;
            check(restored.TryLoadCache() && restored.ready_ && restored.LiveCount() == 3 &&
                  restored.NameOf(2) == L"after-recovery.txt" && restored.AttrAt(2).size == 7,
                "v2 recovery survives consecutive restarts without repeated rebuild");
        }
        for (const auto* mode : {L"cross-add", L"cross-patch", L"cross-move", L"cross-tomb"}) {
            select(mode);
            Engine engine; Init(engine);
            {
                DeltaLog log;
                check(log.Open(DeltaFilePathForVolume(engine.vols_[0].volume_id), engine.built_unix_),
                    "open cross-volume ownership fixture");
                if (std::wstring_view(mode) == L"cross-add") log.QueueAdd(2, 1, 0, 0, 0, L"wrong-volume", 500);
                else if (std::wstring_view(mode) == L"cross-patch")
                    log.QueuePatch(1, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 42, 99, {});
                else if (std::wstring_view(mode) == L"cross-tomb") log.QueueTomb(1);
                else {
                    log.QueueAdd(2, 0, 0, 0, 0, L"same-volume", 500);
                    log.QueuePatch(2, static_cast<uint8_t>(PatchBits::Meta), 1, 0, 0, 0, {});
                }
                check(log.Flush(), "flush cross-volume ownership fixture");
            }
            check(!engine.ReplayDeltasLocked(), "reject cross-volume addition, patch target, move or tombstone");
        }
        select(L"walk");
        {
            Engine engine; Init(engine); engine.vols_.clear();
            {
                DeltaLog log; check(log.Open(DeltaFilePath(0), engine.built_unix_), "open walk-mode WAL");
                log.QueueAdd(2, 0, 0, 0, 9, L"walk-file.txt", 0);
                check(log.Flush(), "flush walk-mode WAL");
            }
            check(engine.ReplayDeltasLocked() && engine.NameOf(2) == L"walk-file.txt" && engine.AttrAt(2).size == 9,
                "walk-mode replay does not require volume ownership metadata");
            MakeLegacy(DeltaFilePath(0));
            engine.built_unix_ = 654321;
            engine.OpenDeltasLocked();
            const auto* log = engine.DeltaFor(0);
            check(log && log->BaseBuilt() == 654321, "walk-mode recovery replaces rejected legacy WAL after rebuilding");
        }
        select(L"scale");
        {
            // A merge that never succeeds leaves every addition and tombstone in
            // the WAL. Atomic saves recreate one name in one folder repeatedly;
            // replay used to rescan that name's child-map range per tombstone.
            constexpr int32_t kRecreated = 100000;
            Engine engine; Init(engine);
            {
                DeltaLog log; check(log.Open(DeltaFilePathForVolume(engine.vols_[0].volume_id), engine.built_unix_),
                    "open repeated-name WAL");
                for (int32_t i = 0; i < kRecreated; ++i)
                    log.QueueAdd(2 + i, 0, 0, 0, static_cast<uint64_t>(i), L"the-real-index", 10000 + static_cast<uint64_t>(i));
                for (int32_t i = 0; i + 1 < kRecreated; ++i) log.QueueTomb(2 + i);
                check(log.Flush(), "flush repeated-name WAL");
            }
            const auto started = GetTickCount64();
            const bool replayed = engine.ReplayDeltasLocked();
            const auto elapsed = GetTickCount64() - started;
            std::cout << "  repeated-name replay: " << kRecreated << " adds + " << (kRecreated - 1)
                      << " tombstones in " << elapsed << " ms\n";
            const int32_t survivor = 2 + kRecreated - 1;
            check(replayed && engine.FindChildLiveLocked(0, L"the-real-index") == survivor &&
                  engine.FindByFrnLocked(engine.vols_[0], 10000 + kRecreated - 1) == survivor &&
                  engine.indexed_.load() == 3, "repeated-name replay keeps only the surviving node");
            check(elapsed < 15000, "repeated-name replay is not quadratic in the WAL length");
        }
        {
            // A live file under a tombstoned directory used to be written as a
            // parentless FileRoot, failing every merge from then on.
            Engine engine; Init(engine);
            const int32_t dir = engine.AddNodeLocked(engine.live_, 0, L"gone", Engine::kFlagDir, 0, 0, 0, true);
            const int32_t orphan = engine.AddNodeLocked(engine.live_, dir, L"orphan.txt", 0, 0, 0, 0, true);
            const int32_t nested = engine.AddNodeLocked(engine.live_, dir, L"sub", Engine::kFlagDir, 0, 0, 0, true);
            engine.AddNodeLocked(engine.live_, nested, L"deep.txt", 0, 0, 0, 0, true);
            const int32_t kept = engine.AddNodeLocked(engine.live_, 1, L"kept.txt", 0, 0, 0, 0, true);
            engine.tombstones_.insert(dir); ++engine.deleted_;
            Engine::Store store;
            std::vector<Engine::VolState> volumes;
            check(engine.FlattenLocked(store, volumes), "flatten with an orphaned subtree");
            bool orphan_written = false, kept_written = false;
            for (const auto& node : store.nodes) {
                const std::wstring_view name(store.pool.data() + node.off, node.len);
                orphan_written |= name == L"orphan.txt" || name == L"deep.txt" || name == L"sub";
                kept_written |= name == L"kept.txt";
            }
            check(!orphan_written && kept_written && store.nodes.size() == 3,
                "orphaned subtree is dropped while reachable nodes are kept");
            check(PublishBase(engine), "merge publishes instead of failing hierarchy validation");
            (void)orphan; (void)kept;
        }
        {
            Engine engine; Init(engine);
            auto& volume = engine.vols_[0];
            const int32_t a = engine.AddNodeLocked(engine.live_, 0, L"a.txt", 0, 77, 0, 0, true, &volume);
            const int32_t b = engine.AddNodeLocked(engine.live_, 0, L"b.txt", 0, 88, 0, 0, true, &volume);
            const int32_t c = engine.AddNodeLocked(engine.live_, 0, L"c.txt", 0, 77, 0, 0, true, &volume);
            std::vector<int32_t> links;
            engine.CollectFrnLocked(volume, 77, links);
            check(engine.FindByFrnLocked(volume, 77) == c && engine.FindByFrnLocked(volume, 88) == b &&
                  engine.FindByFrnLocked(volume, 99) == -1 && links == std::vector<int32_t>{a, c},
                "indexed FRN lookup keeps newest-wins and oldest-first hard-link order");
            // Production lookups run against a mapped base; frn_build is only
            // the MFT-rebuild staging list and stays empty once a base exists.
            static const DiskFrn mapped_base[1] = {{1, 0, 0}};
            volume.frn_base = mapped_base; volume.frn_base_n = 1; volume.frn_build.clear();
            constexpr int32_t kEntries = 200000;
            for (int32_t i = 0; i < kEntries; ++i)
                engine.MapFrnLocked(volume, 1000000 + static_cast<uint64_t>(i), c);
            const auto started = GetTickCount64();
            int32_t misses = 0;
            for (int32_t i = 0; i < kEntries; ++i)
                misses += engine.FindByFrnLocked(volume, 5000000 + static_cast<uint64_t>(i)) == -1;
            const auto elapsed = GetTickCount64() - started;
            std::cout << "  " << kEntries << " FRN misses over " << volume.frn_new.size()
                      << " unmerged entries in " << elapsed << " ms\n";
            check(misses == kEntries && elapsed < 5000, "FRN lookup does not scan unmerged entries");
        }
        SetActiveIndexDirectory(L"");
        std::error_code error;
        std::filesystem::remove_all(root, error);
        check(!error, "isolated fixture cleanup");
        return ok;
    }
};
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--notify-restart-only")
        return pulse::index::EngineTestAccess::NotifyRestart() ? 0 : 1;
    return pulse::index::EngineTestAccess::Run() ? 0 : 1;
}
