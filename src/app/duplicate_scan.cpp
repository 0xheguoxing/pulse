#include "duplicate_scan.h"
#include "../ops/ops_manager.h"
#include "../common/path_utils.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace pulse::app {
namespace {

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// A path as deletion compares it: no \\?\ prefix, backslashes only,
// no trailing separator, upper-cased the way CompareStringOrdinal ignores case.
std::wstring DeletedPathKey(const std::wstring& path) {
    std::wstring key = path::StripExtendedPathPrefix(path);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (!key.empty() && key.back() == L'\\') key.pop_back();
    if (key.empty()) return key;
    std::wstring upper(key.size(), L'\0');
    const int length = static_cast<int>(key.size());
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, key.c_str(), length,
                      upper.data(), length, nullptr, nullptr, 0) == length)
        key.swap(upper);
    return key;
}

// Every deleted path at once: a file is covered when it or one of its
// folders is in the set (a case-insensitive ordinal match). Checking
// thousands of files against thousands of deleted paths pair by pair
// stalled the window.
class DeletedPaths {
public:
    explicit DeletedPaths(const std::vector<std::wstring>& paths) {
        keys_.reserve(paths.size());
        for (const auto& path : paths) {
            std::wstring key = DeletedPathKey(path);
            if (!key.empty()) keys_.insert(std::move(key));
        }
    }
    bool Covers(const std::wstring& file) const {
        if (keys_.empty() || file.empty()) return false;
        const std::wstring key = DeletedPathKey(file);
        if (keys_.contains(key)) return true;
        for (size_t i = key.size(); i-- > 1;)
            if (key[i] == L'\\' && keys_.contains(key.substr(0, i))) return true;
        return false;
    }

private:
    std::unordered_set<std::wstring> keys_;
};

} // namespace

uint64_t DuplicateScanSession::DefaultMinimumBytes(DuplicateScanScope scope) noexcept {
    return scope == DuplicateScanScope::Folder ? 1024ull : 1024ull * 1024ull;
}

std::wstring DuplicateScanSession::NormalizeDriveRoot(std::wstring root) {
    if (root.size() >= 2 && root[1] == L':') {
        wchar_t letter = root[0];
        if (letter >= L'a' && letter <= L'z')
            letter = static_cast<wchar_t>(letter - L'a' + L'A');
        root.assign(1, letter);
        root += L":\\";
    }
    return root;
}

std::vector<std::wstring> DuplicateScanSession::ResolveRoots(
    DuplicateScanScope scope, const std::wstring& folder, const std::wstring& drive,
    const std::vector<index::VolumeInfo>& volumes) {
    std::vector<std::wstring> roots;
    if (scope == DuplicateScanScope::Folder) {
        if (!folder.empty()) roots.push_back(folder);
        return roots;
    }
    if (scope == DuplicateScanScope::Drive) {
        const std::wstring root = NormalizeDriveRoot(drive);
        if (!root.empty()) roots.push_back(root);
        return roots;
    }
    for (const auto& volume : volumes) {
        if (volume.kind != index::VolumeKind::Fixed || volume.mount_point.empty()) continue;
        roots.push_back(NormalizeDriveRoot(volume.mount_point));
    }
    return roots;
}

void DuplicateScanSession::ResetResults() {
    ++result_epoch;
    scanning = false;
    completed = false;
    truncated = false;
    error = ERROR_SUCCESS;
    phase = index::ContentSearchPhase::Enumerating;
    scanned_files = 0;
    total_files = 0;
    scanned_bytes = 0;
    current_root.clear();
    files_per_second = 0;
    megabytes_per_second = 0;
    groups.clear();
    pending_groups_.clear();
    group_indices_.clear();
    invalidated_groups_.clear();
    speed_tick_ = {};
    speed_files_ = 0;
    speed_bytes_ = 0;
}

void DuplicateScanSession::UpdateSpeed(const index::ContentSearchProgress& progress) {
    const auto now = std::chrono::steady_clock::now();
    if (speed_tick_.time_since_epoch().count() == 0 || phase != progress.phase) {
        speed_tick_ = now;
        speed_files_ = progress.scanned_files;
        speed_bytes_ = progress.scanned_bytes;
        if (phase != progress.phase) {
            files_per_second = 0;
            megabytes_per_second = 0;
        }
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - speed_tick_).count();
    if (elapsed < 0.35) return;
    const double files = static_cast<double>(progress.scanned_files - speed_files_) / elapsed;
    const double megabytes =
        static_cast<double>(progress.scanned_bytes - speed_bytes_) / elapsed / (1024.0 * 1024.0);
    const double alpha = (std::min)(1.0, elapsed);
    files_per_second = files_per_second * (1.0 - 0.55 * alpha) + files * (0.55 * alpha);
    megabytes_per_second =
        megabytes_per_second * (1.0 - 0.55 * alpha) + megabytes * (0.55 * alpha);
    speed_tick_ = now;
    speed_files_ = progress.scanned_files;
    speed_bytes_ = progress.scanned_bytes;
}

void DuplicateScanSession::RebuildGroupIndex() {
    group_indices_.clear();
    group_indices_.reserve(groups.size());
    for (size_t index = 0; index < groups.size(); ++index)
        group_indices_[groups[index].id] = index;
}

void DuplicateScanSession::SortGroups() {
    std::sort(groups.begin(), groups.end(),
              [](const DuplicateGroup& left, const DuplicateGroup& right) {
                  if (left.size != right.size) return left.size > right.size;
                  return left.files.size() > right.files.size();
              });
    RebuildGroupIndex();
}

void DuplicateScanSession::AppendHits(const std::vector<index::ContentHit>& hits) {
    bool changed = false;
    for (const auto& hit : hits) {
        if (hit.group == 0 || invalidated_groups_.contains(hit.group)) continue;
        DuplicateFile file{hit.path, hit.name.empty() ? hit.path : hit.name,
                           hit.size, hit.modified};
        const auto visible = group_indices_.find(hit.group);
        if (visible == group_indices_.end()) {
            const auto pending = pending_groups_.find(hit.group);
            if (pending == pending_groups_.end()) {
                pending_groups_.emplace(hit.group, std::move(file));
                continue;
            }
            DuplicateGroup group;
            group.id = hit.group;
            group.size = hit.size;
            group.files.push_back(std::move(pending->second));
            group.files.push_back(std::move(file));
            pending_groups_.erase(pending);
            std::sort(group.files.begin(), group.files.end(),
                      [](const DuplicateFile& left, const DuplicateFile& right) {
                          if (left.modified != right.modified) return left.modified > right.modified;
                          return left.path < right.path;
                      });
            group_indices_[group.id] = groups.size();
            groups.push_back(std::move(group));
            changed = true;
            continue;
        }

        DuplicateGroup& group = groups[visible->second];
        const std::wstring kept = group.keep_index < group.files.size()
            ? group.files[group.keep_index].path : std::wstring{};
        group.files.push_back(std::move(file));
        std::sort(group.files.begin(), group.files.end(),
                  [](const DuplicateFile& left, const DuplicateFile& right) {
                      if (left.modified != right.modified) return left.modified > right.modified;
                      return left.path < right.path;
                  });
        group.keep_index = 0;
        for (size_t index = 0; index < group.files.size(); ++index) {
            if (SamePath(group.files[index].path, kept)) {
                group.keep_index = index;
                break;
            }
        }
        changed = true;
    }
    if (changed) ++result_epoch;
}

void DuplicateScanSession::ApplyUpdate(const index::ContentSearchProgress& progress,
                                       const std::vector<index::ContentHit>& hits) {
    if (progress.generation != generation) return;
    UpdateSpeed(progress);
    phase = progress.phase;
    scanned_files = progress.scanned_files;
    total_files = progress.total_files;
    scanned_bytes = progress.scanned_bytes;
    current_root = progress.current_root;
    truncated = progress.truncated;
    error = progress.error;
    if (!hits.empty()) {
        AppendHits(hits);
    }
    if (progress.done) {
        scanning = false;
        completed = true;
        SortGroups();
        ++result_epoch;
    }
}

void DuplicateScanSession::SetKeep(size_t group, size_t file) {
    if (group >= groups.size() || file >= groups[group].files.size()) return;
    if (groups[group].keep_index == file) return;
    groups[group].keep_index = file;
    ++result_epoch;
}

std::vector<std::wstring> DuplicateScanSession::FilesToDelete(size_t group) const {
    std::vector<std::wstring> paths;
    if (group >= groups.size()) return paths;
    const auto& item = groups[group];
    paths.reserve(item.files.size());
    for (size_t i = 0; i < item.files.size(); ++i) {
        if (i == item.keep_index) continue;
        paths.push_back(item.files[i].path);
    }
    return paths;
}

std::vector<std::wstring> DuplicateScanSession::AllFilesToDelete() const {
    std::vector<std::wstring> paths;
    for (size_t i = 0; i < groups.size(); ++i) {
        auto extra = FilesToDelete(i);
        paths.insert(paths.end(), extra.begin(), extra.end());
    }
    return paths;
}

bool DuplicateScanSession::BuildCleanupRequest(ops::OpRequest& request, size_t selected) const {
    request = {};
    if (scanning || (selected != SIZE_MAX && selected >= groups.size())) return false;
    request.type = ops::OpType::RecycleDelete;
    request.duplicate_cleanup = true;
    for (size_t index = 0; index < groups.size(); ++index) {
        if (selected != SIZE_MAX && index != selected) continue;
        const auto& group = groups[index];
        if (group.files.size() < 2 || group.keep_index >= group.files.size()) continue;
        const auto& kept = group.files[group.keep_index];
        ops::DuplicateCleanupGroup cleanup;
        cleanup.keeper = {kept.path, kept.size, kept.modified};
        for (size_t file = 0; file < group.files.size(); ++file) {
            if (file == group.keep_index) continue;
            const auto& extra = group.files[file];
            cleanup.extras.push_back({extra.path, extra.size, extra.modified});
            request.sources.push_back(extra.path);
        }
        request.duplicate_groups.push_back(std::move(cleanup));
    }
    return !request.sources.empty();
}

void DuplicateScanSession::RemoveDeleted(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    ++result_epoch;
    const DeletedPaths deleted(paths);
    for (auto& group : groups) {
        const std::wstring kept = group.keep_index < group.files.size()
            ? group.files[group.keep_index].path : std::wstring{};
        const bool keeper_deleted = deleted.Covers(kept);
        if (keeper_deleted) {
            // Do not silently replace the promised retained copy with an old
            // scan result. Only a new scan can make this group actionable again.
            invalidated_groups_.insert(group.id);
            group.files.clear();
            continue;
        }
        group.files.erase(std::remove_if(group.files.begin(), group.files.end(),
                                         [&](const DuplicateFile& file) {
                                             return deleted.Covers(file.path);
                                         }),
                          group.files.end());
        group.keep_index = 0;
        for (size_t index = 0; index < group.files.size(); ++index) {
            if (SamePath(group.files[index].path, kept)) {
                group.keep_index = index;
                break;
            }
        }
    }
    for (auto it = groups.begin(); it != groups.end();) {
        if (it->files.size() >= 2) {
            ++it;
            continue;
        }
        if (it->files.size() == 1)
            pending_groups_[it->id] = std::move(it->files.front());
        it = groups.erase(it);
    }
    for (auto it = pending_groups_.begin(); it != pending_groups_.end();) {
        if (deleted.Covers(it->second.path)) {
            invalidated_groups_.insert(it->first);
            it = pending_groups_.erase(it);
        } else ++it;
    }
    SortGroups();
}

size_t DuplicateScanSession::ExtraCount() const {
    size_t count = 0;
    for (const auto& group : groups) {
        if (group.files.size() > 1) count += group.files.size() - 1;
    }
    return count;
}

} // namespace pulse::app
