// ai_activity_log.h - What AI clients did through the broker, one JSON line
// per call in %LOCALAPPDATA%\Pulse\ai\activity\YYYY-MM.jsonl, kept for
// Settings::log_days. Only a summary is written: time, client, tool, how many
// paths and their parent folders (at most three), the outcome, whether files
// changed and the undo group. Never arguments verbatim (search terms, names),
// never file contents.
#pragma once
#include "../common/json_value.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app::ai {

struct Activity {
    uint64_t time = 0;            // Unix milliseconds
    std::wstring client;
    std::string tool;
    size_t path_count = 0;
    std::vector<std::wstring> folders;   // at most 3, distinct
    std::string outcome;          // "ok" or the error code
    bool changed_files = false;
    uint64_t undo_group = 0;
    uint32_t duration_ms = 0;
};

// Fills path_count and folders from a call's arguments: strings (or arrays
// of strings) under "path", "paths", "folder" and "dest_folder". A path
// contributes its parent folder, a folder itself.
void SummarizeArgs(const json::Value& args, Activity& activity);

// Appends one line to <directory>\YYYY-MM.jsonl (local time of `time`),
// creating the directory. Thread-safe within the process.
bool AppendActivity(const std::wstring& directory, const Activity& activity);

// Deletes month files whose month ended more than `log_days` before `now`
// (Unix ms). Other files are left alone.
void PruneActivity(const std::wstring& directory, int log_days, uint64_t now);

uint64_t UnixMillisecondsNow();

} // namespace pulse::app::ai
