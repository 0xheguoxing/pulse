#pragma once
// cloud_roots.h - cloud folders Explorer shows at the top of its navigation
// pane, read the way Explorer learns about them:
//   * Desktop\NameSpace entries whose CLSID is a delegate folder pointing at a
//     real directory (WPS网盘, 百度网盘同步空间, OneDrive, Dropbox, ...), and
//   * Cloud Files sync roots registered under SyncRootManager for this user.
// Read-only: nothing here writes the registry or touches the folders' contents
// (a GetFileAttributes on the root is the only file-system call).
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

// Every location is a root + subkey so tests can point the reader at a private
// HKCU fixture instead of the machine's registry.
struct CloudRegistry {
    struct Key {
        HKEY root = nullptr;
        std::wstring path;
    };
    std::vector<Key> namespace_keys; // ...\Explorer\Desktop\NameSpace
    std::vector<Key> clsid_keys;     // ...\Classes\CLSID, searched in order
    std::vector<Key> nonenum_keys;   // ...\Policies\NonEnum ({CLSID} = 1 hides)
    Key sync_root_manager;           // ...\Explorer\SyncRootManager
    std::wstring user_sid;           // value name under <id>\UserSyncRoots

    static CloudRegistry System();
    // Test layout: <prefix>\NameSpace, <prefix>\CLSID, <prefix>\NonEnum and
    // <prefix>\SyncRootManager under HKEY_CURRENT_USER.
    static CloudRegistry Fixture(const std::wstring& hkcu_prefix, const std::wstring& sid);
};

enum class CloudRootSource : uint8_t { NavigationPane, SyncRoot, Seed };

struct CloudRoot {
    std::wstring path;  // absolute, no trailing backslash (except a drive root)
    std::wstring label; // what Explorer shows
    std::wstring id;    // {CLSID} or sync root id
    CloudRootSource source = CloudRootSource::NavigationPane;
    uint32_t sort_order = 0x42; // CLSID SortOrderIndex (navigation pane only)
};

std::vector<CloudRoot> ReadNavigationPaneCloudRoots(const CloudRegistry& registry);
std::vector<CloudRoot> ReadSyncRootCloudRoots(const CloudRegistry& registry);

// Appends the roots of `more` whose folder is not listed yet (case-insensitive
// path comparison) and for which `is_directory` holds. Order is kept.
void MergeCloudRoots(std::vector<CloudRoot>& into, std::vector<CloudRoot> more,
                     const std::function<bool(const std::wstring&)>& is_directory);

// Absolute path without a trailing separator; environment strings expanded.
std::wstring NormalizeCloudRootPath(const std::wstring& raw);
bool SameCloudRootPath(const std::wstring& a, const std::wstring& b);

// Cloud Files sync roots seen by the last sidebar refresh. Published from the
// refresh thread, read on the UI thread (the Details 状态 column).
void PublishCloudFilesRoots(std::vector<std::wstring> roots);
bool IsUnderCloudFilesRoot(const std::wstring& path);

// Every sidebar cloud root with its label, published by the same refresh;
// CloudRootLabelFor names the provider of a path (deepest root wins), empty
// outside all of them.
void PublishCloudRootLabels(const std::vector<CloudRoot>& roots);
std::wstring CloudRootLabelFor(const std::wstring& path);

// "S-1-5-21-..." of the process token, empty on failure.
std::wstring CurrentUserSidString();

// Brand tint for the sidebar glyph (0xRRGGBB).
uint32_t CloudRootColor(const CloudRoot& root);

} // namespace pulse::app
