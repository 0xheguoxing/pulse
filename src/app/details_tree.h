// details_tree.h — Inline folder expansion in the details view: per-tab state
// and the view built from it. Data only; app_details_tree.cpp builds the view
// and loads folders, Tab::EntryAt/EntryCount/EntryVisible read it.
#pragma once
#include "../fs/fs_snapshot.h"
#include "../ui/details_tree_rows.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::app {

inline constexpr size_t kTreeBatch = 100;         // children listed when a folder opens
inline constexpr size_t kTreeMoreStep = ui::kTreeMoreStep;  // "show N more"
inline constexpr size_t kTreeRecurseRows = 2000;  // Alt+click: rows added at most
inline constexpr int kTreeRecurseDepth = 3;       // Alt+click: levels below the folder
inline constexpr size_t kTreeRecurseSkip = 500;   // Alt+click: bigger folders stay closed
inline constexpr int kTreeMaxDepth = 32;

struct DetailsTreeFolder {
    enum class State : uint8_t { Idle, Loading, Ready, Failed };
    std::wstring path;            // normalized folder path
    bool expanded = false;
    State state = State::Idle;
    fs::SnapshotPtr listing;      // as listed by the worker (any order)
    uint64_t generation = 0;      // worker request in flight, 0 = none
    uint64_t since_ms = 0;        // motion::NowMs() when the read started
    size_t shown = kTreeBatch;    // children listed before the "more" row
    // Visible children in display order (indices into listing).
    std::vector<int> order;
    fs::SnapshotPtr order_listing;
    int order_key = -1;           // sort column * 8 + direction * 4 + hidden mask
    // Alt+click / numpad * (see DetailsTree::Recursion).
    uint64_t recurse_id = 0;      // the recursion that reached this folder
    int recurse = 0;              // levels it still opens below its children
    bool auto_opened = false;     // opened by the recursion, not by the user
};

struct DetailsTree {
    // Keyed by lower-cased normalized path; survives navigation within the tab.
    std::unordered_map<std::wstring, DetailsTreeFolder> folders;
    uint64_t revision = 1;        // bumped by every change that alters rows

    // Built view, valid while base == Tab::snapshot.
    fs::SnapshotPtr base;
    size_t base_size = 0;
    fs::SnapshotPtr combined;     // base followed by child and placeholder entries
    std::shared_ptr<const std::vector<int>> map;  // view row -> source index (a PaneViewModel::FilterMap)
    std::shared_ptr<const ui::DetailsTreeRows> rows;
    std::vector<uint8_t> placeholder;  // by (source index - base_size): 0 entry, 1 stand-in, 2 more row
    std::vector<std::wstring> visible_folders;  // expanded folders actually present in the built view
    uint64_t built_revision = 0;
    int built_key = -1;

    // The last recursive expansion: opens folders kTreeRecurseDepth levels
    // down, leaves folders over kTreeRecurseSkip children closed and stops
    // after kTreeRecurseRows rows; reported once with an undo.
    struct Recursion {
        uint64_t id = 0;                   // 0 = none
        std::wstring root;                 // key of the folder it started from
        bool root_was_expanded = false;
        std::vector<std::wstring> opened;  // keys it expanded below the root
        size_t rows = 0;                   // child rows it listed (last build)
        int skipped = 0;                   // folders left closed for size
        bool stopped = false;              // reached kTreeRecurseRows
        bool reported = false;
    } recursion;

    // Selection on child rows, carried over a rebuild by path.
    std::vector<std::wstring> stash_selected;
    std::wstring stash_focus;

    bool IsPlaceholder(size_t index) const {
        if (index < base_size) return false;
        const size_t k = index - base_size;
        return k < placeholder.size() && placeholder[k] != 0;
    }
    bool IsMoreRow(size_t index) const {
        if (index < base_size) return false;
        const size_t k = index - base_size;
        return k < placeholder.size() && placeholder[k] == 2;
    }
    void Clear() {
        base.reset();
        base_size = 0;
        combined.reset();
        map.reset();
        rows.reset();
        placeholder.clear();
        visible_folders.clear();
        built_revision = 0;
        built_key = -1;
    }
};

} // namespace pulse::app
