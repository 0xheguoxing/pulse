// details_tree_rows.h — Per-row tree facts for inline folder expansion in the
// details view (Finder-style disclosure). Built on the app side, read by the
// renderer; rows are aligned with the pane's view rows.
#pragma once
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

} // namespace pulse::ui
