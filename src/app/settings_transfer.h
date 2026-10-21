// settings_transfer.h - Settings > 关于与诊断 > 备份与迁移: one JSON file that
// carries the preferences and the user's own lists to another PC, or back.
//
// Export reads the data dir. Import never replaces live files while Pulse runs:
// the running instance writes app.json, places.json and context_menu.json on
// exit, which would undo it. Import stages its files under
// <data>\settings_import instead, and the next start applies them
// (ApplyPendingImport) before anything is loaded, right after backing the
// current files up under <data>\backups\pre-import-<stamp>.
//
// Machine-specific state never travels: window placement and sessions live in
// other files, and app.json's system integration, picker history and similar
// members (IsMachineLocalPref) are dropped on export and kept on import.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::app::settings_transfer {

enum Category : uint32_t {
    kPrefs = 1u << 0,          // app.json (minus machine-local members) + context_menu.json
    kPlaces = 1u << 1,         // places.json: quick access, starred, workspaces, networks
    kTags = 1u << 2,           // tags.json, tag_rules.json and places.json's tag members
    kAliases = 1u << 3,        // aliases.json
    kSavedSearches = 1u << 4,  // saved_searches.json
};
inline constexpr uint32_t kAllCategories = 0x1F;
inline constexpr int kCategoryCount = 5;
// Display order: settings, places, tags, aliases, saved searches.
Category CategoryAt(int index) noexcept;
// The bundle's name for a category ("prefs", "places", ...).
const wchar_t* CategoryKey(Category category) noexcept;

inline constexpr int kFormatVersion = 1;
inline constexpr wchar_t kFormatName[] = L"pulse-settings";
inline constexpr size_t kBackupsKept = 5;

// app.json members that describe this PC rather than the user's choices.
bool IsMachineLocalPref(std::wstring_view key) noexcept;
// places.json members that belong to kTags (the rest belong to kPlaces).
bool IsTagPlacesKey(std::wstring_view key) noexcept;

// Categories with something to export in `data_dir`.
uint32_t AvailableCategories(const std::wstring& data_dir);

struct ExportInfo {
    std::wstring app_version;
    std::wstring exported_at;  // local time, "2026-10-21T23:40:05"
};
// The bundle text for `categories`; `included` gets the categories written.
bool BuildBundle(const std::wstring& data_dir, uint32_t categories, const ExportInfo& info,
                 std::wstring& out, uint32_t* included = nullptr);

struct Bundle {
    int format_version = 0;
    uint32_t categories = 0;
    std::wstring app_version;
    std::wstring exported_at;
    std::vector<std::pair<std::wstring, std::wstring>> files;  // file name, JSON text
    const std::wstring* File(std::wstring_view name) const;
};
enum class ParseResult { Ok, Invalid, Newer };
ParseResult ParseBundle(const std::wstring& text, Bundle& out);

// Writes <data>\settings_import for the next start (replacing an earlier one).
bool StageImport(const std::wstring& data_dir, const Bundle& bundle, uint32_t categories);
// Stages a backup made by an earlier import, exactly as it was.
bool StageRestore(const std::wstring& data_dir, const std::wstring& backup_dir);
bool HasPendingImport(const std::wstring& data_dir);
void CancelPendingImport(const std::wstring& data_dir);

struct BackupInfo {
    std::wstring dir;
    std::wstring stamp;  // "20261021-234005"
};
// Pre-import backups, newest first.
std::vector<BackupInfo> ListBackups(const std::wstring& data_dir);

enum class ApplyOutcome { Nothing, Imported, Restored, Failed };
struct ApplyResult {
    ApplyOutcome outcome = ApplyOutcome::Nothing;
    int changed = 0;          // files written or removed
    std::wstring backup_dir;  // Imported: where the previous files went
};
// Applies a staged import or restore; `stamp` names the backup it makes.
ApplyResult ApplyPendingImport(const std::wstring& data_dir, std::wstring_view stamp);

// Building blocks, exposed for the tests.
std::wstring MergePrefs(const std::wstring& local, const std::wstring& imported);
std::wstring RestorePrefs(const std::wstring& current, const std::wstring& backup);
std::wstring MergeObjects(const std::wstring& base, const std::wstring& overlay);
// "20261021-234005" or "2026-10-21T23:40:05" -> "2026-10-21 23:40".
std::wstring DisplayTime(std::wstring_view stamp);

} // namespace pulse::app::settings_transfer
