// batch_rename_plan.h — Step-based batch rename engine (preview only, no
// filesystem writes). Pure C++: the caller supplies file metadata and an
// "exists outside the selection" probe, so the engine stays testable.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::app::rename {

enum class StepType { Replace, Remove, Case, Template, Ext, Insert, Number };
enum class Where { Start, End, Index };   // Insert / Number position
enum class CaseMode { Keep, Lower, Upper, Title, Change };

struct Step {
    StepType type = StepType::Replace;
    bool on = true;
    bool ai = false;          // produced by the assistant (shows the AI badge)
    // Replace
    std::wstring find;
    std::wstring with;
    bool regex = false;
    bool match_case = false;
    bool all = true;          // false = first match only
    // Remove
    bool brackets = false;    // " (1)" / "（2）"
    bool spaces = false;      // collapse runs, trim the start
    bool digits = false;
    std::wstring custom;      // literal text to drop
    // Case (stem) and Ext (extension; Change uses ext_to)
    CaseMode mode = CaseMode::Keep;
    std::wstring ext_to;
    // Template
    std::wstring pattern;
    int start = 1;
    int digits_n = 3;
    int stride = 1;
    // Insert (text may use template tokens) and Number (uses start/digits_n/stride)
    std::wstring text;
    Where where = Where::End;
    int at = 1;               // Where::Index: after this many characters
    std::wstring sep = L" ";  // Number: between the name and the number
};

enum class Order { List, Taken, Modified, Name };
enum class Policy { AutoNumber, Skip, Block };

struct Options {
    bool ext_too = false;      // replace/case also touch the extension
    Order order = Order::List;
    Policy policy = Policy::AutoNumber;
};

// One selected file. Times are FILETIME ticks (100 ns since 1601), 0 = unknown.
struct Source {
    std::wstring path;
    std::wstring name;
    std::wstring folder;       // parent folder display name ({文件夹})
    uint64_t modified = 0;
    uint64_t taken = 0;        // EXIF DateTaken; 0 = none
    uint32_t width = 0, height = 0;
};

enum class RowStatus {
    Ok,        // will rename
    Fixed,     // will rename; a trailing space/dot was dropped
    Info,      // will rename; taken date fell back to modified date
    Same,      // no change
    Conflict,  // clash; auto-numbered (Policy::AutoNumber) or left as is
    Skipped,   // clash and Policy::Skip
    Invalid,   // illegal characters / reserved name
    Empty,     // nothing left of the name
};

enum class Why {
    None,
    TrailingFixed,   // Windows forbids names ending in space or dot
    NoTakenDate,     // used the modified date instead
    ClashExisting,   // arg = the existing file's name
    ClashBatch,      // arg = the other selected file's original name
    ClashCycle,      // swap with another selected file
    BadChars,
    EmptyName,
    BadRegex,
};

struct Row {
    size_t src = 0;
    std::wstring new_name;
    std::wstring wanted;     // the name before auto-numbering (clashes only)
    RowStatus status = RowStatus::Ok;
    Why why = Why::None;
    std::wstring why_arg;
    int added_n = 0;         // the (k) appended by AutoNumber
    bool Renames() const {
        return status == RowStatus::Ok || status == RowStatus::Fixed ||
               status == RowStatus::Info ||
               (status == RowStatus::Conflict && added_n > 0);
    }
};

struct Plan {
    std::vector<Row> rows;          // same order as the sources
    std::vector<size_t> exec_order; // row indices to submit, chain-safe
    int renames = 0, same = 0, issues = 0, auto_fixed = 0;
    bool blocked = false;           // Policy::Block with an unresolved clash, or a bad regex
    bool bad_regex = false;
};

// exists(dir_path_of_source, lower_case_name) — true when a file with that
// name exists in the source's folder and is NOT one of the selected files.
using ExistsFn = std::function<bool(const std::wstring& source_path, const std::wstring& lower_name)>;

Plan Run(const std::vector<Source>& sources, const std::vector<Step>& steps,
         const Options& opts, const ExistsFn& exists);
// The same checks (validity, clashes, order) for names chosen another way
// (按内容命名): names[i] is the whole new name; empty keeps the source's.
Plan RunNames(const std::vector<Source>& sources, const std::vector<std::wstring>& names,
              const Options& opts, const ExistsFn& exists);

bool NeedsTakenDate(const std::vector<Step>& steps, const Options& opts);
bool NeedsDimensions(const std::vector<Step>& steps);
bool UsesNumber(const std::wstring& pattern);   // template shows the 序号 row
bool IsValidComponent(const std::wstring& name);

// Token diff (letters | digits | whitespace run | single char), weighted LCS,
// case-insensitive; case-only changes mark just the changed letters.
struct Seg {
    wchar_t kind;  // '=' same, '-' removed (old side), '+' added (new side), '~' plain
    std::wstring text;
};
void Diff(const std::wstring& a, const std::wstring& b,
          std::vector<Seg>& old_side, std::vector<Seg>& new_side);

std::wstring Lower(const std::wstring& s);

} // namespace pulse::app::rename
