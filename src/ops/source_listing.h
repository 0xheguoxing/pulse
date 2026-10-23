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

}  // namespace pulse::ops
