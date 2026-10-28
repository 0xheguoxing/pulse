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
    std::vector<Key> mycomputer_keys; // ...\Explorer\MyComputer\NameSpace
    std::vector<Key> clsid_keys;     // ...\Classes\CLSID, searched in order
    std::vector<Key> nonenum_keys;   // ...\Policies\NonEnum ({CLSID} = 1 hides)
    Key sync_root_manager;           // ...\Explorer\SyncRootManager
    std::wstring user_sid;           // value name under <id>\UserSyncRoots

    static CloudRegistry System();
    // Test layout: <prefix>\NameSpace, <prefix>\MyComputer, <prefix>\CLSID,
    // <prefix>\NonEnum and <prefix>\SyncRootManager under HKEY_CURRENT_USER.
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

// Program icons Windows shows in This PC (百度网盘, WPS网盘, ...): MyComputer\
// NameSpace entries that come from a program, i.e. their InprocServer32 lives
// outside the Windows directory, or that name a real folder (TargetFolderPath).
// Windows' own entries (user folders, 3D Objects, ...) and NonEnum-hidden ones
// are left out. Read-only, like the rest of this file.
struct ThisPcLauncher {
    std::wstring clsid;  // "{...}"
    std::wstring label;  // what Explorer shows
    std::wstring target; // existing folder from TargetFolderPath, else empty
    uint8_t brand = 1;   // fs::DirEntry::pc_launcher: 1 other, 2 Baidu, 3 WPS
};
std::vector<ThisPcLauncher> ReadThisPcLaunchers(const CloudRegistry& registry);
// The machine's launchers; a test instance reads its PULSE_TEST_CLOUD_REG fixture.
std::vector<ThisPcLauncher> CurrentThisPcLaunchers();
// full_path of a launcher row: this prefix + its CLSID.
constexpr wchar_t kThisPcLauncherPrefix[] = L"shell:::";

} // namespace pulse::app
