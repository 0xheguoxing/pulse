#pragma once

#include <windows.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::ops {

// What one listing of a folder says about a name in it.
struct ListedSource {
    DWORD attributes = 0;
    FILETIME created{};
    FILETIME accessed{};
    FILETIME modified{};
    ULONGLONG bytes = 0;
};

// Reads many sources from one listing of each folder they share. Asking the
// file system about one name costs tens of microseconds (more with an
// antivirus filter), while a listing returns each name for about one. Only
// names spelled exactly as listed are kept, so anything else is left to the
// caller's own query of that path.
class SourceListing {
public:
    // Fewer sources in a folder are cheaper to ask about one by one.
    static constexpr size_t kMinSourcesPerFolder = 16;

    explicit SourceListing(const std::vector<std::wstring>& sources) {
        if (sources.size() < kMinSourcesPerFolder) return;
        std::unordered_map<std::wstring, std::unordered_set<std::wstring>> wanted;
        for (const auto& source : sources) {
            const size_t slash = source.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash == 0 || slash + 1 == source.size() ||
                source[slash] != L'\\' || source[slash - 1] == L'\\')
                continue;
            wanted[source.substr(0, slash)].insert(source.substr(slash + 1));
        }
        for (const auto& [folder, names] : wanted) {
            if (names.size() < kMinSourcesPerFolder || folder.size() + 2 >= MAX_PATH) continue;
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &data,
                                           FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) continue;
            // A few sources picked from a huge folder: stop before the listing
            // costs more than asking about each of them would.
            size_t left = names.size();
            size_t budget = (std::max)(size_t{4096}, names.size() * 64);
            do {
                std::wstring name(data.cFileName);
                if (!names.contains(name)) continue;
                ListedSource listed;
                listed.attributes = data.dwFileAttributes;
                listed.created = data.ftCreationTime;
                listed.accessed = data.ftLastAccessTime;
                listed.modified = data.ftLastWriteTime;
                ULARGE_INTEGER size{};
                size.HighPart = data.nFileSizeHigh;
                size.LowPart = data.nFileSizeLow;
                listed.bytes = size.QuadPart;
                if (known_.try_emplace(folder + L"\\" + name, listed).second) --left;
            } while (left && --budget && FindNextFileW(find, &data));
            FindClose(find);
        }
    }

    // Null when the listing did not show this exact path.
    const ListedSource* Find(const std::wstring& source) const {
        const auto found = known_.find(source);
        return found == known_.end() ? nullptr : &found->second;
    }

private:
    std::unordered_map<std::wstring, ListedSource> known_;
};

// Which names a destination folder already has, from one listing, for a
// transfer of many items into it. Over a network each query of a name is a
// round trip; the listing returns hundreds of names per trip. Only absence
// is answered, and only for names made of ASCII and of characters without
// case, which every file system compares alike. Names are folded with the
// file-system upper-case rules (LCMapStringEx without linguistic casing), so
// a listed name that the file system takes for an ASCII spelling still
// matches it. A name that may be an 8.3 alias (a '~'), or that Win32
// would trim (a trailing dot or space), is left to the caller, and so is any
// name the listing shows: the caller confirms it, which also keeps
// case-sensitive folders right.
class TargetListing {
public:
    TargetListing(const std::wstring& folder, size_t items) {
        if (items < SourceListing::kMinSourcesPerFolder || folder.empty() || folder.size() + 2 >= MAX_PATH) return;
        std::wstring pattern = folder;
        if (pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return;
        // A huge folder receiving a few items: past this many names, asking
        // about each item is cheaper.
        size_t budget = (std::max)(size_t{4096}, items * 64);
        bool complete = false;
        for (;;) {
            names_.insert(Map(data.cFileName, LCMAP_UPPERCASE));
            if (--budget == 0) break;
            if (!FindNextFileW(find, &data)) {
                complete = GetLastError() == ERROR_NO_MORE_FILES;
                break;
            }
        }
        FindClose(find);
        if (!complete) names_.clear();
        valid_ = complete;
    }

    // True: `name` is surely not in the folder. False: ask the file system.
    bool SurelyAbsent(const std::wstring& name) const {
        if (!valid_ || name.empty() || name.back() == L'.' || name.back() == L' ' ||
            name.find(L'~') != std::wstring::npos)
            return false;
        const std::wstring upper = Map(name, LCMAP_UPPERCASE);
        const std::wstring lower = Map(name, LCMAP_LOWERCASE);
        if (upper.size() != name.size() || lower.size() != name.size()) return false;
        for (size_t i = 0; i < name.size(); ++i)
            if (name[i] >= 0x80 && (upper[i] != name[i] || lower[i] != name[i])) return false;
        return !names_.contains(upper);
    }

    // `name` has since arrived in the folder (this transfer put it there).
    void Added(const std::wstring& name) {
        if (valid_) names_.insert(Map(name, LCMAP_UPPERCASE));
    }

private:
    static std::wstring Map(const std::wstring& name, DWORD flags) {
        if (name.empty()) return {};
        std::wstring out(name.size(), L'\0');
        const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, flags, name.c_str(), static_cast<int>(name.size()),
                                       out.data(), static_cast<int>(out.size()), nullptr, nullptr, 0);
        // Unmappable: a key no query can equal, and a name no query trusts.
        if (size <= 0) return std::wstring(1, L'\0') + name;
        out.resize(static_cast<size_t>(size));
        return out;
    }

    bool valid_ = false;
    std::unordered_set<std::wstring> names_;
};

}  // namespace pulse::ops
