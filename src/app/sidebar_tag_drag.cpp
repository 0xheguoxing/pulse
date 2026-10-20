#include "sidebar_tag_drag.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace pulse {
namespace {

int TagsGroupIndex(const ui::WindowViewModel& vm) {
    for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
        const auto& items = vm.sidebar[static_cast<size_t>(g)].items;
        if (!items.empty() && items.front().is_tag) return g;
    }
    return -1;
}

int TagIndexForPath(const AppState& s, const std::wstring& path) {
    std::wstring kind, rest;
    if (!app::ParsePulsePath(path, &kind, &rest) || kind != L"tag") return -1;
    return s.places.FindTagIndex(s.places.ResolveTagRef(rest));
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
    // position so the gap still counts every tag row.
    std::vector<std::pair<int, D2D1_RECT_F>> rows;
    int tag_rows = 0;
    const int group = TagsGroupIndex(vm);
    if (group >= 0) {
        const auto& items = vm.sidebar[static_cast<size_t>(group)].items;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (!items[static_cast<size_t>(i)].is_tag) continue;
            D2D1_RECT_F rc{};
            if (s.renderer.TagItemRect(vm, w, h, group, i, &rc)) rows.emplace_back(tag_rows, rc);
            ++tag_rows;
        }
    }
    const float cursor = static_cast<float>(my);
    int insert_at = rows.empty() ? tag_rows : rows.back().first + 1;
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
    std::vector<int> display;
    if (const int group = TagsGroupIndex(vm); group >= 0) {
        for (const auto& item : vm.sidebar[static_cast<size_t>(group)].items) {
            if (!item.is_tag) continue;
            const int index = TagIndexForPath(s, item.path);
            if (index >= 0) display.push_back(index);
        }
    }
    const auto found = std::find(display.begin(), display.end(), dragged);
    if (found == display.end()) return;
    const size_t from = static_cast<size_t>(found - display.begin());
    size_t to = insert_at > from ? insert_at - 1 : insert_at;
    to = (std::min)(to, display.size() - 1);
    if (to == from) return;
    display.erase(found);
    display.insert(display.begin() + static_cast<std::ptrdiff_t>(to), dragged);
    // Tags the sidebar does not list keep their relative order after the rest.
    const auto& tags = s.places.tags;
    std::vector<bool> placed(tags.size(), false);
    std::vector<app::ColorTag> reordered;
    reordered.reserve(tags.size());
    for (int index : display) {
        if (placed[static_cast<size_t>(index)]) continue;
        placed[static_cast<size_t>(index)] = true;
        reordered.push_back(tags[static_cast<size_t>(index)]);
    }
    for (size_t i = 0; i < tags.size(); ++i)
        if (!placed[i]) reordered.push_back(tags[i]);
    s.places.tags = std::move(reordered);
    s.places.TagsReordered();
}

}  // namespace pulse
