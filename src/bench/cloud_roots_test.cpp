// cloud_roots_test.cpp - navigation-pane and sync-root cloud folders, read from
// a private HKCU fixture (HKCU\Software\PulseTest\cloud_roots_<pid>). Never
// writes Explorer's own keys; the fixture tree and temp folders are removed.
#include "../app/cloud_roots.h"
#include "../fs/cloud_state.h"
#include "../app/cloud_sync_probe.h"
#include <objbase.h>
#include <unordered_map>

#include <windows.h>
#include <cstdio>
#include <string>

using namespace pulse::app;

namespace {
bool g_passed = true;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    g_passed = g_passed && ok;
}

void SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value,
               DWORD type = REG_SZ) {
    HKEY h = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (!h) return;
    RegSetValueExW(h, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

void SetDword(const std::wstring& key, const wchar_t* name, DWORD value) {
    HKEY h = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (!h) return;
    RegSetValueExW(h, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(h);
}

void MakeKey(const std::wstring& key) {
    HKEY h = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &h, nullptr);
    if (h) RegCloseKey(h);
}

const CloudRoot* Find(const std::vector<CloudRoot>& roots, const std::wstring& id) {
    for (const auto& r : roots) if (_wcsicmp(r.id.c_str(), id.c_str()) == 0) return &r;
    return nullptr;
}
} // namespace

int main() {
    const std::wstring sid = L"S-1-5-21-pulse-test";
    const std::wstring prefix = L"Software\\PulseTest\\cloud_roots_" + std::to_wstring(GetCurrentProcessId());
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring base = NormalizeCloudRootPath(std::wstring(temp) + L"pulse_cloud_roots_" +
                                                     std::to_wstring(GetCurrentProcessId()));
    const std::wstring wps = base + L"\\WPSDrive";
    const std::wstring baidu = base + L"\\BaiduSyncdisk";
    const std::wstring od = base + L"\\OneDrive";
    const std::wstring gone = base + L"\\Gone";
    CreateDirectoryW(base.c_str(), nullptr);
    for (const auto* dir : {&wps, &baidu, &od}) CreateDirectoryW(dir->c_str(), nullptr);

    const std::wstring ns = prefix + L"\\NameSpace\\";
    const std::wstring cls = prefix + L"\\CLSID\\";
    // A: delegate folder, TargetFolderPath as REG_EXPAND_SZ, name in the default value.
    const std::wstring a = L"{7AE6DE87-C956-4B40-9C89-3D166C9841D3}";
    MakeKey(ns + a);
    SetString(cls + a, nullptr, L"WPS\x7F51\x76D8");
    SetDword(cls + a, L"System.IsPinnedToNameSpaceTree", 1);
    const std::wstring wps_expand = L"%PULSE_CLOUD_ROOTS_BASE%\\WPSDrive\\";
    SetEnvironmentVariableW(L"PULSE_CLOUD_ROOTS_BASE", base.c_str());
    SetString(cls + a + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", wps_expand, REG_EXPAND_SZ);
    // B: FolderShortcut spelling ("Target"), LocalizedString wins over the default value.
    const std::wstring b = L"{18701C81-34AB-498E-8C7F-5D26B9C36732}";
    MakeKey(ns + b);
    SetString(cls + b, nullptr, L"ignored default");
    SetDword(cls + b, L"System.IsPinnedToNameSpaceTree", 1);
    SetDword(cls + b, L"SortOrderIndex", 0x50);
    SetDword(cls + a, L"SortOrderIndex", 0x40);
    SetString(cls + b, L"LocalizedString", L"\x767E\x5EA6\x7F51\x76D8\x540C\x6B65\x7A7A\x95F4");
    SetString(cls + b + L"\\Instance\\InitPropertyBag", L"Target", baidu);
    // C: switched off in the tree.
    const std::wstring c = L"{00000000-0000-0000-0000-00000000000C}";
    MakeKey(ns + c);
    SetDword(cls + c, L"System.IsPinnedToNameSpaceTree", 0);
    SetString(cls + c + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", od);
    // D: a shell extension without a folder target (Libraries, Home, ...).
    const std::wstring d = L"{00000000-0000-0000-0000-00000000000D}";
    MakeKey(ns + d);
    SetString(cls + d, nullptr, L"Shell extension");
    // E: hidden by the NonEnum policy.
    const std::wstring e = L"{00000000-0000-0000-0000-00000000000E}";
    MakeKey(ns + e);
    SetDword(cls + e, L"System.IsPinnedToNameSpaceTree", 1);
    SetString(cls + e + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", od);
    SetDword(prefix + L"\\NonEnum", e.c_str(), 1);
    // F: listed in NameSpace, but no CLSID registration at all.
    MakeKey(ns + L"{00000000-0000-0000-0000-00000000000F}");
    // H: Windows' own entries - a folder target but no IsPinnedToNameSpaceTree.
    const std::wstring h = L"{00000000-0000-0000-0000-000000000008}";
    MakeKey(ns + h);
    SetString(cls + h, nullptr, L"Pictures");
    SetString(cls + h + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", od);
    // K: known-folder target (Documents, "This Device") is never followed.
    const std::wstring k = L"{00000000-0000-0000-0000-00000000000B}";
    MakeKey(ns + k);
    SetDword(cls + k, L"System.IsPinnedToNameSpaceTree", 1);
    SetString(cls + k + L"\\Instance\\InitPropertyBag", L"TargetKnownFolder",
              L"{FDD39AD0-238F-46AF-ADB4-6C85480369C7}");
    // G: points at a folder that no longer exists.
    const std::wstring g = L"{00000000-0000-0000-0000-000000000006}";
    MakeKey(ns + g);
    SetString(cls + g, nullptr, L"Old drive");
    SetDword(cls + g, L"System.IsPinnedToNameSpaceTree", 1);
    SetDword(cls + g, L"SortOrderIndex", 0x60);
    SetString(cls + g + L"\\Instance\\InitPropertyBag", L"TargetFolderPath", gone);

    const std::wstring srm = prefix + L"\\SyncRootManager\\";
    const std::wstring od_id = L"OneDrive!" + sid + L"!Personal";
    SetString(srm + od_id + L"\\UserSyncRoots", sid.c_str(), od + L"\\");
    const std::wstring other_id = L"Dropbox!S-1-5-21-someone-else!1";
    SetString(srm + other_id + L"\\UserSyncRoots", L"S-1-5-21-someone-else", base);
    const std::wstring dup_id = L"WPSCloud!" + sid + L"!1";
    SetString(srm + dup_id, L"DisplayNameResource", L"WPS Cloud");
    SetString(srm + dup_id + L"\\UserSyncRoots", sid.c_str(), wps + L"\\");
    MakeKey(srm + L"Empty!" + sid);

    const CloudRegistry registry = CloudRegistry::Fixture(prefix, sid);
    const auto pane = ReadNavigationPaneCloudRoots(registry);
    const CloudRoot* ra = Find(pane, a);
    Check(ra && ra->path == wps && ra->label == L"WPS\x7F51\x76D8",
          "TargetFolderPath (REG_EXPAND_SZ) resolves, default value names the row");
    const CloudRoot* rb = Find(pane, b);
    Check(rb && rb->path == baidu && rb->label == L"\x767E\x5EA6\x7F51\x76D8\x540C\x6B65\x7A7A\x95F4",
          "FolderShortcut Target and LocalizedString are honoured");
    Check(!Find(pane, c), "IsPinnedToNameSpaceTree = 0 hides the entry");
    Check(!Find(pane, d), "shell extensions without a folder target are skipped");
    Check(!Find(pane, e), "NonEnum policy hides the entry");
    Check(!Find(pane, L"{00000000-0000-0000-0000-00000000000F}"), "unregistered CLSID is skipped");
    Check(Find(pane, g) != nullptr, "reader itself does not touch the file system");
    Check(!Find(pane, h), "entries without IsPinnedToNameSpaceTree (Windows' own) are skipped");
    Check(!Find(pane, k), "TargetKnownFolder entries are skipped");
    Check(pane.size() == 3 && pane[0].id == a && pane[1].id == b,
          "navigation pane order follows SortOrderIndex");

    const auto sync = ReadSyncRootCloudRoots(registry);
    const CloudRoot* rod = Find(sync, od_id);
    Check(rod && rod->path == od && rod->label == L"OneDrive",
          "sync root for this SID, provider prefix as fallback label");
    Check(!Find(sync, other_id), "another user's sync root is ignored");
    Check(!Find(sync, L"Empty!" + sid), "sync root without UserSyncRoots is ignored");
    const CloudRoot* rdup = Find(sync, dup_id);
    Check(rdup && rdup->label == L"WPS Cloud", "DisplayNameResource names the sync root");
    Check(ReadSyncRootCloudRoots(CloudRegistry::Fixture(prefix, L"")).empty(),
          "no SID, no sync roots");

    std::vector<CloudRoot> merged;
    merged.push_back({od, L"SS - Personal", {}, CloudRootSource::Seed}); // OneDrive account row
    const auto is_dir = [](const std::wstring& p) {
        const DWORD attrs = GetFileAttributesW(p.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    };
    MergeCloudRoots(merged, pane, is_dir);
    MergeCloudRoots(merged, sync, is_dir);
    Check(merged.size() == 3 && merged[0].source == CloudRootSource::Seed &&
              merged[1].id == a && merged[2].id == b,
          "merge keeps order, drops duplicates (case, trailing slash) and missing folders");

    Check(NormalizeCloudRootPath(L"C:\\Foo\\\\") == L"C:\\Foo", "trailing separators trimmed");
    Check(NormalizeCloudRootPath(L"D:\\") == L"D:\\", "drive root keeps its separator");
    Check(NormalizeCloudRootPath(L"D:/Baidu/Sync") == L"D:\\Baidu\\Sync", "forward slashes normalized");
    Check(SameCloudRootPath(L"c:\\users\\ss\\WPSDrive\\", L"C:\\Users\\SS\\wpsdrive"),
          "path comparison is case-insensitive");
    Check(NormalizeCloudRootPath(L"").empty(), "empty path stays empty");
    Check(CurrentUserSidString().rfind(L"S-1-", 0) == 0, "process SID string");
    Check(CloudRootColor({L"", L"OneDrive - Personal", L"", CloudRootSource::Seed}) == 0x2B88D8 &&
              CloudRootColor({L"", L"WPS\x7F51\x76D8", a, CloudRootSource::NavigationPane}) == 0x2F6FD6 &&
              CloudRootColor({L"", L"\x767E\x5EA6\x7F51\x76D8", b, CloudRootSource::NavigationPane}) == 0x2F8CF0 &&
              CloudRootColor({L"", L"Other", L"x", CloudRootSource::SyncRoot}) == 0x60A5FA,
          "brand tints");

    using pulse::fs::CloudState;
    using pulse::fs::CloudStateFromAttrs;
    Check(CloudStateFromAttrs(FILE_ATTRIBUTE_ARCHIVE) == CloudState::Local &&
              CloudStateFromAttrs(pulse::fs::kAttrUnpinned) == CloudState::Local &&
              CloudStateFromAttrs(pulse::fs::kAttrRecallOnDataAccess) == CloudState::Online &&
              CloudStateFromAttrs(FILE_ATTRIBUTE_DIRECTORY | pulse::fs::kAttrRecallOnOpen) == CloudState::Online &&
              CloudStateFromAttrs(FILE_ATTRIBUTE_OFFLINE) == CloudState::Online &&
              CloudStateFromAttrs(pulse::fs::kAttrPinned) == CloudState::Pinned &&
              CloudStateFromAttrs(pulse::fs::kAttrPinned | pulse::fs::kAttrRecallOnDataAccess) == CloudState::Pinned,
          "cloud state from attributes");
    Check(!IsUnderCloudFilesRoot(L"C:\\Users\\SS\\OneDrive"), "nothing published yet");
    PublishCloudFilesRoots({L"C:\\Users\\SS\\OneDrive\\", L"", L"D:\\"});
    Check(IsUnderCloudFilesRoot(L"C:\\Users\\SS\\OneDrive") &&
              IsUnderCloudFilesRoot(L"c:\\users\\ss\\onedrive\\Docs\\a") &&
              !IsUnderCloudFilesRoot(L"C:\\Users\\SS\\OneDriveOld") &&
              !IsUnderCloudFilesRoot(L"C:\\Users\\SS") && IsUnderCloudFilesRoot(L"D:\\x") &&
              IsUnderCloudFilesRoot(L"\\\\?\\C:\\Users\\SS\\OneDrive\\Docs") &&
              !IsUnderCloudFilesRoot(L"\\\\?\\C:\\Users\\SS\\OneDriveOld"),
          "cloud files root prefix matching respects folder boundaries");
    PublishCloudFilesRoots({L"\\\\nas\\share\\Sync"});
    Check(IsUnderCloudFilesRoot(L"\\\\?\\UNC\\nas\\share\\Sync\\a") &&
              IsUnderCloudFilesRoot(L"\\\\nas\\share\\Sync"),
          "long-path and UNC prefixes match the same root");
    PublishCloudFilesRoots({});
    Check(!IsUnderCloudFilesRoot(L"D:\\x"), "republishing replaces the set");

    using pulse::fs::CloudSync;
    using pulse::fs::CloudSyncFromProviderState;
    Check(CloudSyncFromProviderState(0) == CloudSync::Idle && CloudSyncFromProviderState(2) == CloudSync::Idle &&
              CloudSyncFromProviderState(3) == CloudSync::Idle && CloudSyncFromProviderState(9) == CloudSync::Idle &&
              CloudSyncFromProviderState(4) == CloudSync::Syncing && CloudSyncFromProviderState(5) == CloudSync::Syncing &&
              CloudSyncFromProviderState(6) == CloudSync::Syncing && CloudSyncFromProviderState(10) == CloudSync::Syncing &&
              CloudSyncFromProviderState(7) == CloudSync::Error && CloudSyncFromProviderState(8) == CloudSync::Error,
          "storage provider states map to idle / syncing / error");
    {
        using pulse::app::CloudSyncProbe;
        std::unordered_map<std::wstring, uint32_t> fake{{L"a", 6}, {L"b", 7}, {L"c", 2}};
        int calls = 0;
        CloudSyncProbe probe([&](const std::wstring& p) {
            ++calls;
            const auto it = fake.find(p);
            return it == fake.end() ? 0u : it->second;
        }, CloudSyncProbe::Mode::Manual);
        const ULONGLONG t0 = GetTickCount64();
        Check(probe.Lookup(L"a", 1, t0) == CloudSync::Idle && probe.Lookup(L"b", 1, t0) == CloudSync::Idle &&
                  probe.Lookup(L"c", 1, t0) == CloudSync::Idle && probe.QueuedForTest() == 3,
              "probe: unknown rows answer idle and are queued");
        probe.Lookup(L"a", 1, t0);
        Check(probe.QueuedForTest() == 3 && probe.Lookup(L"", 0, t0) == CloudSync::Idle && probe.QueuedForTest() == 3,
              "probe: a queued row is not queued twice, empty paths are ignored");
        const uint64_t r0 = probe.Revision();
        Check(probe.RunPendingForTest() == 3 && calls == 3, "probe: each pending row is read once");
        Check(probe.Lookup(L"a", 1, t0) == CloudSync::Syncing && probe.Lookup(L"b", 1, t0) == CloudSync::Error &&
                  probe.Lookup(L"c", 1, t0) == CloudSync::Idle && probe.Revision() != r0 && probe.QueuedForTest() == 0,
              "probe: answers are cached and move the revision");
        const ULONGLONG later = GetTickCount64() + CloudSyncProbe::kSyncingRecheckMs + 10;
        probe.Lookup(L"a", 1, later);
        probe.Lookup(L"b", 1, later);
        probe.Lookup(L"c", 1, later);
        Check(probe.QueuedForTest() == 1, "probe: only syncing rows are re-read on a timer");
        fake[L"a"] = 2;
        const uint64_t r1 = probe.Revision();
        probe.RunPendingForTest();
        Check(probe.Lookup(L"a", 1, later) == CloudSync::Idle && probe.Revision() != r1,
              "probe: a finished transfer settles and repaints");
        probe.Lookup(L"c", 2, later);
        Check(probe.QueuedForTest() == 1, "probe: a modified row is read again");
        const uint64_t r2 = probe.Revision();
        probe.RunPendingForTest();
        Check(probe.Revision() == r2 && calls == 5, "probe: an unchanged answer does not repaint");
    }
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        wchar_t windir[MAX_PATH]{};
        GetWindowsDirectoryW(windir, MAX_PATH);
        Check(CloudSyncFromProviderState(pulse::app::QueryStorageProviderState(windir)) == CloudSync::Idle &&
                  pulse::app::QueryStorageProviderState(L"Z:\\no\\such\\file") == 0,
              "real query: a local folder is idle, a missing path reads as none");
        if (SUCCEEDED(com)) CoUninitialize();
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, prefix.c_str());
    for (const auto* dir : {&wps, &baidu, &od}) RemoveDirectoryW(dir->c_str());
    RemoveDirectoryW(base.c_str());
    HKEY left = nullptr;
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, prefix.c_str(), 0, KEY_READ, &left) != ERROR_SUCCESS,
          "fixture registry tree removed");
    if (left) RegCloseKey(left);
    return g_passed ? 0 : 1;
}
