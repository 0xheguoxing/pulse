// details_tree_rows.h — Per-row tree facts for inline folder expansion in the
// details view (Finder-style disclosure). Built on the app side, read by the
// renderer; rows are aligned with the pane's view rows.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace pulse::ui {

enum class TreeRowKind : uint8_t {
    Entry,    // a file or folder (base row or an expanded child)
    Loading,  // the folder is being read; drawn after kTreeLoadingDelayMs
    Empty,    // the folder has nothing to show
    Failed,   // the folder could not be read
    More,     // "showing X of Y" with show more / show all / open in new tab
};

struct DetailsTreeRow {
    uint16_t depth = 0;
    TreeRowKind kind = TreeRowKind::Entry;
    bool expandable = false;  // folder row: draws a chevron
    bool expanded = false;    // chevron points down
    uint32_t shown = 0;       // More rows: rows listed so far
    uint32_t total = 0;       // More rows: visible children; expanded big folders: child count pill
    uint64_t since_ms = 0;    // Loading rows: motion::NowMs() when the read started
};

struct DetailsTreeRows {
    std::vector<DetailsTreeRow> rows;  // view row -> facts
    std::vector<int> view_of_source;   // source index -> view row, -1 when not listed
    uint64_t revision = 0;

    const DetailsTreeRow* At(int view_row) const {
        return view_row >= 0 && static_cast<size_t>(view_row) < rows.size()
            ? &rows[static_cast<size_t>(view_row)] : nullptr;
    }
    int ViewOf(int source) const {
        return source >= 0 && static_cast<size_t>(source) < view_of_source.size()
            ? view_of_source[static_cast<size_t>(source)] : -1;
    }
    // View row of the folder that owns view_row (-1 at depth 0).
    int ParentRow(int view_row) const {
        const DetailsTreeRow* row = At(view_row);
        if (!row || row->depth == 0) return -1;
        // Placeholders sit one level below their folder, like its children.
        const uint16_t want = static_cast<uint16_t>(row->depth - 1);
        for (int r = view_row - 1; r >= 0; --r) {
            const DetailsTreeRow& up = rows[static_cast<size_t>(r)];
            if (up.kind == TreeRowKind::Entry && up.depth == want) return r;
        }
        return -1;
    }
};

// Geometry in DIP. Every row reserves the chevron slot, so expanding the first
// folder never shifts the list sideways.
inline constexpr float kTreeChevronDip = 18.0f;
inline constexpr float kTreeIndentDip = 18.0f;
inline constexpr uint64_t kTreeLoadingDelayMs = 150;
// "Show N more" step on the more row.
inline constexpr uint32_t kTreeMoreStep = 500;
// Folders with more children than this show their count next to the name.
inline constexpr uint32_t kTreeCountPillMin = 500;

inline float TreeOffsetDip(const DetailsTreeRow& row) {
    return kTreeChevronDip + static_cast<float>(row.depth) * kTreeIndentDip;
}

// Guide lines: the line of a depth-k folder ("level k") runs down from under
// its chevron to its last child; each child joins it with a short tick, the
// last one with a rounded elbow.
inline constexpr int kTreeGuideMaxLevels = 64;

struct TreeGuideRow {
    uint64_t through = 0;  // levels drawn through the whole row (an ancestor with more below)
    int join = -1;         // level of the row's own folder, -1 at depth 0
    bool last = false;     // no later sibling: the join ends here in an elbow
};

struct TreeGuides {
    std::vector<TreeGuideRow> rows;  // view rows first..last
    int first = 0;
    // The focus row's folder: its line is highlighted on hi_first..hi_last.
    int hi_level = -1;
    int hi_first = -1, hi_last = -1;
    bool Highlighted(int view_row, int level) const {
        return level == hi_level && view_row >= hi_first && view_row <= hi_last;
    }
};

// Guides for view rows first..last; focus_row (-1: none) picks the
// highlighted line. Reads only the depths, so it costs a scan of the rows
// after `last` until every open level has been decided.
inline TreeGuides BuildTreeGuides(const DetailsTreeRows& tree, int first, int last, int focus_row) {
    TreeGuides g;
    const int n = static_cast<int>(tree.rows.size());
    first = first < 0 ? 0 : first;
    last = last >= n ? n - 1 : last;
    if (first > last) return g;
    g.first = first;
    const auto bit = [](int level) { return uint64_t{1} << level; };
    const auto below = [](int level) {  // levels 0..level-1
        return level >= kTreeGuideMaxLevels ? ~uint64_t{0} : (uint64_t{1} << level) - 1;
    };
    // Levels with a child still to come after `last`.
    uint64_t more = 0;
    int limit = 1 << 30;
    for (int j = last + 1; j < n && limit > 0; ++j) {
        const int d = tree.rows[static_cast<size_t>(j)].depth;
        if (d > limit) continue;
        if (d >= 1 && d - 1 < kTreeGuideMaxLevels) more |= bit(d - 1);
        limit = d;
    }
    g.rows.resize(static_cast<size_t>(last - first + 1));
    for (int r = last; r >= first; --r) {
        const int d = tree.rows[static_cast<size_t>(r)].depth;
        TreeGuideRow& out = g.rows[static_cast<size_t>(r - first)];
        if (d >= 1 && d - 1 < kTreeGuideMaxLevels) {
            out.through = more & below(d - 1);
            out.join = d - 1;
            out.last = (more & bit(d - 1)) == 0;
        }
        more &= below(d);  // this row closes every level at or below its depth
        if (d >= 1 && d - 1 < kTreeGuideMaxLevels) more |= bit(d - 1);
    }
    const int parent = tree.ParentRow(focus_row);
    if (parent >= 0) {
        g.hi_level = tree.rows[static_cast<size_t>(parent)].depth;
        g.hi_first = parent + 1;
        g.hi_last = parent;
        for (int j = parent + 1; j < n && j <= last &&
             tree.rows[static_cast<size_t>(j)].depth > g.hi_level; ++j)
            g.hi_last = j;
    }
    return g;
}

// 滚动时固定上级 (sticky parents): an open folder whose own row scrolled above
// the list keeps a copy pinned at the top while its children pass under it,
// at most max_rows deep. Slot k shows the depth-k folder that owns the row
// under the slot; a folder whose subtree is about to end is pushed up by the
// row after it, like VS Code's sticky scroll. row_top(view_row) is a row's
// top and row_at(y) the row at y, both relative to the top of the list.
struct TreePin {
    int row = -1;     // view row of the pinned folder
    float y = 0.0f;   // its pinned top, relative to the list (k * row_h unless pushed)
};
inline constexpr int kTreeStickyMaxRows = 3;

template <class RowTop, class RowAt>
std::vector<TreePin> PinnedTreeRows(const DetailsTreeRows& tree, float row_h, int max_rows,
                                    RowTop row_top, RowAt row_at) {
    std::vector<TreePin> pins;
    const int count = static_cast<int>(tree.rows.size());
    for (int k = 0; k < max_rows && row_h > 0.0f; ++k) {
        const float slot = static_cast<float>(k) * row_h;
        const int probe = row_at(slot);
        int folder = probe;
        const DetailsTreeRow* row = tree.At(folder);
        while (row && row->depth > k) {
            folder = tree.ParentRow(folder);
            row = tree.At(folder);
        }
        if (!row || row->depth != k || row->kind != TreeRowKind::Entry || !row->expanded) break;
        const DetailsTreeRow* first_child = tree.At(folder + 1);
        if (!first_child || first_child->depth <= k) break;   // nothing listed under it
        if (row_top(folder) >= slot - 0.5f) break;            // its own row is still in place
        if (k > 0 && tree.ParentRow(folder) != pins.back().row) break;
        // The first row after the subtree pushes the pin once it is this close.
        float y = slot;
        for (int next = std::max(folder, probe) + 1; next < count; ++next) {
            const float top = row_top(next);
            if (top >= slot + 2.0f * row_h) break;
            if (tree.rows[static_cast<size_t>(next)].depth <= k) {
                y = std::min(slot, top - row_h);
                break;
            }
        }
        pins.push_back({folder, y});
        if (y < slot) break;   // on its way out: nothing pins below it
    }
    return pins;
}

} // namespace pulse::ui
