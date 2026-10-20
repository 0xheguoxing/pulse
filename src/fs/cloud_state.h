#pragma once
// cloud_state.h - what the Cloud Files attributes on a listed item say about
// where its content lives. Pure attribute decoding: no I/O, so asking can never
// make a sync engine download anything.
#include <windows.h>
#include <cstdint>

namespace pulse::fs {

// Older SDKs lack some of these names; the values are fixed by the file system.
inline constexpr DWORD kAttrRecallOnOpen = 0x00040000;       // FILE_ATTRIBUTE_RECALL_ON_OPEN
inline constexpr DWORD kAttrPinned = 0x00080000;             // FILE_ATTRIBUTE_PINNED
inline constexpr DWORD kAttrUnpinned = 0x00100000;           // FILE_ATTRIBUTE_UNPINNED
inline constexpr DWORD kAttrRecallOnDataAccess = 0x00400000; // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS

enum class CloudState : uint8_t {
    Local,  // content present, may be freed again
    Online, // placeholder: opening it downloads
    Pinned, // kept on this device
};

// Any of these: content (or, for folders, the listing) is not on this device,
// and reading it would make the sync engine download it. The one definition
// every reader (thumbnails, previews, properties, content index, folder
// thumbnails, hover preview) checks before touching file content.
inline constexpr DWORD kCloudPlaceholderAttrs = FILE_ATTRIBUTE_OFFLINE | kAttrRecallOnOpen | kAttrRecallOnDataAccess;

constexpr bool IsCloudPlaceholder(DWORD attrs) noexcept {
    return (attrs & kCloudPlaceholderAttrs) != 0;
}

constexpr CloudState CloudStateFromAttrs(DWORD attrs) noexcept {
    if (attrs & kAttrPinned) return CloudState::Pinned;
    if (IsCloudPlaceholder(attrs)) return CloudState::Online;
    return CloudState::Local;
}

// What the sync engine reports for an item (System.StorageProviderState,
// STORAGEPROVIDERSTATE_* values). Only transfers and problems matter here;
// where the content lives comes from the attributes above.
enum class CloudSync : uint8_t { Idle, Syncing, Error };

constexpr CloudSync CloudSyncFromProviderState(uint32_t state) noexcept {
    switch (state) {
    case 4:  // PENDING_UPLOAD
    case 5:  // PENDING_DOWNLOAD
    case 6:  // TRANSFERRING
    case 10: // PENDING_UNSPECIFIED
        return CloudSync::Syncing;
    case 7:  // ERROR
    case 8:  // WARNING
        return CloudSync::Error;
    default: // NONE, SPARSE, IN_SYNC, PINNED, EXCLUDED
        return CloudSync::Idle;
    }
}

} // namespace pulse::fs
