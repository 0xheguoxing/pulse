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

// Content (or, for folders, the listing) is not on this device yet.
constexpr bool IsCloudPlaceholder(DWORD attrs) noexcept {
    return (attrs & (FILE_ATTRIBUTE_OFFLINE | kAttrRecallOnOpen | kAttrRecallOnDataAccess)) != 0;
}

constexpr CloudState CloudStateFromAttrs(DWORD attrs) noexcept {
    if (attrs & kAttrPinned) return CloudState::Pinned;
    if (IsCloudPlaceholder(attrs)) return CloudState::Online;
    return CloudState::Local;
}

} // namespace pulse::fs
