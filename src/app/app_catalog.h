#pragma once

// Start menu apps for the quick search popup (#58-1, as Listary offers):
// every entry of shell:AppsFolder — desktop shortcuts and packaged apps
// alike — with the name the Start menu shows and a path that launches it
// ("shell:AppsFolder\<parsing name>"). The list loads on a worker thread;
// matching is pure and cheap enough for the UI thread (a few hundred names).

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app {

inline constexpr wchar_t kAppsFolderPrefix[] = L"shell:AppsFolder\\";

struct AppEntry {
    std::wstring name;  // as the Start menu shows it
    std::wstring path;  // shell:AppsFolder\<parsing name>
};

inline bool IsAppPath(std::wstring_view path) {
    constexpr std::wstring_view prefix = kAppsFolderPrefix;
    return path.size() > prefix.size() &&
        CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()), prefix.data(),
                             static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

inline std::wstring AppPath(std::wstring_view id) { return std::wstring(kAppsFolderPrefix) + std::wstring(id); }

// Entries a user does not launch from search: uninstallers, readmes, help
// files and websites that installers drop into the Start menu.
bool IsAppNoise(const AppEntry& app);
// The opposite of IsAppNoise for an AppsFolder parsing name `id`.
inline bool KeepApp(const std::wstring& name, const std::wstring& id) { return !IsAppNoise({name, AppPath(id)}); }

// Indexes of the apps matching `query`, best first, at most `limit`: the
// whole name, then a name prefix, a word prefix ("code" in "Visual Studio
// Code"), word initials ("vsc"), pinyin (full or initials, when `pinyin`),
// and last any substring. Every word of a multi-word query must match.
// Ties go to the shorter, then alphabetically first name.
std::vector<size_t> MatchApps(const std::vector<AppEntry>& apps, std::wstring_view query,
                              bool pinyin, size_t limit);

// Enumerates shell:AppsFolder, without noise, one entry per name. Blocking
// (COM and the shell's app list): call it off the UI thread.
std::vector<AppEntry> LoadApps();

// The cached list behind the popup. Refresh starts a background load unless
// one is running or the list is younger than `max_age_ms`; `notify` then
// gets `message` once it lands. The worker owns its state, so it may outlive
// the catalog safely.
class AppCatalog {
public:
    AppCatalog();
    ~AppCatalog();
    AppCatalog(const AppCatalog&) = delete;
    AppCatalog& operator=(const AppCatalog&) = delete;

    void Refresh(HWND notify, UINT message, uint64_t max_age_ms = 5ull * 60 * 1000);
    // The current list; empty until the first load lands.
    std::shared_ptr<const std::vector<AppEntry>> Apps() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::app
