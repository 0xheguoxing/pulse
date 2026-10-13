// pane_drop.cpp - Multi-window M6d: content-area drop zones and adding a
// dropped tab's pane to a layout.
#include "pane_drop.h"

#include "pane_layout.h"
#include "split_tree_shape.h"

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

// The visible panes in `placement`'s order with `added` at its slots.
std::vector<Pane*> PlacedOrder(const std::vector<Pane*>& visible, const std::vector<Pane*>& added,
                               const PanePlacement& placement) {
    std::vector<Pane*> kept = visible;
    if (placement.kept.size() == visible.size())
        for (size_t i = 0; i < visible.size(); ++i) kept[i] = visible[placement.kept[i]];
    std::vector<Pane*> order;
    order.reserve(visible.size() + added.size());
    size_t next_kept = 0, next_added = 0;
    for (size_t at = 0; at < visible.size() + added.size(); ++at) {
        const bool slot = next_added < placement.slots.size() && placement.slots[next_added] == at;
        if (slot && next_added < added.size()) order.push_back(added[next_added++]);
        else if (next_kept < kept.size()) order.push_back(kept[next_kept++]);
    }
    return order;
}

// True when `placement` puts `added` panes into a tab showing `visible`.
bool ValidPlacement(const PanePlacement& placement, size_t visible, size_t added) {
    const size_t total = visible + added;
    if (!placement.Splits() || visible == 0 || placement.slots.size() != added || total > kMaxTabPanes ||
        LayoutPresetCount(placement.layout) != total)
        return false;
    for (size_t i = 0; i < placement.slots.size(); ++i)
        if (placement.slots[i] >= total || (i > 0 && placement.slots[i] <= placement.slots[i - 1])) return false;
    if (placement.kept.empty()) return true;
    std::vector<size_t> sorted = placement.kept;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i] != i) return false;
    return sorted.size() == visible;
}

// Lays out `placement` in `content`; `incoming` stand for the added panes.
std::vector<std::pair<Pane*, D2D1_RECT_F>> LayOut(const std::vector<Pane*>& visible, const std::vector<Pane*>& incoming,
                                                  const PanePlacement& placement, const D2D1_RECT_F& content,
                                                  float scale) {
    std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
    const auto tree = MakePresetTree(placement.layout, PlacedOrder(visible, incoming, placement));
    if (tree) LayoutWindowPanes(*tree, content, scale, laid);
    return laid;
}

float Distance(const D2D1_RECT_F& r, POINT pt) {
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    const float dx = (std::max)({r.left - x, 0.0f, x - r.right}), dy = (std::max)({r.top - y, 0.0f, y - r.bottom});
    return std::sqrt(dx * dx + dy * dy);
}

// M7k: the owner of the leaf showing `pane` in the tree under `node`.
std::unique_ptr<SplitContainer>* LeafOf(std::unique_ptr<SplitContainer>& node, const Pane* pane) {
    if (!node) return nullptr;
    if (node->is_leaf) return node->pane == pane ? &node : nullptr;
    if (auto* found = LeafOf(node->first, pane)) return found;
    return LeafOf(node->second, pane);
}

std::unique_ptr<SplitContainer> CopyTree(const SplitContainer& node) {
    if (node.is_leaf) return SplitContainer::CreateLeaf(node.pane);
    return SplitContainer::Join(node.orientation, node.ratio, node.first ? CopyTree(*node.first) : nullptr,
                                node.second ? CopyTree(*node.second) : nullptr);
}

// Splits the leaf of `pane` in half at `side`; `half` takes that half.
bool SplitLeaf(std::unique_ptr<SplitContainer>& root, const Pane* pane, ContentDrop side,
               std::unique_ptr<SplitContainer> half) {
    std::unique_ptr<SplitContainer>* const leaf = LeafOf(root, pane);
    if (!leaf || !half || !IsEdge(side)) return false;
    const SplitOrientation orientation =
        side == ContentDrop::Left || side == ContentDrop::Right ? SplitOrientation::Vertical : SplitOrientation::Horizontal;
    std::unique_ptr<SplitContainer> kept = std::move(*leaf);
    *leaf = Leading(side) ? SplitContainer::Join(orientation, 0.5f, std::move(half), std::move(kept))
                          : SplitContainer::Join(orientation, 0.5f, std::move(kept), std::move(half));
    return true;
}

// Takes the leaf of `pane` out of the tree under `node`: its sibling takes
// the place of the split they shared.
bool DropLeaf(std::unique_ptr<SplitContainer>& node, const Pane* pane) {
    if (!node || node->is_leaf) return false;
    const auto is_pane = [&](const std::unique_ptr<SplitContainer>& child) {
        return child && child->is_leaf && child->pane == pane;
    };
    if (is_pane(node->first) || is_pane(node->second)) {
        std::unique_ptr<SplitContainer>& sibling = is_pane(node->first) ? node->second : node->first;
        if (!sibling) return false;
        std::unique_ptr<SplitContainer> kept = std::move(sibling);
        node = std::move(kept);
        return true;
    }
    return DropLeaf(node->first, pane) || DropLeaf(node->second, pane);
}

// True when the pane split of `placement` fits `tab` with `added` panes.
bool ValidSplit(const LayoutTab& tab, const PanePlacement& placement, size_t added) {
    if (!placement.pane || !placement.Splits() || !tab.root || added == 0) return false;
    const std::vector<Pane*> visible = tab.VisiblePanes();
    return visible.size() + added <= kMaxTabPanes &&
           std::find(visible.begin(), visible.end(), placement.pane) != visible.end();
}

// The edge of `pane` nearest to `pt`, as a share of its width (left, right)
// or height (top, bottom): the diagonals cut the pane into four sides (M7j).
ContentDrop NearestPaneEdge(const D2D1_RECT_F& pane, D2D1_POINT_2F pt) {
    const float w = (std::max)(1.0f, pane.right - pane.left), h = (std::max)(1.0f, pane.bottom - pane.top);
    const float left = (pt.x - pane.left) / w, right = (pane.right - pt.x) / w;
    const float top = (pt.y - pane.top) / h, bottom = (pane.bottom - pt.y) / h;
    const float nearest = (std::min)({left, right, top, bottom});
    if (nearest == left) return ContentDrop::Left;
    if (nearest == right) return ContentDrop::Right;
    return nearest == top ? ContentDrop::Top : ContentDrop::Bottom;
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

PanePlacement EdgePlacement(const LayoutTab& tab, size_t added, ContentDrop edge) {
    const size_t visible = tab.VisiblePanes().size();
    PanePlacement placement{GrownPreset(visible, added, edge), {}};
    if (placement.layout == LayoutPreset::Single) return {};
    if (visible == 2 && added == 2 && (edge == ContentDrop::Left || edge == ContentDrop::Right)) {
        placement.slots = edge == ContentDrop::Left ? std::vector<size_t>{0, 2} : std::vector<size_t>{1, 3};
        return placement;
    }
    const size_t first = Leading(edge) ? 0 : visible;
    for (size_t i = 0; i < added; ++i) placement.slots.push_back(first + i);
    return placement;
}

std::vector<D2D1_RECT_F> PlacementRects(const LayoutTab& tab, const PanePlacement& placement,
                                        const D2D1_RECT_F& content, float scale, const LayoutTab* incoming_tab) {
    if (placement.pane) {
        // The tree as it will be, with the incoming panes (or one stand-in
        // for all of them) in the half they take.
        const bool own = incoming_tab && incoming_tab->root && DropPaneCount(*incoming_tab) > 0;
        Pane stand_in;
        const std::vector<Pane*> want = own ? incoming_tab->VisiblePanes() : std::vector<Pane*>{&stand_in};
        if (!ValidSplit(tab, placement, want.size())) return {content};
        std::unique_ptr<SplitContainer> tree = CopyTree(*tab.root);
        if (!SplitLeaf(tree, placement.pane, placement.side,
                       own ? CopyTree(*incoming_tab->root) : SplitContainer::CreateLeaf(&stand_in)))
            return {content};
        std::vector<std::pair<Pane*, D2D1_RECT_F>> laid;
        LayoutWindowPanes(*tree, content, scale, laid);
        std::vector<D2D1_RECT_F> rects;
        for (const Pane* pane : want)
            for (const auto& [placed, rect] : laid)
                if (placed == pane) rects.push_back(rect);
        return rects.size() == want.size() ? rects : std::vector<D2D1_RECT_F>{content};
    }
    const std::vector<Pane*> visible = tab.VisiblePanes();
    const size_t added = placement.slots.size();
    if (!ValidPlacement(placement, visible.size(), added)) return {content};
    std::vector<Pane> placeholders(added);
    std::vector<Pane*> incoming;
    for (Pane& pane : placeholders) incoming.push_back(&pane);
    const auto laid = LayOut(visible, incoming, placement, content, scale);
    std::vector<D2D1_RECT_F> rects;
    for (Pane* want : incoming)
        for (const auto& [pane, rect] : laid)
            if (pane == want) rects.push_back(rect);
    return rects.size() == added ? rects : std::vector<D2D1_RECT_F>{content};
}

std::vector<D2D1_RECT_F> PaneDropRects(const LayoutTab& tab, size_t added, ContentDrop edge,
                                       const D2D1_RECT_F& content, float scale) {
    return PlacementRects(tab, EdgePlacement(tab, added, edge), content, scale);
}

D2D1_RECT_F PaneDropRect(const LayoutTab& tab, ContentDrop edge, const D2D1_RECT_F& content, float scale) {
    return PaneDropRects(tab, 1, edge, content, scale).front();
}

PanePlacement AimPaneDrop(const LayoutTab& tab, size_t added, const D2D1_RECT_F& content, float scale, POINT pt,
                          ContentDrop* side) {
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    const bool outside = x < content.left || x >= content.right || y < content.top || y >= content.bottom;
    if (side) *side = outside ? ContentDrop::None : ContentDrop::Center;
    const std::vector<Pane*> visible = tab.VisiblePanes();
    const size_t total = visible.size() + added;
    if (outside || !tab.root || visible.empty() || added == 0 || total > kMaxTabPanes) return {};
    // The pane under the cursor (the nearest one on a splitter).
    std::vector<std::pair<Pane*, D2D1_RECT_F>> now;
    LayoutWindowPanes(*tab.root, content, scale, now);
    if (now.empty()) return {};
    const auto under = std::min_element(now.begin(), now.end(), [&](const auto& a, const auto& b) {
        return Distance(a.second, pt) < Distance(b.second, pt);
    });
    const D2D1_RECT_F pane = under->second;
    const D2D1_POINT_2F nearest{(std::clamp)(static_cast<float>(pt.x), pane.left, pane.right - 1.0f),
                                (std::clamp)(static_cast<float>(pt.y), pane.top, pane.bottom - 1.0f)};
    // No middle that would be a new tab: a cursor on a pane means that pane
    // (M7j; a new tab is a drop on the strip).
    const ContentDrop edge = NearestPaneEdge(pane, nearest);
    if (side) *side = edge;
    // Exactly that pane splits in half there; the others stay put (M7k).
    PanePlacement placement;
    placement.layout = LayoutPreset::Custom;
    placement.pane = under->first;
    placement.side = edge;
    return placement;
}

bool CanPlacePanes(const LayoutTab& tab, const LayoutTab& incoming, const PanePlacement& placement) {
    if (!HasRoomForPanes(tab, incoming)) return false;
    return placement.pane ? ValidSplit(tab, placement, DropPaneCount(incoming))
                          : ValidPlacement(placement, tab.VisiblePanes().size(), DropPaneCount(incoming));
}

bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, const PanePlacement& placement) {
    if (!incoming || !CanPlacePanes(tab, *incoming, placement)) return false;
    const std::vector<Pane*> added = incoming->VisiblePanes();  // their on-screen order
    Pane* focus = incoming->FocusedPane();
    if (!focus || std::find(added.begin(), added.end(), focus) == added.end()) focus = added.front();
    std::vector<std::unique_ptr<Pane>> moved = std::move(incoming->panes);
    incoming->panes.clear();
    std::unique_ptr<SplitContainer> half = std::move(incoming->root);
    if (placement.pane) {
        // They keep their own arrangement in the half of the pane (M7k); a
        // tree that came out as a preset after all is named so.
        SplitLeaf(tab.root, placement.pane, placement.side, std::move(half));
        tab.layout = PresetOfTree(*tab.root);
        StorePanes(tab, tab.VisiblePanes(), std::move(moved), focus);
        for (Pane* pane : added) pane->target = false;
        return true;
    }
    const std::vector<Pane*> order = PlacedOrder(tab.VisiblePanes(), added, placement);
    tab.layout = placement.layout;
    tab.root = MakePresetTree(placement.layout, order);
    StorePanes(tab, order, std::move(moved), focus);
    for (Pane* pane : added) pane->target = false;  // the copy/move target stays where it was
    return true;
}

bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, ContentDrop edge) {
    if (!incoming || !CanAddPane(tab, *incoming, edge)) return false;
    const PanePlacement placement = EdgePlacement(tab, DropPaneCount(*incoming), edge);
    return AddPaneFromTab(tab, std::move(incoming), placement);
}

std::unique_ptr<LayoutTab> TakePaneAsTab(LayoutTab& tab, Pane* pane, PaneOrigin& origin) {
    std::vector<Pane*> order = tab.VisiblePanes();
    const auto it = std::find(order.begin(), order.end(), pane);
    if (!pane || order.size() < 2 || it == order.end() || !tab.root) return nullptr;
    origin = PaneOrigin{static_cast<size_t>(it - order.begin()), tab.layout, {}, false};
    origin.shape = SplitTreeShape(*tab.root);
    origin.target = tab.target_index >= 0 && static_cast<size_t>(tab.target_index) < tab.panes.size() &&
                    tab.panes[static_cast<size_t>(tab.target_index)].get() == pane;
    if (tab.root) CollectSplitRatios(*tab.root, origin.ratios);
    const Pane* focus = tab.FocusedPane() == pane ? nullptr : tab.FocusedPane();
    // Its sibling closes up over the place they shared (M7k).
    if (!DropLeaf(tab.root, pane)) return nullptr;
    order = tab.VisiblePanes();

    auto own = std::make_unique<LayoutTab>();
    for (auto& owned : tab.panes) {
        if (owned.get() == pane) {
            own->panes.push_back(std::move(owned));
            break;
        }
    }
    if (!focus) focus = order.front();
    tab.layout = PresetOfTree(*tab.root);
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
           tab.VisiblePanes().size() + 1 == ShapePaneCount(origin.shape);
}

bool PutPaneBack(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, const PaneOrigin& origin) {
    if (!incoming || !CanPutPaneBack(tab, *incoming, origin)) return false;
    Pane* const added = incoming->panes[0].get();
    std::vector<Pane*> order = tab.VisiblePanes();
    order.insert(order.begin() + static_cast<ptrdiff_t>((std::min)(origin.position, order.size())), added);
    std::unique_ptr<SplitContainer> root = MakeShapedTree(origin.shape, order);
    if (!root) return false;
    std::unique_ptr<Pane> moved = std::move(incoming->panes[0]);
    incoming->panes.clear();
    incoming->root.reset();
    ApplySplitRatios(*root, origin.ratios);
    tab.root = std::move(root);
    tab.layout = PresetOfTree(*tab.root);
    std::vector<std::unique_ptr<Pane>> extras;
    extras.push_back(std::move(moved));
    StorePanes(tab, order, std::move(extras), added);
    added->target = origin.target;
    for (size_t i = 0; origin.target && i < tab.panes.size(); ++i)
        if (tab.panes[i].get() == added) tab.target_index = static_cast<int>(i);
    return true;
}
}  // namespace pulse::app
