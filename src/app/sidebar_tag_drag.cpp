#include "sidebar_tag_drag.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace pulse {
namespace {

int TagsGroupIndex(const ui::WindowViewModel& vm) {
    for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
        if (vm.sidebar[static_cast<size_t>(g)].id == static_cast<int>(app::SidebarSectionId::Tags))
            return g;
    }
    return -1;
}

int TagIndexForPath(const AppState& s, const std::wstring& path) {
    std::wstring kind, rest;
    if (!app::ParsePulsePath(path, &kind, &rest) || kind != L"tag") return -1;
    return s.places.FindTagIndex(s.places.ResolveTagRef(rest));
}

// Rows a tag can be dropped between: tag rows and group headers (not 所有标签).
bool IsReorderRow(const ui::SidebarItem& item) {
    return item.is_tag || app::IsTagGroupPath(item.path);
}

// Which part of the section a row belongs to: the favorites or a group
// (empty = ungrouped).
struct Block {
    bool favorites = false;
    std::wstring group;
};

Block BlockOfTag(const AppState& s, int index) {
    const app::ColorTag& tag = s.places.tags[static_cast<size_t>(index)];
    switch (s.places.tag_sidebar_mode) {
    case app::TagSidebarMode::FavoritesOnly: return {true, {}};
    case app::TagSidebarMode::AllByGroup: return {false, s.places.TagGroupOf(tag)};
    default: break;
    }
    if (tag.favorite > 0) return {true, {}};
    return {false, s.places.TagGroupOf(tag)};
}

}  // namespace

bool IsSidebarTagPath(const std::wstring& path) {
    return path.starts_with(L"pulse:tag:");
}

bool UpdateSidebarTagDrag(AppState& s, int my) {
    if (!IsSidebarTagPath(s.pinDragPath)) return false;
    const ui::WindowViewModel vm = BuildVm(s, false);
    const float w = static_cast<float>(s.compositor.Width());
    const float h = static_cast<float>(s.compositor.Height());
    // Rows scrolled out of view have no rect; keep each laid-out row's display
    // position so the gap still counts every row.
    std::vector<std::pair<int, D2D1_RECT_F>> rows;
    int row_count = 0;
    const int group = TagsGroupIndex(vm);
    if (group >= 0) {
        const auto& items = vm.sidebar[static_cast<size_t>(group)].items;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (!IsReorderRow(items[static_cast<size_t>(i)])) continue;
            D2D1_RECT_F rc{};
            if (s.renderer.SidebarRowRect(vm, w, h, static_cast<int>(app::SidebarSectionId::Tags), i, &rc))
                rows.emplace_back(row_count, rc);
            ++row_count;
        }
    }
    const float cursor = static_cast<float>(my);
    int insert_at = rows.empty() ? row_count : rows.back().first + 1;
    float line_y = rows.empty() ? 0.0f : rows.back().second.bottom;
    for (const auto& [pos, rc] : rows) {
        if (cursor < (rc.top + rc.bottom) * 0.5f) {
            insert_at = pos;
            line_y = rc.top;
            break;
        }
    }
    s.pinDragToIndex = insert_at;
    s.pinGapLineY = line_y;
    s.pinGapVisible = line_y > 0.0f;
    return true;
}

void CommitSidebarTagDrag(AppState& s, const std::wstring& path, size_t insert_at) {
    const int dragged = TagIndexForPath(s, path);
    if (dragged < 0) return;
    const ui::WindowViewModel vm = BuildVm(s, false);
    struct Row {
        app::TagId tag;      // empty for a group header
        std::wstring group;  // header: the group it opens
    };
    std::vector<Row> rows;
    if (const int group = TagsGroupIndex(vm); group >= 0) {
        for (const auto& item : vm.sidebar[static_cast<size_t>(group)].items) {
            if (!IsReorderRow(item)) continue;
            if (item.is_tag) {
                const int index = TagIndexForPath(s, item.path);
                if (index >= 0) rows.push_back({s.places.tags[static_cast<size_t>(index)].id, {}});
            } else {
                rows.push_back({{}, app::TagGroupIdFromPath(item.path)});
            }
        }
    }
    const app::TagId dragged_id = s.places.tags[static_cast<size_t>(dragged)].id;
    const auto found = std::find_if(rows.begin(), rows.end(),
        [&](const Row& row) { return row.tag == dragged_id; });
    if (found == rows.end()) return;
    const size_t from = static_cast<size_t>(found - rows.begin());
    size_t to = insert_at > from ? insert_at - 1 : insert_at;
    to = (std::min)(to, rows.size() - 1);
    if (to == from) return;
    Row moved = std::move(*found);
    rows.erase(found);
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));

    // The dropped tag joins the block of the row above it (a header opens its
    // group); dropped first, it joins the first block.
    Block target = BlockOfTag(s, dragged);
    const size_t anchor = to > 0 ? to - 1 : (rows.size() > 1 ? 1 : 0);
    if (anchor != to) {
        const Row& row = rows[anchor];
        if (row.tag.empty()) target = {false, row.group};
        else target = BlockOfTag(s, s.places.FindTagIndex(row.tag));
    }
    if (s.places.tag_sidebar_mode == app::TagSidebarMode::FavoritesAndGroups) {
        const bool favorite = s.places.tags[static_cast<size_t>(dragged)].favorite > 0;
        // Nine favorites already: the drop stays where it was.
        if (target.favorites && !favorite && !s.places.SetTagFavorite(dragged_id, true)) return;
        if (!target.favorites && favorite) s.places.SetTagFavorite(dragged_id, false);
    }
    if (!target.favorites) s.places.SetTagGroup(dragged_id, target.group);

    std::vector<app::TagId> favorites;
    std::vector<app::TagId> order;
    for (const auto& row : rows) {
        if (row.tag.empty()) continue;
        order.push_back(row.tag);
        const int index = s.places.FindTagIndex(row.tag);
        if (index >= 0 && BlockOfTag(s, index).favorites) favorites.push_back(row.tag);
    }
    s.places.ReorderFavoriteTags(favorites);
    // Tags the sidebar does not list (folded groups) keep their relative order
    // after the rest.
    const auto& tags = s.places.tags;
    std::vector<bool> placed(tags.size(), false);
    std::vector<app::ColorTag> reordered;
    reordered.reserve(tags.size());
    for (const auto& id : order) {
        const int index = s.places.FindTagIndex(id);
        if (index < 0 || placed[static_cast<size_t>(index)]) continue;
        placed[static_cast<size_t>(index)] = true;
        reordered.push_back(tags[static_cast<size_t>(index)]);
    }
    for (size_t i = 0; i < tags.size(); ++i)
        if (!placed[i]) reordered.push_back(tags[i]);
    s.places.tags = std::move(reordered);
    s.places.TagsReordered();
}

}  // namespace pulse
