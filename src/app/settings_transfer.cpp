#include "../common/windows_compat.h"
#include "settings_transfer.h"

#include "../common/json_utils.h"
#include "../common/utf8_file.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>

namespace pulse::app::settings_transfer {
namespace {

struct FileSpec {
    const wchar_t* name;
    uint32_t categories;
    bool legacy_hex;  // places / tags were written with bare 0xRRGGBB colours
};

// Every file a bundle may carry; nothing outside this list is ever written.
constexpr FileSpec kFiles[] = {
    {L"app.json", kPrefs, false},
    {L"context_menu.json", kPrefs, false},
    {L"places.json", kPlaces | kTags, true},
    {L"tags.json", kTags, true},
    {L"tag_rules.json", kTags, false},
    {L"aliases.json", kAliases, false},
    {L"saved_searches.json", kSavedSearches, false},
};
constexpr std::wstring_view kAppFile = L"app.json";
constexpr std::wstring_view kPlacesFile = L"places.json";
constexpr std::wstring_view kTagsFile = L"tags.json";
constexpr wchar_t kStageDir[] = L"\\settings_import";
constexpr wchar_t kListName[] = L"\\apply.list";
constexpr wchar_t kBackupRoot[] = L"\\backups";
constexpr std::wstring_view kBackupPrefix = L"pre-import-";
constexpr wchar_t kAbsentName[] = L"\\absent.list";
constexpr wchar_t kTagRecovery[] = L"\\tags.pending.json";

const FileSpec* FindSpec(std::wstring_view name) {
    for (const auto& spec : kFiles)
        if (name == spec.name) return &spec;
    return nullptr;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring Trimmed(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool ValidContent(const std::wstring& text, bool legacy_hex) {
    return json::ConfigSyntax(text, legacy_hex).Object() ||
           json::ConfigSyntax(text, legacy_hex).Array();
}

std::wstring Quote(const std::wstring& text) {
    std::wstring escaped;
    json::Escape(text, escaped);
    return L"\"" + escaped + L"\"";
}

// Top-level members as (decoded key, raw value text): values are copied
// verbatim, so numbers, colours and nesting survive untouched.
using Members = std::vector<std::pair<std::wstring, std::wstring>>;

bool ReadMembers(const std::wstring& object, Members& out) {
    out.clear();
    const std::wstring text = Trimmed(object);
    return json::ForEachMember(text, [&](const std::wstring& key, const std::wstring& raw) {
        out.emplace_back(key, raw);
    });
}

std::wstring WriteMembers(const Members& members) {
    std::wstring out = L"{\n";
    for (size_t i = 0; i < members.size(); ++i) {
        out += L"  ";
        out += Quote(members[i].first);
        out += L':';
        out += members[i].second;
        if (i + 1 < members.size()) out += L',';
        out += L'\n';
    }
    out += L"}\n";
    return out;
}

void SetMember(Members& members, const std::wstring& key, const std::wstring& raw) {
    for (auto& member : members) {
        if (member.first == key) {
            member.second = raw;
            return;
        }
    }
    members.emplace_back(key, raw);
}

template <class Keep>
std::wstring FilterObject(const std::wstring& object, Keep keep) {
    Members members;
    if (!ReadMembers(object, members)) return {};
    std::erase_if(members, [&](const auto& member) { return !keep(member.first); });
    return WriteMembers(members);
}

bool KeepPlacesMember(const std::wstring& key, uint32_t categories) {
    if (key == L"recent_items") return false;  // history of this PC
    return IsTagPlacesKey(key) ? (categories & kTags) != 0 : (categories & kPlaces) != 0;
}

std::wstring StagePath(const std::wstring& data_dir) { return data_dir + kStageDir; }

void ClearFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            DeleteFileW((dir + L"\\" + found.cFileName).c_str());
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

void RemoveStage(const std::wstring& data_dir) {
    const std::wstring stage = StagePath(data_dir);
    DeleteFileW((stage + kListName).c_str());  // first: a partial stage never applies
    ClearFiles(stage);
    RemoveDirectoryW(stage.c_str());
}

bool BeginStage(const std::wstring& data_dir, std::wstring& stage) {
    if (data_dir.empty()) return false;
    RemoveStage(data_dir);
    stage = StagePath(data_dir);
    return CreateDirectoryW(stage.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// The list is written last, so only a complete stage is ever applied.
bool CommitStage(const std::wstring& data_dir, const std::wstring& stage,
                 const std::vector<std::wstring>& lines) {
    std::wstring list;
    for (const auto& line : lines) {
        list += line;
        list += L'\n';
    }
    if (WriteUtf8FileAtomic(stage + kListName, list)) return true;
    RemoveStage(data_dir);
    return false;
}

bool StageFile(const std::wstring& stage, const std::wstring& name, const std::wstring& text) {
    return !text.empty() && WriteUtf8FileAtomic(stage + L"\\" + name, text);
}

std::vector<std::wstring> Lines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

uint64_t TagsGeneration(const std::wstring& path) {
    std::wstring text;
    Members members;
    if (!ReadUtf8File(path, text) || !ReadMembers(text, members)) return 0;
    for (const auto& [key, raw] : members) {
        if (key != L"tags_generation") continue;
        std::wstring digits = raw;
        std::erase(digits, L'"');
        return _wcstoui64(digits.c_str(), nullptr, 10);
    }
    return 0;
}

void SetTagsGeneration(const std::wstring& path, uint64_t generation) {
    std::wstring text;
    Members members;
    if (!Exists(path) || !ReadUtf8File(path, text) || !ReadMembers(text, members)) return;
    SetMember(members, L"tags_generation", L"\"" + std::to_wstring(generation) + L"\"");
    const std::wstring out = WriteMembers(members);
    if (ValidContent(out, true)) WriteUtf8FileAtomic(path, out);
}

bool MakeBackup(const std::wstring& data_dir, std::wstring_view stamp, std::wstring& dir) {
    const std::wstring root = data_dir + kBackupRoot;
    if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    const std::wstring base = root + L"\\" + std::wstring(kBackupPrefix) + std::wstring(stamp);
    dir = base;
    for (int n = 2; Exists(dir) && n < 100; ++n) dir = base + L"-" + std::to_wstring(n);
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return false;
    std::wstring absent;
    for (const auto& spec : kFiles) {
        const std::wstring live = data_dir + L"\\" + spec.name;
        if (!Exists(live)) {
            absent += spec.name;
            absent += L'\n';
            continue;
        }
        if (!CopyFileW(live.c_str(), (dir + L"\\" + spec.name).c_str(), FALSE)) return false;
    }
    return WriteUtf8FileAtomic(dir + kAbsentName, absent);
}

void PruneBackups(const std::wstring& data_dir) {
    const auto backups = ListBackups(data_dir);
    for (size_t i = kBackupsKept; i < backups.size(); ++i) {
        ClearFiles(backups[i].dir);
        RemoveDirectoryW(backups[i].dir.c_str());
    }
}

} // namespace

Category CategoryAt(int index) noexcept {
    static constexpr Category kOrder[kCategoryCount] = {kPrefs, kPlaces, kTags, kAliases,
                                                        kSavedSearches};
    return kOrder[std::clamp(index, 0, kCategoryCount - 1)];
}

const wchar_t* CategoryKey(Category category) noexcept {
    switch (category) {
    case kPrefs: return L"prefs";
    case kPlaces: return L"places";
    case kTags: return L"tags";
    case kAliases: return L"aliases";
    case kSavedSearches: return L"saved_searches";
    }
    return L"";
}

bool IsMachineLocalPref(std::wstring_view key) noexcept {
    static constexpr std::wstring_view kLocal[] = {
        // System integration: registry state of this PC (启动与集成).
        L"launch_on_startup", L"open_folders_in_pulse", L"integration_enabled",
        L"integration_folders", L"integration_win_e", L"integration_this_pc",
        L"integration_recycle_bin", L"integration_folders_compat",
        L"take_over_explorer_windows", L"take_over_win_e", L"take_over_this_pc",
        L"take_over_recycle_bin", L"shell_tag_menu",
        // History and one-time state.
        L"picker_folders", L"recent_tags", L"tips_seen", L"last_seen_version",
        L"duplicate_scan_folder", L"duplicate_scan_drive",
    };
    return std::find(std::begin(kLocal), std::end(kLocal), key) != std::end(kLocal);
}

bool IsTagPlacesKey(std::wstring_view key) noexcept {
    return key == L"tags" || key == L"tag_groups" || key == L"tag_sidebar_mode" ||
           key == L"tags_generation" || key == L"tag_version";
}

uint32_t AvailableCategories(const std::wstring& data_dir) {
    uint32_t categories = 0;
    if (data_dir.empty()) return 0;
    for (const auto& spec : kFiles)
        if (Exists(data_dir + L"\\" + spec.name)) categories |= spec.categories;
    return categories;
}

bool BuildBundle(const std::wstring& data_dir, uint32_t categories, const ExportInfo& info,
                 std::wstring& out, uint32_t* included) {
    out.clear();
    if (included) *included = 0;
    categories &= kAllCategories;
    if (data_dir.empty() || !categories) return false;
    uint32_t written = 0;
    std::wstring files;
    for (const auto& spec : kFiles) {
        const uint32_t wanted = spec.categories & categories;
        if (!wanted) continue;
        const std::wstring path = data_dir + L"\\" + spec.name;
        if (!Exists(path)) continue;
        std::wstring text;
        if (!ReadUtf8File(path, text)) return false;
        text = Trimmed(text);
        if (text.empty()) continue;
        if (!ValidContent(text, spec.legacy_hex)) return false;
        if (spec.name == kAppFile) {
            text = FilterObject(text, [](const std::wstring& key) { return !IsMachineLocalPref(key); });
        } else if (spec.name == kPlacesFile) {
            text = FilterObject(text, [&](const std::wstring& key) {
                return KeepPlacesMember(key, categories);
            });
        }
        text = Trimmed(text);
        if (text.empty()) return false;
        if (!files.empty()) files += L",\n";
        files += L"    ";
        files += Quote(spec.name);
        files += L':';
        // Strict JSON goes in as is (readable); legacy colours go in as a string.
        files += ValidContent(text, false) ? text : Quote(text);
        written |= wanted;
    }
    if (!written) return false;
    out = L"{\n  \"format\":" + Quote(kFormatName) +
          L",\n  \"format_version\":" + std::to_wstring(kFormatVersion) +
          L",\n  \"app_version\":" + Quote(info.app_version) +
          L",\n  \"exported_at\":" + Quote(info.exported_at) + L",\n  \"categories\":[";
    bool first = true;
    for (int i = 0; i < kCategoryCount; ++i) {
        const Category category = CategoryAt(i);
        if (!(written & category)) continue;
        if (!first) out += L',';
        out += Quote(CategoryKey(category));
        first = false;
    }
    out += L"],\n  \"files\":{\n";
    out += files;
    out += L"\n  }\n}\n";
    if (included) *included = written;
    return true;
}

const std::wstring* Bundle::File(std::wstring_view name) const {
    for (const auto& file : files)
        if (file.first == name) return &file.second;
    return nullptr;
}

ParseResult ParseBundle(const std::wstring& input, Bundle& out) {
    out = Bundle{};
    const std::wstring text = Trimmed(input);
    if (!json::ConfigSyntax(text, false).Object()) return ParseResult::Invalid;
    if (json::ExtractString(text, L"format") != kFormatName) return ParseResult::Invalid;
    const int version = json::ExtractInt(text, L"format_version", 0);
    if (version < 1) return ParseResult::Invalid;
    if (version > kFormatVersion) return ParseResult::Newer;
    out.format_version = version;
    out.app_version = json::ExtractString(text, L"app_version");
    out.exported_at = json::ExtractString(text, L"exported_at");
    const std::wstring files = json::ExtractObject(text, L"files");
    if (files.empty()) return ParseResult::Invalid;
    bool valid = true;
    uint32_t present = 0;
    const bool walked = json::ForEachMember(files, [&](const std::wstring& name,
                                                       const std::wstring& raw) {
        const FileSpec* spec = FindSpec(name);
        if (!spec || out.File(name)) return;  // a newer Pulse's extra files are skipped
        std::wstring content;
        if (!raw.empty() && raw.front() == L'"') {
            size_t position = 0;
            if (!json::ConfigSyntax(raw, false).DecodeString(position, content)) {
                valid = false;
                return;
            }
            content = Trimmed(content);
        } else {
            content = raw;
        }
        if (!ValidContent(content, spec->legacy_hex)) {
            valid = false;
            return;
        }
        present |= spec->categories;
        out.files.emplace_back(name, std::move(content));
    });
    if (!walked || !valid) return ParseResult::Invalid;
    for (const auto& key : json::ExtractStringArray(text, L"categories")) {
        for (int i = 0; i < kCategoryCount; ++i)
            if (key == CategoryKey(CategoryAt(i))) out.categories |= CategoryAt(i);
    }
    if (!out.categories) out.categories = present;
    return out.categories ? ParseResult::Ok : ParseResult::Invalid;
}

bool StageImport(const std::wstring& data_dir, const Bundle& bundle, uint32_t categories) {
    categories &= bundle.categories & kAllCategories;
    if (!categories) return false;
    std::wstring stage;
    if (!BeginStage(data_dir, stage)) return false;
    std::vector<std::wstring> lines{L"mode import"};
    const auto fail = [&] {
        RemoveStage(data_dir);
        return false;
    };
    for (const auto& spec : kFiles) {
        if (!(spec.categories & categories)) continue;
        const std::wstring name = spec.name;
        const std::wstring* text = bundle.File(name);
        if (name == kAppFile) {
            if (!text) continue;
            if (!StageFile(stage, name, FilterObject(*text, [](const std::wstring& key) {
                    return !IsMachineLocalPref(key);
                }))) return fail();
            lines.push_back(L"merge-prefs " + name);
        } else if (name == kPlacesFile) {
            if (!text) continue;
            if (!StageFile(stage, name, FilterObject(*text, [&](const std::wstring& key) {
                    return KeepPlacesMember(key, categories);
                }))) return fail();
            lines.push_back(L"merge-object " + name);
        } else if (text) {
            if (!StageFile(stage, name, *text)) return fail();
            lines.push_back(L"put " + name);
        } else {
            // The category came along without this file: the source has none.
            lines.push_back(L"delete " + name);
        }
    }
    if (categories & kTags) lines.push_back(L"tags-generation");
    return CommitStage(data_dir, stage, lines);
}

bool StageRestore(const std::wstring& data_dir, const std::wstring& backup_dir) {
    if (backup_dir.empty() || !Exists(backup_dir)) return false;
    std::wstring absent_text;
    ReadUtf8File(backup_dir + kAbsentName, absent_text);
    const std::vector<std::wstring> absent = Lines(absent_text);
    std::wstring stage;
    if (!BeginStage(data_dir, stage)) return false;
    std::vector<std::wstring> lines{L"mode restore"};
    for (const auto& spec : kFiles) {
        const std::wstring name = spec.name;
        const std::wstring saved = backup_dir + L"\\" + name;
        if (Exists(saved)) {
            std::wstring text;
            if (!ReadUtf8File(saved, text) || !StageFile(stage, name, text)) {
                RemoveStage(data_dir);
                return false;
            }
            lines.push_back((name == kAppFile ? L"replace-prefs " : L"put ") + name);
        } else if (std::find(absent.begin(), absent.end(), name) != absent.end()) {
            lines.push_back(L"delete " + name);
        }
    }
    if (lines.size() == 1) {
        RemoveStage(data_dir);
        return false;
    }
    return CommitStage(data_dir, stage, lines);
}

bool HasPendingImport(const std::wstring& data_dir) {
    return !data_dir.empty() && Exists(StagePath(data_dir) + kListName);
}

void CancelPendingImport(const std::wstring& data_dir) {
    if (!data_dir.empty()) RemoveStage(data_dir);
}

std::vector<BackupInfo> ListBackups(const std::wstring& data_dir) {
    std::vector<BackupInfo> backups;
    if (data_dir.empty()) return backups;
    const std::wstring root = data_dir + kBackupRoot;
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((root + L"\\" + std::wstring(kBackupPrefix) + L"*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return backups;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring name = found.cFileName;
        if (name.size() <= kBackupPrefix.size()) continue;
        backups.push_back({root + L"\\" + name, name.substr(kBackupPrefix.size())});
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(backups.begin(), backups.end(),
              [](const BackupInfo& a, const BackupInfo& b) { return a.stamp > b.stamp; });
    return backups;
}

ApplyResult ApplyPendingImport(const std::wstring& data_dir, std::wstring_view stamp) {
    ApplyResult result;
    if (!HasPendingImport(data_dir)) return result;
    const std::wstring stage = StagePath(data_dir);
    std::wstring list;
    if (!ReadUtf8File(stage + kListName, list)) {
        RemoveStage(data_dir);
        result.outcome = ApplyOutcome::Failed;
        return result;
    }
    bool import = false, restore = false, generation = false;
    std::vector<std::pair<std::wstring, std::wstring>> ops;
    for (const auto& line : Lines(list)) {
        const size_t space = line.find(L' ');
        const std::wstring verb = line.substr(0, space);
        const std::wstring argument = space == std::wstring::npos ? L"" : line.substr(space + 1);
        if (verb == L"mode") {
            import = argument == L"import";
            restore = argument == L"restore";
        } else if (verb == L"tags-generation") {
            generation = true;
        } else if (FindSpec(argument) &&
                   (verb == L"merge-prefs" || verb == L"replace-prefs" ||
                    verb == L"merge-object" || verb == L"put" || verb == L"delete")) {
            ops.emplace_back(verb, argument);
        }
    }
    if ((!import && !restore) || ops.empty()) {
        RemoveStage(data_dir);
        result.outcome = ApplyOutcome::Failed;
        return result;
    }
    const auto live_path = [&](std::wstring_view name) { return data_dir + L"\\" + std::wstring(name); };
    const uint64_t generation_before = (std::max)(TagsGeneration(live_path(kPlacesFile)),
                                                  TagsGeneration(live_path(kTagsFile)));
    if (import && !MakeBackup(data_dir, stamp, result.backup_dir)) {
        // Without a way back nothing is replaced.
        RemoveStage(data_dir);
        result.outcome = ApplyOutcome::Failed;
        return result;
    }
    bool failed = false;
    bool tag_files = false;
    for (const auto& [verb, name] : ops) {
        const std::wstring live = live_path(name);
        bool ok = true;
        if (verb == L"delete") {
            ok = DeleteFileW(live.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND ||
                 GetLastError() == ERROR_PATH_NOT_FOUND;
        } else {
            std::wstring incoming;
            std::wstring text;
            if (ReadUtf8File(stage + L"\\" + name, incoming)) {
                std::wstring local;
                if (verb != L"put" && (!ReadUtf8File(live, local) || Trimmed(local).empty()))
                    local = L"{}";
                if (verb == L"merge-prefs") text = MergePrefs(local, incoming);
                else if (verb == L"replace-prefs") text = RestorePrefs(local, incoming);
                else if (verb == L"merge-object") text = MergeObjects(local, incoming);
                else text = incoming;
            }
            ok = !text.empty() && ValidContent(Trimmed(text), FindSpec(name)->legacy_hex) &&
                 WriteUtf8FileAtomic(live, text);
        }
        if (ok) ++result.changed;
        else failed = true;
        if (name == kPlacesFile || name == kTagsFile) tag_files = true;
    }
    if (generation) {
        // Both tag copies at one generation above anything seen, so neither an
        // older places.json copy nor a stale recovery file wins at load.
        const uint64_t next = (std::max)({generation_before, TagsGeneration(live_path(kPlacesFile)),
                                          TagsGeneration(live_path(kTagsFile))}) + 1;
        SetTagsGeneration(live_path(kPlacesFile), next);
        SetTagsGeneration(live_path(kTagsFile), next);
    }
    if (tag_files || generation) DeleteFileW((data_dir + kTagRecovery).c_str());
    RemoveStage(data_dir);
    if (import) PruneBackups(data_dir);
    result.outcome = failed ? ApplyOutcome::Failed
                   : import ? ApplyOutcome::Imported : ApplyOutcome::Restored;
    return result;
}

std::wstring MergePrefs(const std::wstring& local, const std::wstring& imported) {
    Members base;
    if (!ReadMembers(local, base)) base.clear();
    Members incoming;
    if (!ReadMembers(imported, incoming)) return {};
    for (const auto& [key, raw] : incoming)
        if (!IsMachineLocalPref(key)) SetMember(base, key, raw);
    const std::wstring out = WriteMembers(base);
    return json::ValidConfigObject(out) ? out : std::wstring{};
}

std::wstring RestorePrefs(const std::wstring& current, const std::wstring& backup) {
    Members base;
    if (!ReadMembers(backup, base)) return {};
    Members now;
    if (ReadMembers(current, now)) {
        for (const auto& [key, raw] : now)
            if (IsMachineLocalPref(key)) SetMember(base, key, raw);
    }
    const std::wstring out = WriteMembers(base);
    return json::ValidConfigObject(out) ? out : std::wstring{};
}

std::wstring MergeObjects(const std::wstring& base_text, const std::wstring& overlay) {
    Members base;
    if (!ReadMembers(base_text, base)) base.clear();
    Members incoming;
    if (!ReadMembers(overlay, incoming)) return {};
    for (const auto& [key, raw] : incoming) SetMember(base, key, raw);
    return WriteMembers(base);
}

std::wstring DisplayTime(std::wstring_view stamp) {
    if (stamp.size() >= 13 && stamp[8] == L'-') {
        std::wstring out;
        out.append(stamp.substr(0, 4)).append(L"-").append(stamp.substr(4, 2)).append(L"-")
           .append(stamp.substr(6, 2)).append(L" ").append(stamp.substr(9, 2)).append(L":")
           .append(stamp.substr(11, 2));
        return out;
    }
    if (stamp.size() >= 16 && stamp[10] == L'T') {
        std::wstring out(stamp.substr(0, 16));
        out[10] = L' ';
        return out;
    }
    return std::wstring(stamp);
}

} // namespace pulse::app::settings_transfer
