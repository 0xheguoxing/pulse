// action_files.cpp - The plans and requests of action_files.h.
//
// Requests are the menus' own: a copy or move keeps both on a clash (as the
// assistant's sort does), 合并到文件夹 is a new folder and a move, 解散文件夹
// is app_commands' DissolveFolders, and the Recycle Bin, a permanent delete,
// a restore and emptying the bin are the requests app_ops_ui queues after its
// dialogs. Each plan is read when the step is reached, so the steps before it
// (select.items, nav.go) have already chosen what it works on.
#include "action_files.h"

#include "../app_internal.h"
#include "../ai_cleanup_scan.h"
#include "../app_commands.h"
#include "../app_runtime.h"
#include "../selection_caps.h"
#include "../../common/localization.h"
#include "../../common/path_utils.h"
#include "../../fs/fs_enum.h"
#include "../../ops/clipboard.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <utility>

namespace pulse::app::actions {

namespace {

using I = l10n::StringId;

constexpr size_t kMaxItems = 5000;

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const std::wstring k = key;
    for (size_t at = text.find(k); at != std::wstring::npos; at = text.find(k, at + value.size()))
        text.replace(at, k.size(), value);
    return text;
}

std::wstring Gb1(uint64_t bytes) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// By reference: `why` is often made from the plan's own fields, and a by-value
// plan could be moved from before they are read.
FilePlan Fail(FilePlan&& p, const char* error, std::wstring why) {
    p.ok = false;
    p.error = error;
    p.why = std::move(why);
    return std::move(p);
}

std::wstring Leaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
}

std::wstring Parent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    std::wstring p = path.substr(0, slash);
    if (p.size() == 2 && p[1] == L':') p += L'\\';
    return p;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return dir.back() == L'\\' ? dir + name : dir + L'\\' + name;
}

bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool Under(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size()) return false;
    if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) return false;
    return path.size() == root.size() || path[root.size()] == L'\\' || root.back() == L'\\';
}

bool IsDriveRoot(const std::wstring& path) {
    return (path.size() == 2 || (path.size() == 3 && path[2] == L'\\')) && path[1] == L':';
}

bool IsDir(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring KnownFolder(const KNOWNFOLDERID& id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// Windows, Program Files and ProgramData: the assistant never moves,
// renames or deletes anything in them, nor puts anything there.
std::wstring SystemFolderOf(const std::wstring& path) {
    for (const KNOWNFOLDERID* id : {&FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
                                    &FOLDERID_ProgramData})
        if (const std::wstring root = KnownFolder(*id); Under(path, root)) return root;
    return {};
}

bool ValidLeaf(const std::wstring& name) {
    if (name.empty() || name.size() > 240 || name == L"." || name == L"..") return false;
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    if (name.back() == L' ' || name.back() == L'.') return false;
    for (wchar_t c : name)
        if (c < 32) return false;
    return true;
}

std::wstring Trimmed(std::wstring v) {
    while (!v.empty() && std::iswspace(v.front())) v.erase(0, 1);
    while (!v.empty() && std::iswspace(v.back())) v.pop_back();
    return v;
}

// args.items, else the focused pane's selection.
bool Items(AppState& s, const json::Value& args, std::vector<std::wstring>& out, FilePlan& p) {
    if (const json::Value* items = args.find("items"); items && items->is_array() && items->size()) {
        for (size_t i = 0; i < items->size(); ++i) {
            if (!items->at(i).is_string()) continue;
            std::wstring path = pulse::path::StripExtendedPathPrefix(Trimmed(json::Wide(items->at(i).as_string())));
            for (wchar_t& c : path)
                if (c == L'/') c = L'\\';
            while (path.size() > 3 && path.back() == L'\\') path.pop_back();
            if (path.size() < 3 || !(path[1] == L':' || path.rfind(L"\\\\", 0) == 0)) {
                p = Fail(std::move(p), "items must be absolute paths", Fill(Text(I::AiFoGone), L"{name}", path));
                return false;
            }
            if (!Exists(path)) {
                p = Fail(std::move(p), "an item does not exist", Fill(Text(I::AiFoGone), L"{name}", Leaf(path)));
                return false;
            }
            out.push_back(std::move(path));
        }
    } else {
        app::Tab* tab = ActiveTab(s);
        if (!tab || !tab->snapshot || IsRecycleTab(tab) || tab->SelectedCount() == 0) {
            p = Fail(std::move(p), "nothing is selected: select the items first (select.items) or give items",
                     Text(I::AiFoNoItems));
            return false;
        }
        const app::SelectionCaps caps = app::ComputeSelectionCaps(*tab, &s.places);
        if (!caps.copy) {
            p = Fail(std::move(p), "the selection is not files or folders", Text(I::AiFoNoItems));
            return false;
        }
        out = SelectedFullPaths(*tab);
        // A tab opened on \\?\C:\… keeps the prefix; plans, drives and
        // cards work on the plain path.
        for (std::wstring& x : out) x = pulse::path::StripExtendedPathPrefix(x);
    }
    if (out.empty()) {
        p = Fail(std::move(p), "nothing to work on", Text(I::AiFoNoItems));
        return false;
    }
    if (out.size() > kMaxItems) {
        p = Fail(std::move(p), "too many items for one step (5000 at most)",
                 Fill(Text(I::AiFoTooMany), L"{n}", std::to_wstring(kMaxItems)));
        return false;
    }
    for (const std::wstring& path : out) {
        if (IsDriveRoot(path) || app::IsUncServerRoot(path)) {
            p = Fail(std::move(p), "a whole drive cannot be moved, renamed or deleted", Text(I::AiFoDriveRoot));
            return false;
        }
        if (const std::wstring root = SystemFolderOf(path); !root.empty()) {
            p = Fail(std::move(p), "the item is inside a system or program folder; Pulse never touches those",
                     Fill(Text(I::AiFoSystem), L"{folder}", root));
            return false;
        }
    }
    return true;
}

void AddRows(FilePlan& p, const std::wstring& to) {
    for (const std::wstring& path : p.sources) {
        FileRow r;
        r.name = Leaf(path);
        r.to = to;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
            r.folder = (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!r.folder) r.bytes = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        }
        if (r.folder) p.files_only = false;
        p.bytes += r.bytes;
        p.rows.push_back(std::move(r));
    }
    p.count = p.rows.size();
}

// A real folder the user could open, not a search or This PC.
bool RealFolder(const std::wstring& path) {
    return !path.empty() && !fs::IsVirtualPath(path) && !app::IsUncServerRoot(path);
}

FilePlan PlanTransfer(AppState& s, FilePlan p, const json::Value& args, DestResolver resolve) {
    if (p.paste) {
        app::Tab* tab = ActiveTab(s);
        if (!tab || !RealFolder(tab->current_path) || tab->net_readonly)
            return Fail(std::move(p), "the focused pane is not a folder to paste into", Text(I::AiFoNoDest));
        ops::ClipboardData cb;
        if (!ops::ReadClipboard(cb) || cb.paths.empty())
            return Fail(std::move(p), "the clipboard holds no files", Text(I::AiFoClipEmpty));
        p.op = cb.cut ? FileOp::Move : FileOp::Copy;
        for (const std::wstring& path : cb.paths) p.sources.push_back(fs::NormalizePath(path));
        if (p.op == FileOp::Move)
            for (const std::wstring& path : p.sources)
                if (const std::wstring root = SystemFolderOf(path); !root.empty())
                    return Fail(std::move(p), "a cut item is inside a system or program folder",
                                Fill(Text(I::AiFoSystem), L"{folder}", root));
        p.dest = pulse::path::StripExtendedPathPrefix(tab->current_path);
    } else {
        std::vector<std::wstring> items;
        if (!Items(s, args, items, p)) return p;
        p.sources = std::move(items);
        const std::string dest = args.get_string("dest", args.get_string("path"));
        if (!dest.empty()) {
            p.dest = resolve ? resolve(json::Wide(dest)) : json::Wide(dest);
            if (!RealFolder(p.dest))
                return Fail(std::move(p), "dest must be an absolute folder or start with a known folder (Documents)",
                            Fill(Text(I::AiFoBadDest), L"{dest}", json::Wide(dest)));
        } else {
            // As Ctrl+Alt+C / Ctrl+Alt+X: the pane marked as the destination.
            app::Tab* dst = s.targetPane && s.targetPane != s.pane ? s.targetPane->ActiveTab() : nullptr;
            if (!dst || !RealFolder(dst->current_path) || dst->net_readonly)
                return Fail(std::move(p), "no destination: give dest (a folder)", Text(I::AiFoNoDest));
            p.dest = pulse::path::StripExtendedPathPrefix(dst->current_path);
        }
    }
    if (const std::wstring root = SystemFolderOf(p.dest); !root.empty())
        return Fail(std::move(p), "dest is inside a system or program folder", Fill(Text(I::AiFoSystem), L"{folder}", root));
    if (Exists(p.dest) && !IsDir(p.dest))
        return Fail(std::move(p), "dest is a file, not a folder", Fill(Text(I::AiFoBadDest), L"{dest}", p.dest));
    p.dest_exists = IsDir(p.dest);
    for (const std::wstring& src : p.sources)
        if (Under(p.dest, src))
            return Fail(std::move(p), "a folder cannot go into itself", Text(I::AiFoIntoItself));
    // A move into the folder an item is in already does nothing for it.
    if (p.op == FileOp::Move) {
        std::erase_if(p.sources, [&](const std::wstring& src) { return Same(Parent(src), p.dest); });
        if (p.sources.empty()) {
            p.ok = true;
            p.skip = true;
            p.why = Fill(Text(I::AiFoSameFolder), L"{folder}", Leaf(p.dest));
            return p;
        }
    }
    AddRows(p, p.dest);
    if (p.dest_exists)
        for (const FileRow& r : p.rows)
            if (Exists(Join(p.dest, r.name))) ++p.clashes;
    p.ok = true;
    return p;
}

FilePlan PlanRename(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    json::Value one = json::Value::Object();
    if (const json::Value* it = args.find("item"); it && it->is_string()) {
        json::Value list = json::Value::Array();
        list.push(*it);
        one.set("items", std::move(list));
    }
    if (!Items(s, one.find("items") ? one : args, items, p)) return p;
    if (items.size() != 1) return Fail(std::move(p), "file.rename renames one item; use rename_files for several", Text(I::AiFoOneItem));
    p.new_name = Trimmed(json::Wide(args.get_string("name", args.get_string("new_name"))));
    if (!ValidLeaf(p.new_name))
        return Fail(std::move(p), "name must be a plain file name", Fill(Text(I::AiFoBadName), L"{name}", p.new_name));
    p.sources = std::move(items);
    const std::wstring old = Leaf(p.sources[0]);
    if (old == p.new_name) {
        p.ok = true;
        p.skip = true;
        return p;
    }
    const std::wstring target = Join(Parent(p.sources[0]), p.new_name);
    if (!Same(old, p.new_name) && Exists(target))
        return Fail(std::move(p), "an item of that name is already there", Fill(Text(I::AiFoNameTaken), L"{name}", p.new_name));
    AddRows(p, p.new_name);
    p.dest = Parent(p.sources[0]);
    p.ok = true;
    return p;
}

FilePlan PlanGroup(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    p.new_name = Trimmed(json::Wide(args.get_string("name")));
    if (!ValidLeaf(p.new_name))
        return Fail(std::move(p), "give name: the new folder's name", Fill(Text(I::AiFoBadName), L"{name}", p.new_name));
    p.dest = Parent(p.sources[0]);
    for (const std::wstring& src : p.sources)
        if (!Same(Parent(src), p.dest))
            return Fail(std::move(p), "the items must be in one folder", Text(I::AiFoOneFolder));
    if (Exists(Join(p.dest, p.new_name)))
        return Fail(std::move(p), "an item of that name is already there", Fill(Text(I::AiFoNameTaken), L"{name}", p.new_name));
    p.dest_exists = false;
    AddRows(p, p.new_name);
    p.ok = true;
    return p;
}

FilePlan PlanDissolve(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    p.dest = Parent(p.sources[0]);
    for (const std::wstring& src : p.sources) {
        if (!IsDir(src)) return Fail(std::move(p), "only folders can be dissolved", Text(I::AiFoNotFolders));
        if (!Same(Parent(src), p.dest))
            return Fail(std::move(p), "the folders must be in one folder", Text(I::AiFoOneFolder));
    }
    AddRows(p, p.dest);
    for (FileRow& r : p.rows) {
        WIN32_FIND_DATAW fd{};
        size_t n = 0;
        HANDLE h = FindFirstFileExW(Join(Join(p.dest, r.name), L"*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (std::wcscmp(fd.cFileName, L".") != 0 && std::wcscmp(fd.cFileName, L"..") != 0) ++n;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        r.bytes = n;   // a folder's row says how many items move up
        p.inner += n;
    }
    p.ok = true;
    return p;
}

FilePlan PlanRecycle(AppState& s, FilePlan p, const json::Value& args) {
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    AddRows(p, {});
    // Each drive's bin must take its share (the second batch's rule): one
    // that has none, or has no room, deletes for good, so nothing is done.
    std::vector<std::pair<wchar_t, uint64_t>> drives;
    for (size_t i = 0; i < p.sources.size(); ++i) {
        const std::wstring& path = p.sources[i];
        const wchar_t d = path.size() >= 2 && path[1] == L':' ? static_cast<wchar_t>(std::towupper(path[0])) : 0;
        auto it = std::find_if(drives.begin(), drives.end(), [&](const auto& x) { return x.first == d; });
        if (it == drives.end()) drives.push_back({d, p.rows[i].bytes});
        else it->second += p.rows[i].bytes;
    }
    for (const auto& [drive, bytes] : drives) {
        const app::ai::BinState bin = drive ? app::ai::ReadBin(drive) : app::ai::BinState{};
        if (!bin.available) {
            p = Fail(std::move(p), "this place has no Recycle Bin, so deleting would be permanent; the user deletes it",
                     Text(I::AiFoBinOff));
            p.error += drive ? std::string(" (drive ") + static_cast<char>(drive) + ":)" : std::string(" (not on a drive)");
            return p;
        }
        p.bin_max = bin.max_bytes;
        p.bin_used = bin.used_bytes;
        if (bin.used_bytes + bytes > bin.max_bytes)
            return Fail(std::move(p), "the Recycle Bin is too small for these files",
                        Fill(Fill(Text(I::AiClBinFull), L"{max}", Gb1(bin.max_bytes)), L"{used}", Gb1(bin.used_bytes)));
    }
    p.ok = true;
    return p;
}

FilePlan PlanPurge(AppState& s, FilePlan p, const json::Value& args) {
    if (const app::Tab* tab = ActiveTab(s); tab && IsRecycleTab(tab) && !args.find("items"))
        return Fail(std::move(p), "items in the Recycle Bin: use recycle.empty, or the user deletes them", Text(I::AiFoInBin));
    std::vector<std::wstring> items;
    if (!Items(s, args, items, p)) return p;
    p.sources = std::move(items);
    AddRows(p, {});
    p.ok = true;
    return p;
}

FilePlan PlanRestore(AppState& s, FilePlan p) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || !IsRecycleTab(tab) || tab->SelectedCount() == 0)
        return Fail(std::move(p), "open the Recycle Bin (nav.go pulse:recycle) and select the items first", Text(I::AiFoNotBin));
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const fs::DirEntry entry = tab->EntryAt(static_cast<size_t>(index));
        if (entry.recycle_path.empty()) continue;
        const std::wstring original = entry.full_path;
        if (const std::wstring root = SystemFolderOf(original); !root.empty())
            return Fail(std::move(p), "the item came from a system or program folder", Fill(Text(I::AiFoSystem), L"{folder}", root));
        FileRow r;
        r.name = entry.name;
        r.to = Parent(original);
        r.folder = entry.is_dir;
        r.bytes = entry.is_dir ? 0 : entry.size;
        // Something is at its place again: the restore would fail on it, so
        // it stays in the bin and the card says so.
        if (!original.empty() && Exists(original)) {
            ++p.clashes;
            r.skip = true;
        } else {
            p.sources.push_back(entry.recycle_path);
            if (r.folder) p.files_only = false;
            p.bytes += r.bytes;
        }
        p.rows.push_back(std::move(r));
    }
    p.count = p.rows.size();
    if (!p.count) return Fail(std::move(p), "nothing in the Recycle Bin is selected", Text(I::AiFoNotBin));
    if (p.sources.empty())
        return Fail(std::move(p), "every item's original place is taken by another item", Text(I::AiFoRestoreTaken));
    p.ok = true;
    return p;
}

FilePlan PlanEmptyBin(FilePlan p) {
    SHQUERYRBINFO info{};
    info.cbSize = sizeof(info);
    if (FAILED(SHQueryRecycleBinW(nullptr, &info)))
        return Fail(std::move(p), "Windows did not say what is in the Recycle Bin", Text(I::AiActWhyFailed));
    p.count = static_cast<size_t>(info.i64NumItems);
    p.bytes = static_cast<uint64_t>(info.i64Size);
    p.ok = true;
    if (p.count == 0) {
        p.skip = true;
        p.why = Text(I::AiFoBinEmpty);
    }
    return p;
}

} // namespace

FileOp FileOpOf(std::string_view id) noexcept {
    if (id == "file.copy_to") return FileOp::Copy;
    if (id == "file.move_to" || id == "file.paste") return FileOp::Move;
    if (id == "file.rename") return FileOp::Rename;
    if (id == "file.group_into_folder") return FileOp::Group;
    if (id == "file.dissolve") return FileOp::Dissolve;
    if (id == "file.recycle") return FileOp::Recycle;
    if (id == "file.delete_permanent") return FileOp::Purge;
    if (id == "recycle.restore") return FileOp::Restore;
    if (id == "recycle.empty") return FileOp::EmptyBin;
    return FileOp::None;
}

bool FileOpUndoes(FileOp op) noexcept {
    switch (op) {
    case FileOp::Copy:
    case FileOp::Move:
    case FileOp::Rename:
    case FileOp::Group:
    case FileOp::Dissolve:
    case FileOp::Recycle: return true;
    default: return false;
    }
}

FilePlan PlanFileOp(AppState& s, std::string_view id, const json::Value& args, DestResolver resolve) {
    FilePlan p;
    p.op = FileOpOf(id);
    p.paste = id == "file.paste";
    switch (p.op) {
    case FileOp::Copy:
    case FileOp::Move: return PlanTransfer(s, std::move(p), args, resolve);
    case FileOp::Rename: return PlanRename(s, std::move(p), args);
    case FileOp::Group: return PlanGroup(s, std::move(p), args);
    case FileOp::Dissolve: return PlanDissolve(s, std::move(p), args);
    case FileOp::Recycle: return PlanRecycle(s, std::move(p), args);
    case FileOp::Purge: return PlanPurge(s, std::move(p), args);
    case FileOp::Restore: return PlanRestore(s, std::move(p));
    case FileOp::EmptyBin: return PlanEmptyBin(std::move(p));
    case FileOp::None: break;
    }
    return Fail(std::move(p), "not a file action", Text(I::AiActWhyUnknown));
}

std::vector<uint64_t> SubmitFilePlan(AppState& s, const FilePlan& plan, uint64_t undo_group,
                                     const std::wstring& undo_label) {
    std::vector<uint64_t> tasks;
    if (!plan.ok || plan.skip) return tasks;
    const auto request = [&](ops::OpType type) {
        ops::OpRequest r;
        r.type = type;
        r.undo_group = undo_group;
        r.undo_label = undo_label;
        return r;
    };
    switch (plan.op) {
    case FileOp::Copy:
    case FileOp::Move: {
        if (!plan.dest_exists) {
            ops::OpRequest make = request(ops::OpType::CreateFolder);
            make.sources.push_back(plan.dest);
            tasks.push_back(s.ops.Submit(std::move(make)));
        }
        ops::OpRequest r = request(plan.op == FileOp::Move ? ops::OpType::Move : ops::OpType::Copy);
        r.sources = plan.sources;
        r.dest_dir = plan.dest;
        r.collision_policy = ops::CollisionPolicy::KeepBoth;   // nothing is overwritten
        if (plan.op == FileOp::Move) s.cutPaths.clear();
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Rename: {
        ops::OpRequest r = request(ops::OpType::Rename);
        r.sources = plan.sources;
        r.new_name = plan.new_name;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Group: {
        // 合并到文件夹 with the name given: the folder, then the move into it.
        const std::wstring folder = Join(plan.dest, plan.new_name);
        ops::OpRequest make = request(ops::OpType::CreateFolder);
        make.sources.push_back(folder);
        tasks.push_back(s.ops.Submit(std::move(make)));
        ops::OpRequest r = request(ops::OpType::Move);
        r.sources = plan.sources;
        r.dest_dir = folder;
        r.collision_policy = ops::CollisionPolicy::KeepBoth;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Dissolve: {
        app::Tab* tab = ActiveTab(s);
        if (tab) tasks = DissolveFolders(s, *tab, plan.sources, undo_group, undo_label);
        break;
    }
    case FileOp::Recycle: {
        ops::OpRequest r = request(ops::OpType::RecycleDelete);
        r.sources = plan.sources;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Purge: {
        ops::OpRequest r = request(ops::OpType::RealDelete);
        r.undo_group = 0;
        r.sources = plan.sources;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::Restore: {
        ops::OpRequest r = request(ops::OpType::RestoreRecycle);
        r.undo_group = 0;
        r.sources = plan.sources;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::EmptyBin: {
        ops::OpRequest r = request(ops::OpType::EmptyRecycle);
        r.undo_group = 0;
        tasks.push_back(s.ops.Submit(std::move(r)));
        break;
    }
    case FileOp::None: break;
    }
    return tasks;
}

} // namespace pulse::app::actions
