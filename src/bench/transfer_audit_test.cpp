#include "../ops/ops_manager.h"
#include "../ipc/shell_client.h"
#include "../ops/shell_command_template.h"
#include "../ops/elevated_transfer.h"
#include "../ops/elevated_transfer_client.h"
#include "../ops/source_listing.h"
#include <aclapi.h>
#include <algorithm>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>

namespace pulse::ipc {
std::function<void(ShellClient&, uint32_t, uint32_t)> audit_submit;
bool InterceptShellSubmitForReview(ShellClient& client, uint32_t type, uint32_t id) {
    if (!audit_submit) return false;
    audit_submit(client, type, id);
    return true;
}
struct ShellClientReviewTestAccess {
    static bool Configure(const ShellClient::Callbacks& cb) {
        auto& client = ShellClient::Instance();
        if (client.running_ || client.reader_.joinable() || client.connection_ || client.child_started_ || !client.pending_.empty()) return false;
        client.cb_ = cb;
        client.running_ = true;
        return true;
    }
    static void Reset() {
        auto& client = ShellClient::Instance();
        client.running_ = false;
        client.cb_ = {};
    }
    static void Done(ShellClient& client, uint32_t id) {
        client.FireDone(id, 107, false, L"accepted before immediate callback");
    }
};
}
namespace pulse::ops {
std::atomic<unsigned> audit_starts{0};
std::function<void(OpsManager&, const std::wstring&)> audit_scan;
std::function<void(const ipc::ShellClient::Callbacks&)> audit_callbacks;
void ObserveTransferStartForReview(OpsManager&, uint64_t task) {
    if (task) ++audit_starts;
}
void ObserveTransferScanForReview(OpsManager& manager, const std::wstring& path) {
    if (audit_scan) audit_scan(manager, path);
}
void ObserveShellCallbacksForReview(const ipc::ShellClient::Callbacks& cb) {
    if (audit_callbacks) audit_callbacks(cb);
}
struct OpsManagerAuditAccess {
    static bool RouteInterleavedResults() {
        OpsManager manager;
        manager.own_shell_client_ = false;
        ipc::ShellClient::Callbacks callbacks;
        audit_callbacks = [&](const auto& cb) { callbacks = cb; };
        manager.ConfigureShellCallbacks();
        audit_callbacks = {};
        if (!callbacks.done) return false;
        manager.file_op_ids_.insert(10);
        manager.file_op_ids_.insert(11);
        manager.ctx_invoke_ids_.insert(12);
        manager.menu_session_by_token_[70] = 13;
        manager.menu_token_by_session_[13] = 70;
        uint32_t delivered = 0;
        manager.SetShellMenuCallback([&](uint32_t token, auto items, bool partial, auto) {
            if (items.empty() && !partial) delivered = token;
        });
        callbacks.done(10, 101, false, L"first");
        callbacks.done(12, 102, false, L"menu");
        callbacks.done(11, 103, true, L"second");
        callbacks.done(13, 104, false, L"query failure");
        callbacks.done(999, 105, false, L"late closed session");
        callbacks.done(10, 106, true, L"duplicate");
        uint32_t hr = 0; bool cancelled = false; std::wstring error;
        const bool second = manager.WaitShellDone(11, hr, cancelled, error) &&
            hr == 103 && cancelled && error == L"second";
        const bool first = manager.WaitShellDone(10, hr, cancelled, error) &&
            hr == 101 && !cancelled && error == L"first";
        const bool routed = first && second && delivered == 70 && manager.TakeCtxInvokeDone() &&
            !manager.TakeCtxInvokeDone() && manager.done_results_.empty() &&
            manager.file_op_ids_.empty() && manager.menu_session_by_token_.empty();
        if (!ipc::ShellClientReviewTestAccess::Configure(callbacks)) return false;
        struct ResetClient {
            ~ResetClient() { ipc::audit_submit = {}; ipc::ShellClientReviewTestAccess::Reset(); }
        } reset;
        bool registered_before_done = false;
        ipc::audit_submit = [&](ipc::ShellClient& client, uint32_t type, uint32_t id) {
            registered_before_done = type == ipc::REQ_RENAME && manager.file_op_ids_.contains(id);
            ipc::ShellClientReviewTestAccess::Done(client, id);
        };
        const auto id = ipc::ShellClient::Instance().Rename(L"private-dispatch-fixture", L"new-name",
            [&](uint32_t accepted) { manager.RegisterShellRequest(accepted); });
        return routed && id && registered_before_done && manager.WaitShellDone(id, hr, cancelled, error) &&
            hr == 107 && !cancelled && error == L"accepted before immediate callback" && manager.file_op_ids_.empty();
    }
};
}

namespace {
using namespace pulse::ops;
namespace fs = std::filesystem;
void Write(const fs::path& file, const char* text) { std::ofstream(file) << text; }
std::string Read(const fs::path& file) { std::ifstream in(file); return {std::istreambuf_iterator<char>(in), {}}; }
bool Wait(OpsManager& manager, uint64_t previous) {
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        const auto status = manager.Status();
        if (!status.active && status.completed_ops > previous) return true;
        Sleep(5);
    }
    return false;
}
// An undo may run as one task per item, and the manager is briefly inactive
// between them; settled means inactive for a while, not just once.
bool WaitSettled(OpsManager& manager, uint64_t previous) {
    const auto deadline = GetTickCount64() + 10000;
    ULONGLONG quiet_since = 0;
    while (GetTickCount64() < deadline) {
        const auto status = manager.Status();
        const ULONGLONG now = GetTickCount64();
        if (status.active || status.completed_ops <= previous) quiet_since = 0;
        else if (!quiet_since) quiet_since = now;
        else if (now - quiet_since >= 200) return true;
        Sleep(5);
    }
    return false;
}
OpRequest Request(OpType type, const fs::path& source, const fs::path& target) {
    OpRequest request; request.type = type; request.sources = {source.wstring()}; request.dest_dir = target.wstring(); return request;
}
struct DenyAccess {
    fs::path path;
    PSECURITY_DESCRIPTOR original = nullptr;
    PACL acl = nullptr;
    bool applied = false;
    DenyAccess(const fs::path& value, DWORD mask) : path(value) {
        if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &acl, nullptr, &original) != ERROR_SUCCESS) return;
        BYTE world[SECURITY_MAX_SID_SIZE]{}; DWORD bytes = sizeof(world);
        if (!CreateWellKnownSid(WinWorldSid, nullptr, world, &bytes)) return;
        EXPLICIT_ACCESSW entry{}; entry.grfAccessPermissions = mask;
        entry.grfAccessMode = DENY_ACCESS; entry.grfInheritance = NO_INHERITANCE;
        entry.Trustee.TrusteeForm = TRUSTEE_IS_SID; entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world);
        PACL denied = nullptr;
        if (SetEntriesInAclW(1, &entry, acl, &denied) != ERROR_SUCCESS) return;
        applied = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, denied, nullptr) == ERROR_SUCCESS;
        LocalFree(denied);
    }
    ~DenyAccess() {
        if (applied) SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
        if (original) LocalFree(original);
    }
};
}

int main() {
    using namespace pulse::ops;
    const auto root = fs::absolute(fs::path(L"bench_data") /
        (L"transfer-audit-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    fs::create_directories(root);
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
    };
    check(OpsManagerAuditAccess::RouteInterleavedResults(),
          "file/menu/query interleaving preserves exact task result and consumes each only once");
    {
        const auto source = root / L"terminal-source";
        const auto destination = root / L"terminal-destination";
        fs::create_directories(source / L"tree");
        fs::create_directories(destination / L"tree");
        Write(source / L"tree" / L"new.txt", "merged move");
        Write(destination / L"tree" / L"existing.txt", "keep");
        OpsManager manager; manager.Start([] {}, false);
        const auto success = manager.Submit(Request(OpType::Move, source / L"tree", destination));
        const auto failure = manager.Submit(Request(OpType::Move, source / L"missing.txt", destination));
        std::vector<TaskResult> results;
        const auto deadline = GetTickCount64() + 10000;
        while (results.size() < 2 && GetTickCount64() < deadline) {
            auto batch = manager.DrainTaskResults();
            for (auto& result : batch) results.push_back(std::move(result));
            Sleep(5);
        }
        manager.Stop();
        check(audit_starts.load() == 2, "production transfer startup hook observes both accepted task IDs");
        check(results.size() == 2 && results[0].task_id == success && results[0].success &&
              results[0].moved_sources == std::vector<std::wstring>{(source / L"tree").wstring()} &&
              results[1].task_id == failure && !results[1].success && results[1].moved_sources.empty(),
              "terminal queue retains rapid success/failure IDs and confirmed merged-directory move root");
        const auto completed = manager.DrainCompletions();
        check(!completed.empty() && completed.front().task_id == success &&
              fs::exists(destination / L"tree" / L"new.txt") && fs::exists(destination / L"tree" / L"existing.txt"),
              "precise completions carry origin task ID through merged move");
    }
    {
        // A selection big enough for one folder listing and the indexed
        // commit: every root copies, moves, is reported and comes back.
        const auto source = root / L"many-src";
        const auto copied = root / L"many-copy";
        const auto moved = root / L"many-move";
        fs::create_directories(source); fs::create_directory(copied); fs::create_directory(moved);
        constexpr int kMany = 300;
        std::vector<std::wstring> paths;
        for (int i = 0; i < kMany; ++i) {
            const auto file = source / (L"item-" + std::to_wstring(i) + L".txt");
            Write(file, ("many " + std::to_string(i)).c_str());
            paths.push_back(file.wstring());
        }
        const SourceListing listing(paths);
        bool listed_same = true;
        for (const auto& path : paths) {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            const ListedSource* listed = listing.Find(path);
            listed_same = listed_same && listed &&
                GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
                listed->attributes == data.dwFileAttributes && listed->bytes == data.nFileSizeLow &&
                CompareFileTime(&listed->modified, &data.ftLastWriteTime) == 0;
        }
        std::wstring other_spelling = paths.front();
        other_spelling.replace(other_spelling.rfind(L"item"), 4, L"ITEM");
        check(listed_same && !listing.Find(other_spelling) && !listing.Find(source.wstring()),
              "folder listing matches per-item metadata and leaves other spellings to a query");
        check(!NeedsShellTransfer(paths, moved.wstring(), true),
              "moving many items out of a writable folder needs no elevated transfer");
        OpsManager manager; manager.Start([] {}, false);
        const auto result_of = [&](uint64_t task) {
            const auto deadline = GetTickCount64() + 20000;
            while (GetTickCount64() < deadline) {
                for (auto& result : manager.DrainTaskResults())
                    if (result.task_id == task) return std::optional<TaskResult>(std::move(result));
                Sleep(5);
            }
            return std::optional<TaskResult>();
        };
        auto copy = Request(OpType::Copy, source, copied); copy.sources = paths;
        const auto copied_result = result_of(manager.Submit(copy));
        bool copies_match = copied_result && copied_result->success;
        for (int i = 0; copies_match && i < kMany; ++i) {
            const auto name = L"item-" + std::to_wstring(i) + L".txt";
            copies_match = Read(copied / name) == "many " + std::to_string(i) &&
                fs::last_write_time(copied / name) == fs::last_write_time(source / name);
        }
        check(copies_match, "copy of many items writes every item with its time");
        auto move = Request(OpType::Move, source, moved); move.sources = paths;
        const auto moved_result = result_of(manager.Submit(move));
        check(moved_result && moved_result->success && moved_result->moved_sources == paths &&
              fs::is_empty(source) &&
              std::distance(fs::directory_iterator(moved), fs::directory_iterator{}) == kMany,
              "move of many items reports every root as moved");
        const auto undo_before = manager.Status().completed_ops;
        manager.Undo();
        check(WaitSettled(manager, undo_before) && fs::is_empty(moved) &&
              Read(source / L"item-299.txt") == "many 299",
              "undo of a move of many items brings every item back");
        manager.Stop();
    }
    {
        // A folder past MAX_PATH is still listed once, through an extended
        // path; only a plain absolute folder is extended.
        std::wstring deep = (root / L"long-listing").wstring();
        bool made = CreateDirectoryW((L"\\\\?\\" + deep).c_str(), nullptr) != FALSE;
        std::vector<std::wstring> made_dirs{deep};
        while (made && deep.size() < 300) {
            deep += L"\\" + std::wstring(40, L'd');
            made = CreateDirectoryW((L"\\\\?\\" + deep).c_str(), nullptr) != FALSE;
            made_dirs.push_back(deep);
        }
        std::vector<std::wstring> deep_paths;
        for (int i = 0; made && i < 20; ++i) {
            const std::wstring file = deep + L"\\deep-" + std::to_wstring(i) + L".txt";
            HANDLE handle = CreateFileW((L"\\\\?\\" + file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) { made = false; break; }
            DWORD written = 0;
            made = WriteFile(handle, "deep", 4, &written, nullptr) && written == 4;
            CloseHandle(handle);
            deep_paths.push_back(file);
        }
        const SourceListing deep_listing(deep_paths);
        bool all_listed = made && deep_paths.size() == 20 && (deep + L"\\*").size() >= MAX_PATH;
        for (const auto& path : deep_paths) {
            const ListedSource* listed = deep_listing.Find(path);
            all_listed = all_listed && listed && listed->bytes == 4;
        }
        const TargetListing deep_target(deep, 40);
        check(all_listed && deep_target.SurelyAbsent(L"new.txt") && !deep_target.SurelyAbsent(L"deep-3.txt"),
              "folders past MAX_PATH are listed through an extended path");
        const std::wstring long_name(300, L'y');
        check(ListingPattern(L"C:\\short") == L"C:\\short\\*" && ListingPattern(L"C:\\") == L"C:\\*" &&
              ListingPattern(L"C:\\" + long_name) == L"\\\\?\\C:\\" + long_name + L"\\*" &&
              ListingPattern(L"\\\\srv\\share\\" + long_name) == L"\\\\?\\UNC\\srv\\share\\" + long_name + L"\\*" &&
              ListingPattern(L"C:\\x\\..\\" + long_name).empty() && ListingPattern(L"C:/x/" + long_name).empty() &&
              ListingPattern(L"rel\\" + long_name).empty() && ListingPattern(L"").empty(),
              "only plain absolute long folders are extended");
        for (const auto& path : deep_paths) DeleteFileW((L"\\\\?\\" + path).c_str());
        for (auto dir = made_dirs.rbegin(); dir != made_dirs.rend(); ++dir) RemoveDirectoryW((L"\\\\?\\" + *dir).c_str());
    }
    {
        // A folder of the target's name that appears after the scan found
        // the name free (here while a conflict is pending) is merged into, so
        // undo must take back only what the transfer put there, not the
        // folder with the other program's file. The undo record is read
        // instead of run: undoing a copy recycles.
        const auto source = root / L"late-src";
        const auto target = root / L"late-into";
        fs::create_directories(source / L"dup"); fs::create_directory(target);
        Write(source / L"a.txt", "mine");
        Write(target / L"a.txt", "theirs");
        Write(source / L"dup" / L"x.txt", "copied");
        OpsManager manager; manager.Start([] {}, false);
        auto copy = Request(OpType::Copy, source / L"a.txt", target);
        copy.sources.push_back((source / L"dup").wstring());
        const auto before = manager.Status().completed_ops;
        manager.Submit(copy);
        bool asked = false;
        for (const auto deadline = GetTickCount64() + 10000; !asked && GetTickCount64() < deadline;) {
            if (const auto conflict = manager.PendingConflict()) {
                asked = true;
                fs::create_directory(target / L"dup");
                Write(target / L"dup" / L"other.txt", "not ours");
                manager.ResolveConflict(conflict->token, ConflictChoice::Skip, false);
            } else {
                Sleep(5);
            }
        }
        const bool copied = asked && Wait(manager, before) && Read(target / L"dup" / L"x.txt") == "copied" &&
            Read(target / L"a.txt") == "theirs" && Read(target / L"dup" / L"other.txt") == "not ours";
        const auto quoted = [](const fs::path& path) {
            std::wstring out = L"\"";
            for (const wchar_t c : path.wstring()) { if (c == L'\\') out += L'\\'; out += c; }
            return out + L"\"";
        };
        const std::wstring undo = manager.UndoToJson();
        check(copied && undo.find(quoted(target / L"dup" / L"x.txt")) != std::wstring::npos &&
              undo.find(quoted(target / L"dup")) == std::wstring::npos,
              "a folder that appeared after the scan is undone item by item, not as a whole");
        manager.Stop();
    }
    {
        // One listing of a target folder vouches for names that are surely
        // absent; whatever it cannot vouch for is left to a query.
        const auto target = root / L"target-listing";
        fs::create_directories(target / L"Folder");
        for (const wchar_t* name : {L"Existing.txt", L"照片.jpg", L"\u00c4rger.txt", L"f\u0131le.txt"})
            Write(target / name, "listed");
        const TargetListing few(target.wstring(), 3);
        const TargetListing listing(target.wstring(), 40);
        TargetListing grown(target.wstring(), 40);
        grown.Added(L"arrived.txt");
        const TargetListing missing((target / L"absent").wstring(), 40);
        check(!few.SurelyAbsent(L"new.txt") && listing.SurelyAbsent(L"new.txt") &&
              listing.SurelyAbsent(L"新照片.jpg") && !listing.SurelyAbsent(L"existing.TXT") &&
              !listing.SurelyAbsent(L"folder") && !listing.SurelyAbsent(L"照片.jpg") &&
              !listing.SurelyAbsent(L"\u00e4rger.txt") && !listing.SurelyAbsent(L"NEWFIL~1.TXT") &&
              !listing.SurelyAbsent(L"trailing.") && !listing.SurelyAbsent(L"trailing ") &&
              !grown.SurelyAbsent(L"ARRIVED.txt") && grown.SurelyAbsent(L"other.txt") &&
              !missing.SurelyAbsent(L"new.txt"),
              "target listing vouches only for absent ASCII or caseless names and leaves the rest to a query");
        bool agrees = true;
        for (const wchar_t* name : {L"file.txt", L"FILE.TXT", L"existing.txt", L"EXISTING.TXT", L"folder", L"FOLDER",
                                    L"\u00e4rger.txt", L"\u00c4RGER.TXT", L"照片.jpg", L"new.txt", L"existing"})
            if (listing.SurelyAbsent(name) && GetFileAttributesW((target / name).c_str()) != INVALID_FILE_ATTRIBUTES)
                agrees = false;
        check(agrees, "target listing never calls a name absent that the file system finds");

        // Two selected folders with one name: the first is renamed into the
        // target during the scan, so the second must still find it taken and
        // merge into it. Undo then returns each file to its own folder:
        // neither folder may carry the other's file along, as the second
        // would if taken for a folder of its own, or the first if sent back
        // before the second's file.
        const auto first = root / L"same-name-a";
        const auto second = root / L"same-name-b";
        const auto into = root / L"same-name-into";
        fs::create_directories(first / L"dup"); fs::create_directories(second / L"dup"); fs::create_directory(into);
        Write(first / L"dup" / L"from-first.txt", "first");
        Write(second / L"dup" / L"from-second.txt", "second");
        OpRequest request = Request(OpType::Move, first / L"dup", into);
        for (int i = 0; i < 20; ++i) {
            const auto file = first / (L"fill-" + std::to_wstring(i) + L".txt");
            Write(file, "fill");
            request.sources.push_back(file.wstring());
        }
        request.sources.push_back((second / L"dup").wstring());
        OpsManager manager; manager.Start([] {}, false);
        const auto before = manager.Status().completed_ops;
        manager.Submit(request);
        check(Wait(manager, before) && manager.Status().phase == OpPhase::Completed &&
              Read(into / L"dup" / L"from-first.txt") == "first" &&
              Read(into / L"dup" / L"from-second.txt") == "second" && fs::exists(into / L"fill-19.txt"),
              "a second selected folder with a name renamed in during the scan merges into it");
        const auto undo_before = manager.Status().completed_ops;
        manager.Undo();
        check(WaitSettled(manager, undo_before) && Read(first / L"dup" / L"from-first.txt") == "first" &&
              Read(second / L"dup" / L"from-second.txt") == "second" &&
              !fs::exists(second / L"dup" / L"from-first.txt") && !fs::exists(into / L"dup" / L"from-first.txt") &&
              fs::exists(first / L"fill-19.txt"),
              "undo returns each same-name folder's own file to it");
        manager.Stop();
    }
    for (const std::wstring file : {L"C:\\fixture\\%1 %L %l %V %v %*.txt", L"C:\\Unicode 空格\\", L"C:\\ordinary.txt"}) {
        const std::wstring command = ExpandShellCommand(L"viewer.exe \"%1\" %L \"%*\"", file);
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &count);
        check(arguments && count == 4 && arguments[1] == file && arguments[2] == file && arguments[3] == file,
              "shell template inserts literal filenames once with Windows argument quoting");
        if (arguments) LocalFree(arguments);
    }
    for (bool pause : {false, true}) {
        const auto source = root / (pause ? L"paused.txt" : L"cancelled.txt");
        const auto destination = root / (pause ? L"pause-target" : L"cancel-target");
        Write(source, "request control"); fs::create_directory(destination);
        OpsManager manager;
        manager.Submit(Request(OpType::Move, source, destination));
        if (pause) manager.PauseCurrent(); else manager.CancelCurrent();
        manager.Start([] {}, false);
        if (pause) {
            const auto deadline = GetTickCount64() + 3000;
            while (GetTickCount64() < deadline && manager.Status().phase != OpPhase::Paused) Sleep(5);
            check(manager.Status().phase == OpPhase::Paused && fs::exists(source) && !fs::exists(destination / source.filename()),
                  "pause accepted before worker starts prevents atomic move");
            manager.ResumeCurrent();
        }
        const bool done = Wait(manager, 0);
        check(done && (pause ? fs::exists(destination / source.filename()) && !fs::exists(source)
                            : !fs::exists(destination / source.filename()) && fs::exists(source)),
              "queued cancel/resume survives worker initialization");
        const auto next = root / (pause ? L"next-pause.txt" : L"next-cancel.txt");
        Write(next, "independent");
        const auto completed = manager.Status().completed_ops;
        manager.Submit(Request(OpType::Copy, next, destination));
        check(Wait(manager, completed) && fs::exists(destination / next.filename()), "next request does not inherit prior cancel or pause");
        manager.Stop();
    }
    {
        const auto source = root / L"scan-cancel";
        const auto destination = root / L"scan-cancel-target";
        fs::create_directory(source); fs::create_directory(destination);
        Write(source / L"one.txt", "one"); Write(source / L"two.txt", "two");
        OpsManager manager; std::atomic<bool> cancel_once{true};
        audit_scan = [&](OpsManager& active, const std::wstring& path) {
            if (&active == &manager && path == (source / L"one.txt").wstring() && cancel_once.exchange(false))
                active.CancelCurrent();
        };
        manager.Start([] {}, false);
        auto request = Request(OpType::Move, source / L"one.txt", destination);
        request.sources.push_back((source / L"two.txt").wstring());
        manager.Submit(request);
        check(Wait(manager, 0) && fs::exists(source / L"one.txt") && fs::exists(source / L"two.txt") && fs::is_empty(destination),
              "cancellation at scanning boundary prevents all subsequent root mutations");
        manager.Stop();
        check(!cancel_once.load(), "production transfer scan hook delivered the targeted first source");
        audit_scan = {};
    }
    for (bool nested : {false, true}) {
        const auto source = root / (nested ? L"keep-tree-src" : L"keep-file-src");
        const auto destination = root / (nested ? L"keep-tree-dst" : L"keep-file-dst");
        const auto source_parent = nested ? source / L"tree" : source;
        const auto destination_parent = nested ? destination / L"tree" : destination;
        fs::create_directories(source_parent); fs::create_directories(destination_parent);
        Write(source_parent / L"same.txt", "moving source"); Write(destination_parent / L"same.txt", "original destination");
        OpsManager manager; manager.Start([] {}, false);
        auto request = Request(OpType::Move, nested ? source_parent : source_parent / L"same.txt", destination);
        request.collision_policy = CollisionPolicy::KeepBoth;
        manager.Submit(request);
        check(Wait(manager, 0) && manager.Status().phase == OpPhase::Completed && manager.CanUndo(), "KeepBoth move records actual destination");
        const auto serialized = manager.UndoToJson();
        check(manager.UndoFromJson(serialized), "KeepBoth undo survives persisted JSON round trip");
        const auto before = manager.Status().completed_ops;
        manager.Undo();
        check(Wait(manager, before) && Read(source_parent / L"same.txt") == "moving source" &&
              Read(destination_parent / L"same.txt") == "original destination",
              "KeepBoth undo restores exact original basename without changing target conflict");
        manager.Stop();
    }
    {
        const auto source = root / L"incomplete-src";
        const auto destination = root / L"incomplete-dst";
        fs::create_directories(source / L"tree" / L"denied"); fs::create_directories(destination / L"tree");
        Write(source / L"first.txt", "first completed item");
        Write(source / L"tree" / L"denied" / L"secret.txt", "unread subtree");
        DenyAccess denied(source / L"tree" / L"denied", FILE_LIST_DIRECTORY);
        check(denied.applied, "isolated subtree denies enumeration");
        OpsManager manager; manager.Start([] {}, false);
        manager.Submit(Request(OpType::Copy, source / L"tree", destination));
        check(Wait(manager, 0) && manager.Status().phase == OpPhase::Failed,
              "copy cannot report complete when a subtree cannot be enumerated");
        auto request = Request(OpType::Move, source / L"first.txt", destination);
        request.sources.push_back((source / L"tree").wstring());
        const auto before = manager.Status().completed_ops;
        manager.Submit(request);
        check(Wait(manager, before) && manager.Status().phase == OpPhase::Failed && fs::exists(destination / L"first.txt") && manager.CanUndo(),
              "partial move reports scan failure and retains completed-item undo");
        const auto undo_before = manager.Status().completed_ops;
        manager.Undo();
        check(Wait(manager, undo_before) && Read(source / L"first.txt") == "first completed item",
              "partial move undo restores only confirmed completed item");
        manager.Stop();
    }
    {
        const auto target = root / L"file-only-target";
        const auto source = root / L"permission.txt";
        const auto folder = root / L"permission-folder";
        fs::create_directory(target); fs::create_directory(folder); Write(source, "allowed file");
        DenyAccess denied(target, FILE_ADD_SUBDIRECTORY);
        check(denied.applied && !NeedsShellTransfer({source.wstring()}, target.wstring(), false) &&
              NeedsShellTransfer({folder.wstring()}, target.wstring(), false),
              "file-only destination rights do not demand directory-create elevation");
        OpsManager manager; manager.Start([] {}, false);
        manager.Submit(Request(OpType::Copy, source, target));
        check(Wait(manager, 0) && manager.Status().phase == OpPhase::Completed && Read(target / source.filename()) == "allowed file",
              "file copy succeeds with no directory-create permission or UAC");
#ifdef PULSE_ELEVATED_TEST_CLIENT
        check(ElevatedHelperProcessIdForTesting() == 0, "permitted operations never launched the authorization helper");
        std::atomic<bool> invalid_cancel{false};
        const auto invalid_name = TransferWithElevatedHelper({source.wstring()}, target.wstring(), false,
            nullptr, invalid_cancel, ShellCollisionPolicy::System, {}, L"..\\outside");
        check(invalid_name.hr == E_INVALIDARG && ElevatedHelperProcessIdForTesting() == 0,
              "exact destination rejects path traversal before launching helper");
#endif
        manager.Stop();
    }
#ifdef PULSE_ELEVATED_TEST_CLIENT
    {
        const auto target = root / L"authorized-partial";
        const auto source = root / L"authorized-first.txt";
        const auto folder = root / L"authorized-folder";
        fs::create_directory(target); fs::create_directory(folder);
        Write(source, "confirmed authorized item"); Write(folder / L"child.txt", "directory will be denied");
        DenyAccess denied(target, FILE_ADD_SUBDIRECTORY);
        check(denied.applied, "authorized fixture permits files but rejects directories");
        OpsManager manager; manager.Start([] {}, false);
        auto request = Request(OpType::Copy, source, target);
        request.sources.push_back(folder.wstring());
        manager.Submit(request);
        check(Wait(manager, 0) && manager.Status().phase == OpPhase::Failed &&
              Read(target / source.filename()) == "confirmed authorized item", "authorized batch retains first copy after later directory failure");
        auto completions = manager.DrainCompletions();
        size_t exact = 0, refresh = 0;
        for (const auto& completion : completions) {
            if (completion.refresh_only) ++refresh;
            else if (completion.sources == std::vector<std::wstring>{source.wstring()} &&
                     completion.destinations == std::vector<std::wstring>{(target / source.filename()).wstring()}) ++exact;
        }
        check(exact == 1 && refresh == 1 && !manager.CanUndo(),
              "failed authorized transfer publishes exact completion once plus refresh without unsafe undo");
        const DWORD helper = ElevatedHelperProcessIdForTesting();
        const auto before = manager.Status().completed_ops;
        manager.Submit(request);
        const auto deadline = GetTickCount64() + 5000;
        bool answered = false;
        while (GetTickCount64() < deadline) {
            if (const auto conflict = manager.PendingConflict()) {
                manager.ResolveConflict(conflict->token, ConflictChoice::Cancel, false);
                answered = true;
                break;
            }
            Sleep(5);
        }
        check(answered && Wait(manager, before) && manager.Status().phase == OpPhase::Failed,
              "real OpsManager conflict cancellation completes through authorized client");
        manager.Stop();
        const auto next_target = root / L"authorized-next";
        fs::create_directory(next_target);
        std::atomic<bool> cancel{false};
        const auto invalid_copy = TransferWithElevatedHelper({source.wstring()}, next_target.wstring(), false,
            nullptr, cancel, ShellCollisionPolicy::System, {}, L"restored-original-name.txt");
        check(invalid_copy.hr == E_INVALIDARG && helper == ElevatedHelperProcessIdForTesting() &&
              fs::exists(source) && fs::is_empty(next_target),
              "explicit leaf name is reserved for single-root move and invalid copy preserves helper and files");
        // An explicit leaf is used by Move undo; ordinary Copy keeps the source basename.
        const auto next = TransferWithElevatedHelper({source.wstring()}, next_target.wstring(), true,
            nullptr, cancel, ShellCollisionPolicy::System, {}, L"restored-original-name.txt");
        check(SUCCEEDED(next.hr) && helper != 0 && helper == ElevatedHelperProcessIdForTesting() &&
              !fs::exists(source) && Read(next_target / L"restored-original-name.txt") == "confirmed authorized item",
              "conflict cancel preserves helper session and exact transfer destination name");
        ShutdownElevatedTransferHelper();
    }
#endif
    {
        // On a share several deletes run at once: the selected items, and the
        // files inside one folder. Marked remote-like, the fixture's drive
        // takes that path; what fails still gets its retries and prompts.
        // Last: its protected item starts the authorization helper.
        const std::wstring drive(1, root.wstring()[0]);
        fs::create_directories(root / L"test-data");
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", (root / L"test-data").c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", drive.c_str());
        const auto share = root / L"share-delete";
        fs::create_directories(share / L"tree" / L"sub" / L"deeper");
        OpRequest remove; remove.type = OpType::RealDelete;
        for (int i = 0; i < 30; ++i) {
            const auto file = share / (L"file-" + std::to_wstring(i) + L".txt");
            Write(file, "share");
            remove.sources.push_back(file.wstring());
        }
        SetFileAttributesW((share / L"file-7.txt").c_str(), FILE_ATTRIBUTE_READONLY);
        for (int i = 0; i < 120; ++i) Write(share / L"tree" / (L"t-" + std::to_wstring(i) + L".txt"), "tree");
        for (int i = 0; i < 40; ++i) Write(share / L"tree" / L"sub" / (L"s-" + std::to_wstring(i) + L".txt"), "sub");
        SetFileAttributesW((share / L"tree" / L"sub" / L"s-3.txt").c_str(), FILE_ATTRIBUTE_READONLY);
        const auto held = share / L"held.txt";
        Write(held, "held");
        remove.sources.insert(remove.sources.begin() + 12, held.wstring());
        remove.sources.push_back((share / L"tree").wstring());
        HANDLE holder = CreateFileW(held.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        OpsManager manager; manager.Start([] {});
        const auto result_of = [&](uint64_t task) {
            const auto deadline = GetTickCount64() + 20000;
            while (GetTickCount64() < deadline) {
                for (auto& result : manager.DrainTaskResults())
                    if (result.task_id == task) return std::optional<TaskResult>(std::move(result));
                Sleep(5);
            }
            return std::optional<TaskResult>();
        };
        // What the task reports deleted: its completion, not the request.
        const auto deleted_by = [&](uint64_t task) {
            std::vector<std::wstring> deleted;
            for (const auto& done : manager.DrainCompletions())
                if (done.task_id == task && !done.refresh_only)
                    deleted.insert(deleted.end(), done.sources.begin(), done.sources.end());
            return deleted;
        };
        const auto remove_task = manager.Submit(remove);
        const auto removed = result_of(remove_task);
        const auto removed_items = deleted_by(remove_task);
        size_t left = 0;
        for (const auto& entry : fs::recursive_directory_iterator(share)) { (void)entry; ++left; }
        check(holder != INVALID_HANDLE_VALUE && removed && removed_items.size() == remove.sources.size() - 1 &&
              std::find(removed_items.begin(), removed_items.end(), held.wstring()) == removed_items.end() &&
              left == 1 && fs::exists(held) && manager.Status().failed_items == 1,
              "deleting on a share at once removes every item but the one in use, read-only ones too");
        if (holder != INVALID_HANDLE_VALUE) CloseHandle(holder);

        // A folder selected with a folder inside it (search results): both
        // run at once, and whichever gets there first, all of it goes.
        const auto nest = root / L"share-nest";
        fs::create_directories(nest / L"outer" / L"inner");
        OpRequest nested; nested.type = OpType::RealDelete;
        for (int i = 0; i < 10; ++i) {
            const auto file = nest / (L"n-" + std::to_wstring(i) + L".txt");
            Write(file, "nest");
            nested.sources.push_back(file.wstring());
        }
        for (int i = 0; i < 60; ++i) Write(nest / L"outer" / (L"o-" + std::to_wstring(i) + L".txt"), "outer");
        for (int i = 0; i < 60; ++i) Write(nest / L"outer" / L"inner" / (L"i-" + std::to_wstring(i) + L".txt"), "inner");
        nested.sources.push_back((nest / L"outer").wstring());
        nested.sources.push_back((nest / L"outer" / L"inner").wstring());
        check(result_of(manager.Submit(nested)) && fs::is_empty(nest),
              "a folder and a folder inside it deleted on a share at once leave nothing");

        // Protected item: the permission prompt comes as before; skipping it
        // keeps it, and the items deleted at once around it are reported.
        const auto plain = root / L"share-plain";
        const auto guarded = root / L"share-guarded";
        fs::create_directories(plain); fs::create_directories(guarded);
        OpRequest mixed; mixed.type = OpType::RealDelete;
        for (int i = 0; i < 12; ++i) {
            const auto file = plain / (L"p-" + std::to_wstring(i) + L".txt");
            Write(file, "plain");
            mixed.sources.push_back(file.wstring());
        }
        const auto locked = guarded / L"locked.txt";
        Write(locked, "locked");
        mixed.sources.insert(mixed.sources.begin() + 5, locked.wstring());
        {
            DenyAccess parent(guarded, FILE_DELETE_CHILD);
            DenyAccess item(locked, DELETE);
            const auto task = manager.Submit(mixed);
            bool prompted = false;
            for (const auto deadline = GetTickCount64() + 10000; GetTickCount64() < deadline && !prompted; Sleep(5)) {
                const auto status = manager.Status();
                prompted = status.task_id == task && status.authorization == AuthorizationState::ActionRequired &&
                    status.current_item == locked.wstring();
            }
            if (prompted) manager.ResolveAuthorization(task, false);
            const auto skipped = result_of(task);
            check(parent.applied && item.applied && prompted && skipped && deleted_by(task).size() == 12 &&
                  fs::exists(locked) && fs::is_empty(plain),
                  "a protected item on a share still asks, and skipping it keeps only it");
        }
        manager.Stop();
        ShutdownElevatedTransferHelper();
        SetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", nullptr);
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr);
    }
    std::cout << "Fixture: " << root.string() << '\n';
    return failures ? 1 : 0;
}