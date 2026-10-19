#include "preview_file_utils.h"
#include "../common/path_utils.h"
#include <cwctype>

namespace pulse::preview {

namespace {

constexpr DWORD kRecallOnOpen = 0x00040000; // FILE_ATTRIBUTE_RECALL_ON_OPEN
constexpr DWORD kRecallOnData = 0x00400000; // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS

} // namespace

bool IsOfflinePlaceholder(DWORD attrs) {
    // INVALID_FILE_ATTRIBUTES means no attributes (a path that is gone, or one
    // the caller could not query in time), not a set of placeholder bits. The
    // decoders then open the path themselves, and a vanished one is reported
    // as path-unavailable instead of offline-placeholder.
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & (FILE_ATTRIBUTE_OFFLINE | kRecallOnOpen | kRecallOnData)) != 0;
}

std::wstring ExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring extension = path.substr(dot);
    for (wchar_t& c : extension) c = static_cast<wchar_t>(std::towlower(c));
    return extension;
}

std::wstring ShellPath(const std::wstring& path) {
    return pulse::path::StripExtendedPathPrefix(path);
}

} // namespace pulse::preview
