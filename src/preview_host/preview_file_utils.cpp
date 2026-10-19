#include "preview_file_utils.h"
#include "../common/file_placeholder.h"
#include "../common/path_utils.h"
#include <cwctype>

namespace pulse::preview {

bool IsOfflinePlaceholder(DWORD attrs) {
    // Previews never recall (common/file_placeholder.h). A vanished path
    // (INVALID_FILE_ATTRIBUTES) is left to the decoders, which then report
    // path-unavailable instead of offline-placeholder.
    return files::PreviewWouldRecall(attrs);
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
