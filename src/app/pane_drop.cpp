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

// Stores `tab`'s panes with `order` (its visible panes) first, then the
// hidden ones; focuses `focus` (or keeps the focused pane) and remaps the
// target. `extras` are panes joining `tab`, each placed in `order`.
void StorePanes(LayoutTab& tab, const std::vector<Pane*>& order, std::vector<std::unique_ptr<Pane>> extras,
                const Pane* focus) {
    const auto pane_at = [&](int index) -> const Pane* {
        return index >= 0 && static_cast<size_t>(index) < tab.panes.size() ? tab.panes[static_cast<size_t>(index)].get()
                                                                          : nullptr;
    };
    const Pane* const target = pane_at(tab.target_index);
    if (!focus) focus = pane_at(tab.focused_index);
    std::vector<std::unique_ptr<Pane>> panes;
    panes.reserve(tab.panes.size() + extras.size());
    for (Pane* pane : order) {
        for (auto& owned : extras)
            if (owned.get() == pane) panes.push_back(std::move(owned));
        for (auto& owned : tab.panes)
            if (owned.get() == pane) panes.push_back(std::move(owned));
    }
    for (auto& owned : tab.panes)
        if (owned) panes.push_back(std::move(owned));  // hidden panes keep their order after the visible ones
    tab.panes = std::move(panes);
    tab.focused_index = 0;
    tab.target_index = -1;
    for (size_t i = 0; i < tab.panes.size(); ++i) {
        if (tab.panes[i].get() == focus) tab.focused_index = static_cast<int>(i);
        if (tab.panes[i].get() == target) tab.target_index = static_cast<int>(i);
    }
    for (size_t i = 0; i < tab.panes.size(); ++i) tab.panes[i]->focused = static_cast<int>(i) == tab.focused_index;
    if (tab.layout != LayoutPreset::TwoVertical && tab.layout != LayoutPreset::TwoHorizontal) {
        tab.compare = false;
        tab.compare_diff_only = false;
    }
}

LayoutPreset ShrunkPreset(size_t visible) {
    switch (visible) {
    case 3: return LayoutPreset::TwoVertical;
    case 4: return LayoutPreset::Three;
    default: return LayoutPreset::Single;
    }
}

// The visible panes of `tab` with `added` in front or at the back. Two
// joining two at the left or right fill one column of the grid (top-left and
// bottom-left, or the right ones) instead of the top or bottom row.
std::vector<Pane*> GrownOrder(const LayoutTab& tab, const std::vector<Pane*>& added, ContentDrop edge) {
    std::vector<Pane*> order = tab.VisiblePanes();
    if (order.size() == 2 && added.size() == 2 && (edge == ContentDrop::Left || edge == ContentDrop::Right)) {
        return edge == ContentDrop::Left ? std::vector<Pane*>{added[0], order[0], added[1], order[1]}
                                         : std::vector<Pane*>{order[0], added[0], order[1], added[1]};
    }
    order.insert(Leading(edge) ? order.begin() : order.end(), added.begin(), added.end());
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

LayoutPreset GrownPreset(size_t visible, size_t added, ContentDrop edge) {
    if (!IsEdge(edge) || visible == 0 || added == 0) return LayoutPreset::Single;
    switch (visible + added) {
    case 2: return edge == ContentDrop::Left || edge == ContentDrop::Right ? LayoutPreset::TwoVertical
                                                                          : LayoutPreset::TwoHorizontal;
    case 3: return LayoutPreset::Three;
    case 4: return LayoutPreset::FourGrid;
    default: return LayoutPreset::Single;
    }
}

size_t DropPaneCount(const LayoutTab& incoming) {
    if (!incoming.root || incoming.panes.empty()) return 0;
    const std::vector<Pane*> visible = incoming.VisiblePanes();
    if (visible.size() != incoming.panes.size()) return 0;
    for (const Pane* pane : visible)
        if (!pane) return 0;
    return visible.size();
}

bool HasRoomForPanes(const LayoutTab& tab, const LayoutTab& incoming) {
    const size_t added = DropPaneCount(incoming);
    return tab.root && added > 0 && tab.VisiblePanes().size() + added <= kMaxTabPanes;
}

bool CanAddPane(const LayoutTab& tab, const LayoutTab& incoming, ContentDrop edge) {
    return HasRoomForPanes(tab, incoming) &&
           GrownPreset(tab.VisiblePanes().size(), DropPaneCount(incoming), edge) != LayoutPreset::Single;
}

std::vector<D2D1_RECT_F> PaneDropRects(const LayoutTab& tab, size_t added, ContentDrop edge,
                                       const D2D1_RECT_F& content, float scale) {
    const LayoutPreset grown = GrownPreset(tab.VisiblePanes().size(), added, edge);
    if (grown == LayoutPreset::Single) return {content};
    std::vector<Pane> placeholders(added);
    std::vector<Pane*> incoming;
    for (Pane& pane : placeholders) incoming.push_back(&pane);
    const auto tree = MakePresetTree(grown, GrownOrder(tab, incoming, edge));
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    LayoutWindowPanes(*tree, content, scale, laid);
    std::vector<D2D1_RECT_F> rects;
    for (Pane* want : incoming)
        for (const auto& [pane, rect] : laid)
            if (pane == want) rects.push_back(rect);
    return rects.size() == added ? rects : std::vector<D2D1_RECT_F>{content};
}

D2D1_RECT_F PaneDropRect(const LayoutTab& tab, ContentDrop edge, const D2D1_RECT_F& content, float scale) {
    return PaneDropRects(tab, 1, edge, content, scale).front();
}

bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, ContentDrop edge) {
    if (!incoming || !CanAddPane(tab, *incoming, edge)) return false;
    const std::vector<Pane*> added = incoming->VisiblePanes();  // their on-screen order
    const LayoutPreset grown = GrownPreset(tab.VisiblePanes().size(), added.size(), edge);
    Pane* focus = incoming->FocusedPane();
    if (!focus || std::find(added.begin(), added.end(), focus) == added.end()) focus = added.front();
    std::vector<std::unique_ptr<Pane>> moved = std::move(incoming->panes);
    incoming->panes.clear();
    incoming->root.reset();
    const std::vector<Pane*> order = GrownOrder(tab, added, edge);
    tab.layout = grown;
    tab.root = MakePresetTree(grown, order);
    StorePanes(tab, order, std::move(moved), focus);
    for (Pane* pane : added) pane->target = false;  // the copy/move target stays where it was
    return true;
}

std::unique_ptr<LayoutTab> TakePaneAsTab(LayoutTab& tab, Pane* pane, PaneOrigin& origin) {
    std::vector<Pane*> order = tab.VisiblePanes();
    const auto it = std::find(order.begin(), order.end(), pane);
    if (!pane || order.size() < 2 || it == order.end()) return nullptr;
    const size_t visible = order.size();
    origin = PaneOrigin{static_cast<size_t>(it - order.begin()), tab.layout, {}, false};
    origin.target = tab.target_index >= 0 && static_cast<size_t>(tab.target_index) < tab.panes.size() &&
                    tab.panes[static_cast<size_t>(tab.target_index)].get() == pane;
    if (tab.root) CollectSplitRatios(*tab.root, origin.ratios);
    const Pane* focus = tab.FocusedPane() == pane ? nullptr : tab.FocusedPane();
    order.erase(it);

    auto own = std::make_unique<LayoutTab>();
    for (auto& owned : tab.panes) {
        if (owned.get() == pane) {
            own->panes.push_back(std::move(owned));
            break;
        }
    }
    if (!focus) focus = order.front();
    tab.layout = ShrunkPreset(visible);
    tab.root = MakePresetTree(tab.layout, order);
    StorePanes(tab, order, {}, focus);

    own->root = SplitContainer::CreateLeaf(pane);
    own->layout = LayoutPreset::Single;
    own->focused_index = 0;
    pane->focused = true;
    pane->target = false;
    return own;
}

bool CanPutPaneBack(const LayoutTab& tab, const LayoutTab& incoming, const PaneOrigin& origin) {
    return incoming.panes.size() == 1 && incoming.panes[0] &&
           tab.VisiblePanes().size() + 1 == LayoutPresetCount(origin.layout);
}

bool PutPaneBack(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, const PaneOrigin& origin) {
    if (!incoming || !CanPutPaneBack(tab, *incoming, origin)) return false;
    std::unique_ptr<Pane> moved = std::move(incoming->panes[0]);
    incoming->panes.clear();
    incoming->root.reset();
    Pane* const added = moved.get();
    std::vector<Pane*> order = tab.VisiblePanes();
    order.insert(order.begin() + static_cast<ptrdiff_t>((std::min)(origin.position, order.size())), added);
    tab.layout = origin.layout;
    tab.root = MakePresetTree(origin.layout, order);
    if (tab.root) ApplySplitRatios(*tab.root, origin.ratios);
    std::vector<std::unique_ptr<Pane>> extras;
    extras.push_back(std::move(moved));
    StorePanes(tab, order, std::move(extras), added);
    added->target = origin.target;
    for (size_t i = 0; origin.target && i < tab.panes.size(); ++i)
        if (tab.panes[i].get() == added) tab.target_index = static_cast<int>(i);
    return true;
}
}  // namespace pulse::app
