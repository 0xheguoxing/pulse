#pragma once
#include <cwctype>
#include <string>
#include <string_view>

namespace pulse {

// Why a name cannot be used, so a failed rename can say so.
enum class RenameFilenameIssue { None, Empty, InvalidCharacter, TrailingDotOrSpace, Reserved };

inline RenameFilenameIssue RenameFilenameProblem(const std::wstring& name) {
    if (name.empty()) return RenameFilenameIssue::Empty;
    for (const auto c : name)
        if (c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos)
            return RenameFilenameIssue::InvalidCharacter;
    // Also covers "." and "..".
    if (name.back() == L'.' || name.back() == L' ') return RenameFilenameIssue::TrailingDotOrSpace;
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    for (auto& c : stem) c = static_cast<wchar_t>(towupper(c));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
        stem == L"CONIN$" || stem == L"CONOUT$") return RenameFilenameIssue::Reserved;
    if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
        ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3'))
        return RenameFilenameIssue::Reserved;
    return RenameFilenameIssue::None;
}

inline bool IsRenameFilename(const std::wstring& name) {
    return RenameFilenameProblem(name) == RenameFilenameIssue::None;
}

} // namespace pulse
