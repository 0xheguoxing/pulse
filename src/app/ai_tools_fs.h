// ai_tools_fs.h - The AI tools' file system side (ai_tools.h registers them).
//
// Functions of a call's JSON arguments with no AppState and no UI, safe on
// any worker thread, so pulse_ai_tools_test runs them on fixtures. Paths from
// AI clients are untrusted: only absolute drive or UNC paths are accepted,
// normalized (".." resolved), without wildcards, streams or device forms.
// Results describe items; they never carry file contents.
#pragma once
#include "ai_broker.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace pulse::app::ai {

// An absolute path argument made usable (no trailing separator except on a
// drive root). False with `error` (invalid_args) when refused.
bool CheckPathArg(const json::Value& value, const char* name, std::wstring& out, ToolOutcome& error);
// `path` with the \\?\ prefix when it is too long for the plain Win32 form.
std::wstring Win32Path(const std::wstring& path);
std::wstring ParentFolder(const std::wstring& path);   // empty for a root
// Windows, Program Files (both), ProgramData, a Recycle Bin or System Volume
// Information, or anything inside them: no AI tool writes there.
bool IsSystemFolder(const std::wstring& path);
// "2026-10-21T03:04:05Z"; empty for a zero time.
std::string IsoTime(const FILETIME& time);
std::string IsoTime(uint64_t filetime);

// list_folder {path, limit?, cursor?}: folders first, then files, each in
// Explorer's name order (numbers by value); hidden items flagged, protected
// system files left out. The cursor is the offset of the next page.
ToolOutcome ListFolder(const json::Value& args, const std::atomic<bool>& cancelled);

// get_properties {paths[]}: per item, the file system's facts plus what the
// shell's property system knows (image size, pages, duration, camera, date
// taken) and a program's version; an item that fails carries "error".
// Initializes COM on the calling thread for the call.
ToolOutcome GetProperties(const json::Value& args, const std::atomic<bool>& cancelled);

// search {query, folder?, type?, modified_after?, limit?} as Pulse's own
// search syntax: type becomes type:/file:/folders_only, modified_after a
// dm: range, folder the path prefix.
struct SearchPlan {
    std::wstring needle;
    std::wstring folder;
    bool folders_only = false;
    size_t limit = 50;
};
bool PlanSearch(const json::Value& args, SearchPlan& plan, ToolOutcome& error);
struct FoundItem {
    std::wstring path;
    std::wstring name;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t modified = 0;   // FILETIME; 0 = unknown
};
json::Value SearchResultJson(const std::vector<FoundItem>& items, size_t total);

// create_zip {paths[], dest_folder?, name?}: items of one folder into a new
// archive that never replaces anything.
struct ZipPlan {
    std::vector<std::wstring> sources;
    std::wstring folder;     // where the archive goes
    std::wstring zip_path;   // free when planned
};
bool PlanZip(const json::Value& args, ZipPlan& plan, ToolOutcome& error);
// The folder a confirmation and an always-allow rule refer to: dest_folder,
// else the first item's folder. Empty when the arguments are unusable.
std::wstring ZipTargetFolder(const json::Value& args);
// Blocking; honours `cancelled` (nothing is left behind then). Data: path,
// size, files. Recording the undo step is the caller's.
ToolOutcome RunZip(const ZipPlan& plan, const std::atomic<bool>& cancelled);

} // namespace pulse::app::ai
