// tag_missing.cpp — see tag_missing.h.
#include "tag_missing.h"

#include "app_commands.h"
#include "tag_view.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>

namespace pulse {
namespace {

struct MissingAnswer {
    bool missing = false;
    ULONGLONG at = 0;
};

std::unordered_map<std::wstring, MissingAnswer>& Cache() {
    static std::unordered_map<std::wstring, MissingAnswer> cache;
    return cache;
}

// "X:\" for a drive path (plain or \\?\), "" for anything else (shares).
std::wstring DriveRoot(const std::wstring& path) {
    std::wstring_view v = path;
    if (v.starts_with(L"\\\\?\\")) v.remove_prefix(4);
    if (v.size() < 2 || v[1] != L':' || !std::iswalpha(v[0])) return {};
    return std::wstring{v[0], L':', L'\\'};
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool CheckMissing(const std::wstring& path, std::unordered_map<std::wstring, bool>& roots) {
    const std::wstring root = DriveRoot(path);
    if (root.empty()) return false;
    auto it = roots.find(root);
    if (it == roots.end()) {
        // Only drives that are here: fixed, or removable with its media in.
        const UINT type = GetDriveTypeW(root.c_str());
        const bool here = (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) && Exists(root);
        it = roots.emplace(root, here).first;
    }
    if (!it->second) return false;
    if (Exists(path)) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool EqualFold(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
               CSTR_EQUAL;
}

// path is base or inside it.
bool Under(const std::wstring& path, const std::wstring& base) {
    if (base.empty() || path.size() < base.size() || !EqualFold(std::wstring_view(path).substr(0, base.size()), base))
        return false;
    return path.size() == base.size() || base.back() == L'\\' || path[base.size()] == L'\\';
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    // Keep the root's backslash (X:\, \\?\X:\).
    if (slash > 0 && path[slash - 1] == L':') return path.substr(0, slash + 1);
    return path.substr(0, slash);
}

std::wstring Join(const std::wstring& folder, std::wstring_view tail) {
    while (!tail.empty() && tail.front() == L'\\') tail.remove_prefix(1);
    std::wstring out = folder;
    if (!out.empty() && out.back() != L'\\') out += L'\\';
    out += tail;
    return out;
}

void Toast(AppState& s, const std::wstring& text) {
    if (s.hwnd) s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, text, std::wstring{}, {});
}

std::wstring Formatted(l10n::StringId id, int count) {
    wchar_t text[256]{};
    swprintf_s(text, l10n::Get(id).c_str(), count);
    return text;
}

} // namespace

std::vector<std::wstring> MissingTaggedPaths(const app::ColorTag& tag) {
    const ULONGLONG now = GetTickCount64();
    auto& cache = Cache();
    std::unordered_map<std::wstring, bool> roots;
    std::vector<std::wstring> missing;
    for (const auto& path : tag.paths) {
        auto it = cache.find(path);
        if (it == cache.end() || now - it->second.at > 30000) {
            it = cache.insert_or_assign(path, MissingAnswer{CheckMissing(path, roots), now}).first;
        }
        if (it->second.missing) missing.push_back(path);
    }
    if (cache.size() > 50000) cache.clear();
    return missing;
}

void AppendMissingTagMenu(const app::ColorTag& tag, std::vector<ui::FluentMenuItem>& items,
                          int clean_command, int relocate_command) {
    const size_t count = MissingTaggedPaths(tag).size();
    if (count == 0) return;
    ui::FluentMenuItem menu;
    menu.text = Formatted(l10n::StringId::TagMissingMenu, static_cast<int>(count));
    menu.glyph = L"\xE7BA";   // warning
    ui::FluentMenuItem relocate;
    relocate.command = relocate_command;
    relocate.text = l10n::Get(l10n::StringId::TagMissingRelocate);
    relocate.glyph = L"\xE8DA";   // open folder
    menu.children.push_back(std::move(relocate));
    ui::FluentMenuItem clean;
    clean.command = clean_command;
    clean.text = l10n::Get(l10n::StringId::TagMissingClean);
    clean.glyph = L"\xE74D";
    menu.children.push_back(std::move(clean));
    menu.separator_after = true;
    items.push_back(std::move(menu));
}

void CleanMissingTagged(AppState& s, const std::vector<std::wstring>& tag_ids) {
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> removals;
    for (const auto& id : tag_ids) {
        const app::ColorTag* tag = s.places.FindTag(id);
        if (!tag) continue;
        auto missing = MissingTaggedPaths(*tag);
        if (!missing.empty()) removals.emplace_back(id, std::move(missing));
    }
    RemoveTagPathsUndoable(s, removals);
}

void RelocateMissingTagged(AppState& s, const std::wstring& tag_id) {
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag) return;
    std::vector<std::wstring> missing = MissingTaggedPaths(*tag);
    if (missing.empty()) return;
    // The deepest folder holding them all; on different drives, the first
    // one's folder (the rest wait for another round).
    std::wstring base = ParentOf(missing.front());
    for (const auto& path : missing)
        while (!base.empty() && !Under(path, base)) {
            const std::wstring up = ParentOf(base);
            base = up == base ? std::wstring{} : up;
        }
    if (base.empty()) {
        base = ParentOf(missing.front());
        std::erase_if(missing, [&](const std::wstring& path) { return !Under(path, base); });
    }
    std::wstring folder;
    if (!PickFolder(s, folder, l10n::Get(l10n::StringId::TagMissingRelocatePick).c_str()) || folder.empty()) return;
    // The folder moved: the same tails under the picked one.
    int found = 0;
    for (const auto& path : missing)
        if (Exists(Join(folder, std::wstring_view(path).substr(base.size())))) ++found;
    if (found > 0) {
        s.places.RemapPaths(base, folder);
    } else {
        // The files themselves are in the picked folder.
        for (const auto& path : missing) {
            const std::wstring candidate = Join(folder, std::wstring_view(path).substr(path.find_last_of(L'\\') + 1));
            if (!Exists(candidate)) continue;
            s.places.RemapPaths(path, candidate);
            ++found;
        }
    }
    Cache().clear();
    InvalidateRect(s.hwnd, nullptr, FALSE);
    Toast(s, found > 0 ? Formatted(l10n::StringId::TagMissingRelocated, found)
                       : l10n::Get(l10n::StringId::TagMissingNotFound));
}
} // namespace pulse
