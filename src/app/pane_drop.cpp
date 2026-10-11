// pane_drop.cpp - Multi-window M6d: content-area drop zones and adding a
// dropped tab's pane to a layout.
#include "pane_drop.h"

#include "pane_layout.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse::app {
namespace {
bool IsEdge(ContentDrop edge) {
    return edge == ContentDrop::Left || edge == ContentDrop::Top || edge == ContentDrop::Right ||
           edge == ContentDrop::Bottom;
}

bool Leading(ContentDrop edge) { return edge == ContentDrop::Left || edge == ContentDrop::Top; }

// The visible panes of `tab` with `added` in front or at the back.
std::vector<Pane*> GrownOrder(const LayoutTab& tab, Pane* added, ContentDrop edge) {
    std::vector<Pane*> order = tab.VisiblePanes();
    order.insert(Leading(edge) ? order.begin() : order.end(), added);
    return order;
}
}  // namespace

ContentDrop ContentDropZone(const D2D1_RECT_F& content, POINT pt, float band) {
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    const float w = content.right - content.left, h = content.bottom - content.top;
    if (w <= 0.0f || h <= 0.0f || x < content.left || x >= content.right || y < content.top || y >= content.bottom)
        return ContentDrop::None;
    // Distance to each edge as a share of the band, so corners go to the
    // edge the cursor is relatively closer to.
    const float left = (x - content.left) / (w * band), right = (content.right - x) / (w * band);
    const float top = (y - content.top) / (h * band), bottom = (content.bottom - y) / (h * band);
    const float nearest = (std::min)({left, right, top, bottom});
    if (nearest >= 1.0f) return ContentDrop::Center;
    if (nearest == left) return ContentDrop::Left;
    if (nearest == right) return ContentDrop::Right;
    return nearest == top ? ContentDrop::Top : ContentDrop::Bottom;
}

LayoutPreset GrownPreset(size_t visible, ContentDrop edge) {
    if (!IsEdge(edge)) return LayoutPreset::Single;
    switch (visible) {
    case 1: return edge == ContentDrop::Left || edge == ContentDrop::Right ? LayoutPreset::TwoVertical
                                                                          : LayoutPreset::TwoHorizontal;
    case 2: return LayoutPreset::Three;
    case 3: return LayoutPreset::FourGrid;
    default: return LayoutPreset::Single;
    }
}

bool CanAddPane(const LayoutTab& tab, const LayoutTab& incoming, ContentDrop edge) {
    return tab.root && incoming.panes.size() == 1 && incoming.panes[0] &&
           GrownPreset(tab.VisiblePanes().size(), edge) != LayoutPreset::Single;
}

D2D1_RECT_F PaneDropRect(const LayoutTab& tab, ContentDrop edge, const D2D1_RECT_F& content, float scale) {
    const LayoutPreset grown = GrownPreset(tab.VisiblePanes().size(), edge);
    if (grown == LayoutPreset::Single) return content;
    Pane placeholder;
    const auto tree = MakePresetTree(grown, GrownOrder(tab, &placeholder, edge));
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    LayoutWindowPanes(*tree, content, scale, laid);
    for (const auto& [pane, rect] : laid)
        if (pane == &placeholder) return rect;
    return content;
}

bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, ContentDrop edge) {
    if (!incoming || !CanAddPane(tab, *incoming, edge)) return false;
    const LayoutPreset grown = GrownPreset(tab.VisiblePanes().size(), edge);
    std::unique_ptr<Pane> moved = std::move(incoming->panes[0]);
    incoming->panes.clear();
    incoming->root.reset();
    Pane* const added = moved.get();
    const std::vector<Pane*> order = GrownOrder(tab, added, edge);
    const Pane* const target = tab.target_index >= 0 && static_cast<size_t>(tab.target_index) < tab.panes.size()
                                   ? tab.panes[static_cast<size_t>(tab.target_index)].get()
                                   : nullptr;

    std::vector<std::unique_ptr<Pane>> panes;
    panes.reserve(tab.panes.size() + 1);
    for (Pane* pane : order) {
        if (pane == added) {
            panes.push_back(std::move(moved));
            continue;
        }
        for (auto& owned : tab.panes)
            if (owned.get() == pane) panes.push_back(std::move(owned));
    }
    for (auto& owned : tab.panes)
        if (owned) panes.push_back(std::move(owned));  // hidden panes keep their order after the visible ones
    tab.panes = std::move(panes);
    tab.layout = grown;
    tab.root = MakePresetTree(grown, order);
    if (grown != LayoutPreset::TwoVertical && grown != LayoutPreset::TwoHorizontal) {
        tab.compare = false;
        tab.compare_diff_only = false;
    }
    tab.target_index = -1;  // indices follow the new order
    for (size_t i = 0; i < tab.panes.size(); ++i) {
        tab.panes[i]->focused = tab.panes[i].get() == added;
        if (tab.panes[i].get() == added) tab.focused_index = static_cast<int>(i);
        if (tab.panes[i].get() == target) tab.target_index = static_cast<int>(i);
    }
    added->target = false;
    return true;
}
}  // namespace pulse::app
