#pragma once
// Drives that behave like network storage although Windows reports them as
// local: cloud drives mounted through a user-mode file system (WinFsp, Dokan,
// CBFS: Baidu's virtual disk, CloudDrive, rclone, Google Drive, ...). Their
// "disk" is a remote service, so Pulse treats them like mapped network
// drives: no index, no folder sizes, no recycle-bin scan, the remote copy
// strategy, and "Network drive" as the This PC type.
//
// Header-only on purpose: the app, the index service, the content indexer
// and the tests each compile their own source lists.
#include <windows.h>
#include <string_view>

namespace pulse::fs {

// QueryDosDevice target of a drive letter. User-mode file systems create
// \Device\Volume{GUID} disk devices; real partitions, VHDs and BitLocker
// volumes are \Device\HarddiskVolumeN, subst drives \??\C:\..., RAM disks and
// encrypted containers use their own driver names.
inline bool IsUserModeVolumeDevice(std::wstring_view target) {
    constexpr std::wstring_view kPrefix = L"\\Device\\Volume{";
    return target.size() > kPrefix.size() &&
           CompareStringOrdinal(target.data(), static_cast<int>(kPrefix.size()), kPrefix.data(),
                                static_cast<int>(kPrefix.size()), TRUE) == CSTR_EQUAL;
}

// File system names (GetVolumeInformationW) of user-mode / cloud mounts.
// Only for callers that already read the volume information: asking a stuck
// mount for it can block, the device test above cannot.
inline bool IsRemoteLikeFileSystemName(std::wstring_view name) {
    constexpr std::wstring_view kNames[] = {
        L"FUSE", L"WinFsp", L"Dokan", L"CloudDrive", L"rclone", L"Baidu", L"CBFS",
    };
    for (const auto needle : kNames) {
        if (name.size() < needle.size()) continue;
        for (size_t at = 0; at + needle.size() <= name.size(); ++at)
            if (CompareStringOrdinal(name.data() + at, static_cast<int>(needle.size()), needle.data(),
                                     static_cast<int>(needle.size()), TRUE) == CSTR_EQUAL)
                return true;
    }
    return false;
}

// Drive letter of "X:...", "\\?\X:..." or "\\.\X:..."; 0 for anything else
// (UNC paths are network paths in their own right).
inline wchar_t DriveLetterOf(std::wstring_view path) {
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' && (path[2] == L'?' || path[2] == L'.') &&
        path[3] == L'\\')
        path.remove_prefix(4);
    if (path.size() < 2 || path[1] != L':') return 0;
    const wchar_t c = path[0];
    if (c >= L'a' && c <= L'z') return static_cast<wchar_t>(c - L'a' + L'A');
    return c >= L'A' && c <= L'Z' ? c : 0;
}

namespace detail {
// --test-instance only: PULSE_TEST_REMOTE_LIKE_DRIVES=XY marks letters as
// user-mode mounts, so shots and tests need no real WinFsp drive.
inline bool TestMarksRemoteLike(wchar_t letter) {
    if (!GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0)) return false;
    wchar_t letters[32]{};
    const DWORD n = GetEnvironmentVariableW(L"PULSE_TEST_REMOTE_LIKE_DRIVES", letters, ARRAYSIZE(letters));
    if (!n || n >= ARRAYSIZE(letters)) return false;
    for (DWORD i = 0; i < n; ++i) {
        wchar_t c = letters[i];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        if (c == letter) return true;
    }
    return false;
}
} // namespace detail

// True for mapped network drives and user-mode (cloud) mounts. Costs a
// GetDriveTypeW and a QueryDosDeviceW: object-manager lookups, no media
// access, safe on a drive whose service hangs.
inline bool IsRemoteLikeDrive(std::wstring_view path) {
    const wchar_t letter = DriveLetterOf(path);
    if (!letter) return false;
    const wchar_t root[4] = {letter, L':', L'\\', L'\0'};
    const UINT type = GetDriveTypeW(root);
    if (type == DRIVE_REMOTE) return true;
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return false;
    if (detail::TestMarksRemoteLike(letter)) return true;
    const wchar_t device[3] = {letter, L':', L'\0'};
    wchar_t target[512]{};
    if (!QueryDosDeviceW(device, target, ARRAYSIZE(target))) return false;
    return IsUserModeVolumeDevice(target);  // first string = current mapping
}

} // namespace pulse::fs
