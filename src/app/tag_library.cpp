// tag_library.cpp — see tag_library.h.
#include "tag_library.h"

#include "app_commands.h"
#include "../common/display_path.h"
#include "../common/json_utils.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"

#include <cwchar>

namespace pulse {
namespace {

// File format: {"pulse_tags": 1, "groups": [{"name"}], "tags": [{"id",
// "name", "color": "#RRGGBB", "group": name, "favorite", "paths": [...]}]}
constexpr int kLibraryVersion = 1;

bool SameName(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

void Quoted(const std::wstring& text, std::wstring& out) {
    out += L'"';
    json::Escape(text, out);
    out += L'"';
}

void Toast(AppState& s, const std::wstring& text, const std::wstring& sub = {}) {
    if (s.hwnd) s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, text, sub, {});
}

std::wstring Formatted(l10n::StringId id, int a, int b = 0) {
    wchar_t text[256]{};
    swprintf_s(text, l10n::Get(id).c_str(), a, b);
    return text;
}

std::wstring GroupIdByName(AppState& s, const std::wstring& name) {
    if (name.empty()) return {};
    for (const auto& group : s.places.tag_groups)
        if (SameName(group.name, name)) return group.id;
    return s.places.CreateTagGroup(name);
}

} // namespace

void ExportTagLibrary(AppState& s) {
    std::wstring file;
    if (!PickJsonFile(s.hwnd, true, L"pulse-tags.json", file)) return;
    std::wstring out = L"{\n  \"pulse_tags\": " + std::to_wstring(kLibraryVersion) + L",\n  \"groups\": [";
    for (size_t i = 0; i < s.places.tag_groups.size(); ++i) {
        out += i ? L", {\"name\": " : L"{\"name\": ";
        Quoted(s.places.tag_groups[i].name, out);
        out += L'}';
    }
    out += L"],\n  \"tags\": [";
    for (size_t i = 0; i < s.places.tags.size(); ++i) {
        const app::ColorTag& tag = s.places.tags[i];
        out += i ? L",\n    {\"id\": " : L"\n    {\"id\": ";
        Quoted(tag.id, out);
        out += L", \"name\": ";
        Quoted(tag.name, out);
        wchar_t color[16]{};
        swprintf_s(color, L"#%06X", tag.rgb & 0xFFFFFFu);
        out += L", \"color\": ";
        Quoted(color, out);
        const app::TagGroup* group = s.places.FindTagGroup(s.places.TagGroupOf(tag));
        if (group) {
            out += L", \"group\": ";
            Quoted(group->name, out);
        }
        if (tag.favorite > 0) out += L", \"favorite\": true";
        out += L", \"paths\": [";
        for (size_t p = 0; p < tag.paths.size(); ++p) {
            if (p) out += L", ";
            Quoted(pulse::path::FriendlyPathText(tag.paths[p]), out);
        }
        out += L"]}";
    }
    out += L"\n  ]\n}\n";
    if (!WriteUtf8FileAtomic(file, out)) {
        Toast(s, l10n::Get(l10n::StringId::TagLibraryExportFailed), file);
        return;
    }
    Toast(s, Formatted(l10n::StringId::TagLibraryExported, static_cast<int>(s.places.tags.size())), file);
}

void ImportTagLibrary(AppState& s) {
    std::wstring file;
    if (!PickJsonFile(s.hwnd, false, L"pulse-tags.json", file)) return;
    std::wstring text;
    const std::wstring tags_raw = ReadUtf8File(file, text) ? json::ExtractArray(text, L"tags") : std::wstring{};
    if (json::ExtractInt(text, L"pulse_tags", 0) < 1 || tags_raw.empty()) {
        Toast(s, l10n::Get(l10n::StringId::TagLibraryBadFile), file);
        return;
    }
    // Groups first, in the file's order.
    json::ForEachElement(json::ExtractArray(text, L"groups"), [&](const std::wstring& raw) {
        GroupIdByName(s, json::ExtractString(raw, L"name"));
    });
    int tag_count = 0;
    int item_count = 0;
    int skipped = 0;
    std::vector<app::TagAdsUpdate> ads_updates;
    json::ForEachElement(tags_raw, [&](const std::wstring& raw) {
        const std::wstring id = json::ExtractString(raw, L"id");
        const std::wstring name = json::ExtractString(raw, L"name");
        if (name.empty()) return;
        std::wstring local = s.places.FindTag(id) ? id : std::wstring{};
        for (size_t i = 0; local.empty() && i < s.places.tags.size(); ++i)
            if (SameName(s.places.tags[i].name, name)) local = s.places.tags[i].id;
        const std::wstring group = GroupIdByName(s, json::ExtractString(raw, L"group"));
        if (local.empty()) {
            const std::wstring color = json::ExtractString(raw, L"color");
            const uint32_t rgb = color.size() == 7 && color[0] == L'#'
                ? static_cast<uint32_t>(std::wcstoul(color.c_str() + 1, nullptr, 16)) : 0x8A8A8Au;
            local = s.places.CreateTag(name, rgb);
            if (local.empty()) return;
            if (json::ExtractBool(raw, L"favorite")) s.places.SetTagFavorite(local, true);
        }
        const app::ColorTag* tag = s.places.FindTag(local);
        if (!tag) return;
        if (!group.empty() && s.places.TagGroupOf(*tag).empty()) s.places.SetTagGroup(local, group);
        ++tag_count;
        const int index = s.places.FindTagIndex(local);
        std::vector<std::wstring> paths;
        for (const auto& path : json::ExtractStringArray(raw, L"paths")) {
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                ++skipped;
                continue;
            }
            if (!s.places.PathHasTag(path, index)) paths.push_back(path);
        }
        if (paths.empty()) return;
        s.places.SetTagsBatch(local, paths, true, &ads_updates);
        item_count += static_cast<int>(paths.size());
    });
    QueueTagAds(s, std::move(ads_updates));
    InvalidateRect(s.hwnd, nullptr, FALSE);
    Toast(s, Formatted(l10n::StringId::TagLibraryImported, tag_count, item_count),
          skipped > 0 ? Formatted(l10n::StringId::TagLibrarySkipped, skipped) : std::wstring{});
}
} // namespace pulse
