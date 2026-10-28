// tag_query.cpp — the combined tag view query (see the header).
#include "tag_query.h"

#include "../fs/fs_enum.h"

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace pulse::app {
namespace {

std::wstring Escape(const std::wstring& value) {
    std::wstring out;
    for (wchar_t ch : value) {
        if (ch == L'%' || ch == L';' || ch == L',' || ch == L'=') {
            wchar_t hex[4]{};
            swprintf_s(hex, L"%%%02X", static_cast<unsigned>(ch));
            out += hex;
        } else {
            out += ch;
        }
    }
    return out;
}

int HexDigit(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

std::wstring Unescape(const std::wstring& value) {
    std::wstring out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'%' && i + 2 < value.size()) {
            const int hi = HexDigit(value[i + 1]);
            const int lo = HexDigit(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<wchar_t>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += value[i];
    }
    return out;
}

std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(separator, start);
        const size_t stop = end == std::wstring::npos ? text.size() : end;
        if (stop > start) parts.push_back(text.substr(start, stop - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return parts;
}

std::wstring JoinIds(const std::vector<TagId>& ids) {
    std::wstring out;
    for (const auto& id : ids) {
        if (!out.empty()) out += L',';
        out += Escape(id);
    }
    return out;
}

std::wstring PathKey(const std::wstring& path) {
    std::wstring key = fs::NormalizePath(path);
    for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
    return key;
}

bool UnderFolder(const std::wstring& path_key, const std::wstring& folder_key) {
    if (folder_key.empty()) return true;
    if (path_key.size() <= folder_key.size()) return false;
    if (path_key.compare(0, folder_key.size(), folder_key) != 0) return false;
    return folder_key.back() == L'\\' || path_key[folder_key.size()] == L'\\';
}

void EraseId(std::vector<TagId>& ids, const TagId& id) {
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
}

} // namespace

bool TagQuery::Contains(const TagId& id) const {
    TagTermKind kind{};
    return KindOf(id, &kind);
}

bool TagQuery::KindOf(const TagId& id, TagTermKind* kind) const {
    const auto has = [&](const std::vector<TagId>& ids) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    TagTermKind found{};
    if (has(all)) found = TagTermKind::All;
    else if (has(any)) found = TagTermKind::Any;
    else if (has(none)) found = TagTermKind::Not;
    else return false;
    if (kind) *kind = found;
    return true;
}

void TagQuery::Set(const TagId& id, TagTermKind kind) {
    if (id.empty()) return;
    Remove(id);
    (kind == TagTermKind::All ? all : kind == TagTermKind::Any ? any : none).push_back(id);
}

bool TagQuery::Remove(const TagId& id) {
    const bool had = Contains(id);
    EraseId(all, id);
    EraseId(any, id);
    EraseId(none, id);
    return had;
}

std::vector<TagQueryTerm> TagQueryTerms(const TagQuery& query) {
    std::vector<TagQueryTerm> terms;
    for (const auto& id : query.all) terms.push_back({ id, TagTermKind::All });
    for (const auto& id : query.any) terms.push_back({ id, TagTermKind::Any });
    for (const auto& id : query.none) terms.push_back({ id, TagTermKind::Not });
    return terms;
}

std::wstring TagQueryFolderLabel(const std::wstring& folder) {
    std::wstring trimmed = folder;
    while (trimmed.size() > 1 && trimmed.back() == L'\\') trimmed.pop_back();
    const size_t slash = trimmed.find_last_of(L'\\');
    std::wstring name = slash == std::wstring::npos ? trimmed : trimmed.substr(slash + 1);
    return name.empty() ? folder : name;
}

TagQuery ParseTagQuery(const std::wstring& rest) {
    TagQuery query;
    for (const auto& part : Split(rest, L';')) {
        const size_t eq = part.find(L'=');
        if (eq == std::wstring::npos) continue;
        const std::wstring key = part.substr(0, eq);
        const std::wstring value = part.substr(eq + 1);
        if (key == L"in") {
            query.in = Unescape(value);
            continue;
        }
        std::vector<TagId>* ids = key == L"all" ? &query.all
                                : key == L"any" ? &query.any
                                : key == L"not" ? &query.none
                                                : nullptr;
        if (!ids) continue;
        for (const auto& raw : Split(value, L',')) {
            const TagId id = Unescape(raw);
            if (!id.empty() && !query.Contains(id)) ids->push_back(id);
        }
    }
    return query;
}

std::wstring MakeTagQueryPath(const TagQuery& query) {
    std::wstring rest;
    const auto add = [&](const wchar_t* key, const std::wstring& value) {
        if (value.empty()) return;
        if (!rest.empty()) rest += L';';
        rest += key;
        rest += L'=';
        rest += value;
    };
    add(L"all", JoinIds(query.all));
    add(L"any", JoinIds(query.any));
    add(L"not", JoinIds(query.none));
    add(L"in", Escape(query.in));
    return L"pulse:tags:" + rest;
}

bool TagQueryFromPath(const PlacesCatalog& places, const std::wstring& path, TagQuery* out) {
    std::wstring kind, rest;
    if (!ParsePulsePath(path, &kind, &rest) || !IsTagViewKind(kind)) return false;
    TagQuery query;
    if (kind == L"tag") {
        const TagId id = places.ResolveTagRef(rest);
        if (!id.empty()) query.all.push_back(id);
    } else {
        query = ParseTagQuery(rest);
    }
    if (out) *out = std::move(query);
    return true;
}

void PruneTagQuery(const PlacesCatalog& places, TagQuery& query) {
    const auto prune = [&](std::vector<TagId>& ids) {
        ids.erase(std::remove_if(ids.begin(), ids.end(),
                                 [&](const TagId& id) { return !places.FindTag(id); }),
                  ids.end());
    };
    prune(query.all);
    prune(query.any);
    prune(query.none);
}

bool TagQueryMatches(const PlacesCatalog& places, const TagQuery& query, const std::wstring& path) {
    const std::vector<int>* held = places.TagIndicesForPath(path);
    const auto has = [&](const TagId& id) {
        const int index = places.FindTagIndex(id);
        return held && index >= 0 && std::find(held->begin(), held->end(), index) != held->end();
    };
    for (const auto& id : query.all) {
        if (!has(id)) return false;
    }
    if (!query.any.empty() && std::none_of(query.any.begin(), query.any.end(), has)) return false;
    for (const auto& id : query.none) {
        if (has(id)) return false;
    }
    return query.in.empty() || UnderFolder(PathKey(path), PathKey(query.in));
}

std::vector<std::wstring> TagQueryPaths(const PlacesCatalog& places, const TagQuery& query) {
    // Candidates: the smallest "all" tag, else the "any" tags, else every
    // tagged item (a query of only "not" terms or only a folder).
    std::vector<const ColorTag*> sources;
    const ColorTag* smallest = nullptr;
    for (const auto& id : query.all) {
        const ColorTag* tag = places.FindTag(id);
        if (!tag) return {};   // a deleted tag matches nothing
        if (!smallest || tag->paths.size() < smallest->paths.size()) smallest = tag;
    }
    if (smallest) {
        sources.push_back(smallest);
    } else if (!query.any.empty()) {
        for (const auto& id : query.any) {
            if (const ColorTag* tag = places.FindTag(id)) sources.push_back(tag);
        }
    } else {
        for (const auto& tag : places.tags) sources.push_back(&tag);
    }
    std::vector<std::wstring> out;
    std::unordered_set<std::wstring> seen;
    for (const ColorTag* tag : sources) {
        for (const auto& path : tag->paths) {
            if (path.empty() || !seen.insert(PathKey(path)).second) continue;
            if (TagQueryMatches(places, query, path)) out.push_back(path);
        }
    }
    return out;
}

std::wstring TagQueryTitle(const PlacesCatalog& places, const TagQuery& query) {
    const auto name = [&](const TagId& id) {
        const ColorTag* tag = places.FindTag(id);
        return tag ? tag->name : std::wstring{};
    };
    std::wstring out;
    for (const auto& id : query.all) {
        if (!out.empty()) out += L" + ";
        out += name(id);
    }
    if (!query.any.empty()) {
        if (!out.empty()) out += L" + ";
        std::wstring any;
        for (const auto& id : query.any) {
            if (!any.empty()) any += L" | ";
            any += name(id);
        }
        out += query.any.size() > 1 && !query.all.empty() ? L"(" + any + L")" : any;
    }
    for (const auto& id : query.none) {
        out += out.empty() ? L"\x2212 " : L" \x2212 ";
        out += name(id);
    }
    if (!query.in.empty()) out += L" @ " + TagQueryFolderLabel(query.in);
    return out;
}

std::vector<std::pair<TagId, int>> TagQueryRefinements(const PlacesCatalog& places,
                                                        const TagQuery& query,
                                                        const std::vector<std::wstring>& paths,
                                                        size_t limit) {
    std::vector<int> counts(places.tags.size(), 0);
    for (const auto& path : paths) {
        if (const std::vector<int>* held = places.TagIndicesForPath(path)) {
            for (int index : *held) {
                if (index >= 0 && index < static_cast<int>(counts.size())) ++counts[static_cast<size_t>(index)];
            }
        }
    }
    std::vector<std::pair<TagId, int>> out;
    const int total = static_cast<int>(paths.size());
    for (size_t i = 0; i < counts.size(); ++i) {
        const auto& tag = places.tags[i];
        if (counts[i] == 0 || counts[i] >= total || query.Contains(tag.id)) continue;
        out.emplace_back(tag.id, counts[i]);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::vector<std::pair<std::wstring, int>> TagQueryLocations(const std::vector<std::wstring>& paths,
                                                            size_t limit) {
    std::unordered_map<std::wstring, size_t> slot;
    std::vector<std::pair<std::wstring, int>> out;
    for (const auto& path : paths) {
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash == 0) continue;
        std::wstring parent = path.substr(0, slash);
        if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
        const auto [it, added] = slot.try_emplace(PathKey(parent), out.size());
        if (added) out.emplace_back(std::move(parent), 0);
        ++out[it->second].second;
    }
    if (out.size() < 2) return {};
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

} // namespace pulse::app
