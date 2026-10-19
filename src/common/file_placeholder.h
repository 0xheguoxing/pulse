// file_placeholder.h - files a reader must leave unopened: offline (HSM)
// files and cloud placeholders, whose data is recalled (downloaded) on read.
// One policy per kind of reader; every reader uses one of these two.
#pragma once

#include <windows.h>

namespace pulse::files {

constexpr DWORD kAttributeRecallOnOpen = 0x00040000;   // FILE_ATTRIBUTE_RECALL_ON_OPEN
constexpr DWORD kAttributePinned = 0x00080000;         // FILE_ATTRIBUTE_PINNED
constexpr DWORD kAttributeRecallOnData = 0x00400000;   // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS

// Previews (the preview host, Quick Look, Shell preview handlers) never
// recall: an offline file or a placeholder stays unread, even one the user
// pinned whose data has not arrived yet. INVALID_FILE_ATTRIBUTES means no
// attributes (a vanished path), not a set of placeholder bits.
inline bool PreviewWouldRecall(DWORD attrs) noexcept {
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & (FILE_ATTRIBUTE_OFFLINE | kAttributeRecallOnOpen | kAttributeRecallOnData)) != 0;
}

// Content indexing and search read a pinned placeholder: the user asked for
// it to be on this device, so its recall is wanted.
inline bool IsUnpinnedPlaceholder(DWORD attrs) noexcept {
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & (kAttributeRecallOnOpen | kAttributeRecallOnData)) != 0 && (attrs & kAttributePinned) == 0;
}

} // namespace pulse::files
