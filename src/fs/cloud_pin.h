#pragma once
// cloud_pin.h - 始终保留在此设备上 / 释放空间 for Cloud Files items, the way
// Explorer does it: only the PINNED / UNPINNED attributes change and the sync
// engine (OneDrive, WPS网盘, ...) downloads or dehydrates on its own. Nothing is
// ever deleted, moved or opened for data, so a file can never be lost here.
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace pulse::fs {

enum class CloudPinAction : unsigned char {
    Keep, // 始终保留在此设备上: set PINNED, clear UNPINNED
    Free, // 释放空间: set UNPINNED, clear PINNED
};

// The attributes to write back for an item that has |attrs| now: the pin pair
// swapped for |action|, everything SetFileAttributesW cannot take masked off
// (directory, reparse, sparse, compressed, encrypted, recall bits), and
// FILE_ATTRIBUTE_NORMAL when nothing is left.
DWORD CloudPinAttrs(DWORD attrs, CloudPinAction action) noexcept;

// Whether |action| would change anything for an item with |attrs|.
bool CloudPinChanges(DWORD attrs, CloudPinAction action) noexcept;

// Reparse tags owned by the Cloud Files filter (IO_REPARSE_TAG_CLOUD_0..F).
constexpr bool IsCloudReparseTag(DWORD tag) noexcept { return (tag & 0xFFFF0FFFu) == 0x9000001Au; }

struct CloudPinResult {
    size_t visited = 0; // items looked at
    size_t changed = 0; // items whose attributes were written
    size_t failed = 0;  // SetFileAttributesW refused (in use, access denied, gone)
    bool cancelled = false;
};

// Applies |action| to every path and, for folders, everything below them.
// Parents are written before their children; junctions, symlinks and other
// non-cloud reparse points are left alone and never entered; 释放空间 does not
// enter folders that are still online-only placeholders (nothing local to free,
// and listing them would make the provider fetch the listing).
CloudPinResult ApplyCloudPin(const std::vector<std::wstring>& paths, CloudPinAction action,
                             const std::atomic<bool>* cancel = nullptr);

} // namespace pulse::fs
