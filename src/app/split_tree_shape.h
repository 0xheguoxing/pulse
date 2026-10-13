#pragma once
// split_tree_shape.h - Multi-window M7k: the shape of a free split tree, for
// saving it and naming it. Header-only on purpose: session_tab_model.cpp is
// linked into tests that do not link app_model.cpp, so nothing here may use
// the SplitContainer functions defined there.
#include "app_model.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {
namespace split_tree_detail {
inline void AppendShape(const SplitContainer& node, std::wstring& out) {
    if (node.is_leaf) {
        out += L'P';
        return;
    }
    out += node.orientation == SplitOrientation::Vertical ? L'V' : L'H';
    if (node.first) AppendShape(*node.first, out);
    if (node.second) AppendShape(*node.second, out);
}

inline std::unique_ptr<SplitContainer> Read(std::wstring_view shape, size_t& at, const std::vector<Pane*>& panes,
                                            size_t& next) {
    if (at >= shape.size()) return nullptr;
    const wchar_t c = shape[at++];
    auto node = std::make_unique<SplitContainer>();
    if (c == L'P') {
        if (next >= panes.size()) return nullptr;
        node->pane = panes[next++];
        return node;
    }
    node->is_leaf = false;
    node->orientation = c == L'V' ? SplitOrientation::Vertical : SplitOrientation::Horizontal;
    node->ratio = 0.5f;
    node->first = Read(shape, at, panes, next);
    node->second = node->first ? Read(shape, at, panes, next) : nullptr;
    if (!node->second) return nullptr;
    return node;
}
}  // namespace split_tree_detail

// The shape of a split tree in the order CollectPanes and CollectSplitRatios
// walk it: 'P' a pane, 'V' (left | right) or 'H' (top over bottom) a split
// followed by its two halves; "VPHPP" is LayoutPreset::Three.
inline std::wstring SplitTreeShape(const SplitContainer& root) {
    std::wstring shape;
    split_tree_detail::AppendShape(root, shape);
    return shape;
}

// How many panes `shape` holds; 0 when it is malformed.
inline size_t ShapePaneCount(std::wstring_view shape) {
    size_t open = 1, panes = 0;  // subtrees still to read
    for (const wchar_t c : shape) {
        if (open == 0) return 0;
        if (c == L'P') {
            --open;
            ++panes;
        } else if (c == L'V' || c == L'H') {
            ++open;
        } else {
            return 0;
        }
    }
    return open == 0 ? panes : 0;
}

// The tree `shape` describes with `panes` at its leaves in order and every
// ratio 0.5; null when `shape` is malformed or holds another pane count.
inline std::unique_ptr<SplitContainer> MakeShapedTree(std::wstring_view shape, const std::vector<Pane*>& panes) {
    if (panes.empty() || ShapePaneCount(shape) != panes.size()) return nullptr;
    size_t at = 0, next = 0;
    return split_tree_detail::Read(shape, at, panes, next);
}

// The preset whose tree (MakePresetTree) has the shape of `root`, else Custom.
inline LayoutPreset PresetOfTree(const SplitContainer& root) {
    const std::wstring shape = SplitTreeShape(root);
    if (shape == L"P") return LayoutPreset::Single;
    if (shape == L"VPP") return LayoutPreset::TwoVertical;
    if (shape == L"HPP") return LayoutPreset::TwoHorizontal;
    if (shape == L"VPHPP") return LayoutPreset::Three;
    if (shape == L"HVPPVPP") return LayoutPreset::FourGrid;
    return LayoutPreset::Custom;
}

// The preset with `count` panes (2 side by side, 3, 4): what a custom tree
// is saved as where only presets are known (pinned workspaces, older Pulse).
inline LayoutPreset PresetForPaneCount(size_t count) {
    switch (count) {
    case 0:
    case 1: return LayoutPreset::Single;
    case 2: return LayoutPreset::TwoVertical;
    case 3: return LayoutPreset::Three;
    default: return LayoutPreset::FourGrid;
    }
}
}  // namespace pulse::app
