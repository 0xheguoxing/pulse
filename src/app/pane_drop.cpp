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

float Area(const D2D1_RECT_F& r) { return (std::max)(0.0f, r.right - r.left) * (std::max)(0.0f, r.bottom - r.top); }

float Overlap(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return Area(D2D1::RectF((std::max)(a.left, b.left), (std::max)(a.top, b.top), (std::min)(a.right, b.right),
                            (std::min)(a.bottom, b.bottom)));
}

bool Inside(const D2D1_RECT_F& r, POINT pt) {
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

float Distance(const D2D1_RECT_F& r, POINT pt) {
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y);
    const float dx = (std::max)({r.left - x, 0.0f, x - r.right}), dy = (std::max)({r.top - y, 0.0f, y - r.bottom});
    return std::sqrt(dx * dx + dy * dy);
}

D2D1_POINT_2F Center(const D2D1_RECT_F& r) { return {(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f}; }

// The half of `pane` at `side`: where the user means the new panes to go.
D2D1_RECT_F HalfAt(const D2D1_RECT_F& pane, ContentDrop side) {
    const float mx = (pane.left + pane.right) * 0.5f, my = (pane.top + pane.bottom) * 0.5f;
    switch (side) {
    case ContentDrop::Left: return D2D1::RectF(pane.left, pane.top, mx, pane.bottom);
    case ContentDrop::Right: return D2D1::RectF(mx, pane.top, pane.right, pane.bottom);
    case ContentDrop::Top: return D2D1::RectF(pane.left, pane.top, pane.right, my);
    default: return D2D1::RectF(pane.left, my, pane.right, pane.bottom);
    }
}

// Every way to choose `count` ascending places out of `total`.
void Combinations(size_t total, size_t count, size_t from, std::vector<size_t>& pick,
                  std::vector<std::vector<size_t>>& out) {
    if (pick.size() == count) {
        out.push_back(pick);
        return;
    }
    for (size_t at = from; at < total; ++at) {
        pick.push_back(at);
        Combinations(total, count, at + 1, pick, out);
        pick.pop_back();
    }
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
                                        const D2D1_RECT_F& content, float scale) {
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
    const D2D1_RECT_F want = HalfAt(pane, edge);
    const float want_area = (std::max)(1.0f, Area(want));
    const float diagonal = (std::max)(1.0f, std::hypot(content.right - content.left, content.bottom - content.top));

    std::vector<LayoutPreset> presets;
    if (total == 2) presets = {LayoutPreset::TwoVertical, LayoutPreset::TwoHorizontal};
    else if (total == 3) presets = {LayoutPreset::Three};
    else presets = {LayoutPreset::FourGrid};
    std::vector<std::vector<size_t>> choices;
    std::vector<size_t> pick;
    Combinations(total, added, 0, pick, choices);

    std::vector<Pane> placeholders(added);
    std::vector<Pane*> incoming;
    for (Pane& p : placeholders) incoming.push_back(&p);
    // The panes already there may trade places (at most 3! orders): the top
    // of a big left pane takes its place and pushes it down, say.
    std::vector<std::vector<size_t>> orders;
    std::vector<size_t> order(visible.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    do {
        orders.push_back(order);
    } while (std::next_permutation(order.begin(), order.end()));
    orders.front().clear();  // the first is their own order

    PanePlacement best;
    float best_score = -1e9f;
    for (const LayoutPreset preset : presets) {
        for (const std::vector<size_t>& slots : choices) {
            for (const std::vector<size_t>& kept : orders) {
                const PanePlacement candidate{preset, slots, kept};
                const auto laid = LayOut(visible, incoming, candidate, content, scale);
                float covered = 0.0f, new_area = 0.0f, moved = 0.0f;
                bool cursor = false;
                for (const auto& [p, rect] : laid) {
                    if (std::find(incoming.begin(), incoming.end(), p) != incoming.end()) {
                        covered += Overlap(rect, want);
                        new_area += Area(rect);
                        cursor = cursor || Inside(rect, pt);
                        continue;
                    }
                    for (const auto& [old_pane, old_rect] : now) {
                        if (old_pane != p) continue;
                        const D2D1_POINT_2F a = Center(old_rect), b = Center(rect);
                        moved += std::hypot(a.x - b.x, a.y - b.y) / diagonal;
                    }
                }
                // Fill the meant half, without spilling far past it, under the
                // cursor; then keep the other panes where they were.
                const float score = covered / want_area + 0.5f * (covered / (std::max)(1.0f, new_area)) +
                                    (cursor ? 0.5f : 0.0f) - 0.5f * moved;
                if (score > best_score + 1e-4f) {
                    best_score = score;
                    best = candidate;
                }
            }
        }
    }
    return best;
}

bool CanPlacePanes(const LayoutTab& tab, const LayoutTab& incoming, const PanePlacement& placement) {
    return HasRoomForPanes(tab, incoming) &&
           ValidPlacement(placement, tab.VisiblePanes().size(), DropPaneCount(incoming));
}

bool AddPaneFromTab(LayoutTab& tab, std::unique_ptr<LayoutTab> incoming, const PanePlacement& placement) {
    if (!incoming || !CanPlacePanes(tab, *incoming, placement)) return false;
    const std::vector<Pane*> added = incoming->VisiblePanes();  // their on-screen order
    Pane* focus = incoming->FocusedPane();
    if (!focus || std::find(added.begin(), added.end(), focus) == added.end()) focus = added.front();
    std::vector<std::unique_ptr<Pane>> moved = std::move(incoming->panes);
    incoming->panes.clear();
    incoming->root.reset();
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
