// tag_query.h — the combined tag view pulse:tags: (pulse_tag_redesign §5, §10).
//
//   pulse:tags:all=a,b;any=c,d;not=e;in=D:\项目
//
// Tag ids, comma separated; values escape % ; , = as %XX. The result is every
// "all" tag ∧ (any "any" tag) ∧ no "not" tag ∧ inside `in` (with subfolders).
// pulse:tag:<id> stays valid and means all=<id>.
#pragma once
#include "places.h"

#include <string>
#include <utility>
#include <vector>

namespace pulse::app {

enum class TagTermKind { All = 0, Any = 1, Not = 2 };

struct TagQuery {
    std::vector<TagId> all;
    std::vector<TagId> any;
    std::vector<TagId> none;
    std::wstring in;   // folder; empty = everywhere

    bool Empty() const { return all.empty() && any.empty() && none.empty(); }
    bool Contains(const TagId& id) const;
    // Where id sits now; false when it is not in the query.
    bool KindOf(const TagId& id, TagTermKind* kind) const;
    // Moves id to `kind` (adding it when missing).
    void Set(const TagId& id, TagTermKind kind);
    bool Remove(const TagId& id);
};

inline bool IsTagViewKind(const std::wstring& kind) { return kind == L"tag" || kind == L"tags"; }

struct TagQueryTerm {
    TagId id;
    TagTermKind kind = TagTermKind::All;
};
// The terms in condition-bar order: all, then any, then not.
std::vector<TagQueryTerm> TagQueryTerms(const TagQuery& query);
// A folder's last name ("D:" for a drive root).
std::wstring TagQueryFolderLabel(const std::wstring& folder);

// `rest` is the part after "pulse:tags:".
TagQuery ParseTagQuery(const std::wstring& rest);
std::wstring MakeTagQueryPath(const TagQuery& query);
// pulse:tag:<id> or pulse:tags:…; false for any other path.
bool TagQueryFromPath(const PlacesCatalog& places, const std::wstring& path, TagQuery* out);
// Drops ids that are no longer in the catalog.
void PruneTagQuery(const PlacesCatalog& places, TagQuery& query);

// The matching paths, each once, in catalog order.
std::vector<std::wstring> TagQueryPaths(const PlacesCatalog& places, const TagQuery& query);
bool TagQueryMatches(const PlacesCatalog& places, const TagQuery& query, const std::wstring& path);

// 客户A + 设计稿 − 归档 (任一 terms joined by |).
std::wstring TagQueryTitle(const PlacesCatalog& places, const TagQuery& query);

// Refinements: other tags on the results with their counts, most first;
// tags on none or on every result are left out (they would not narrow).
std::vector<std::pair<TagId, int>> TagQueryRefinements(const PlacesCatalog& places,
                                                        const TagQuery& query,
                                                        const std::vector<std::wstring>& paths,
                                                        size_t limit);
// The parent folders holding most results, with counts (two or more distinct
// folders, else none: a single folder would not narrow).
std::vector<std::pair<std::wstring, int>> TagQueryLocations(const std::vector<std::wstring>& paths,
                                                            size_t limit);

} // namespace pulse::app
