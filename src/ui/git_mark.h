#pragma once
// Git working-tree state of one list row (in-app Git status). The app fills
// PaneViewModel::git_marks; the list tints the name and, in Details, shows a
// pill in the badge slot. Draw and hit-test share GitMarkText for its width.
#include "../common/localization.h"
#include <d2d1.h>
#include <cstdint>
#include <string>

namespace pulse::ui {

// Locked: SVN only (a lock this working copy holds).
enum class GitMark : uint8_t { None = 0, Added, Renamed, Modified, Deleted, Conflict, Locked };

inline const std::wstring& GitMarkText(GitMark mark) {
    using l10n::StringId;
    switch (mark) {
    case GitMark::Added: return l10n::Get(StringId::GitMarkAdded);
    case GitMark::Renamed: return l10n::Get(StringId::GitMarkRenamed);
    case GitMark::Modified: return l10n::Get(StringId::GitMarkModified);
    case GitMark::Deleted: return l10n::Get(StringId::GitMarkDeleted);
    case GitMark::Conflict: return l10n::Get(StringId::GitMarkConflict);
    case GitMark::Locked: return l10n::Get(StringId::GitMarkLocked);
    default: break;
    }
    static const std::wstring kNone;
    return kNone;
}

// GitHub's light palette / VS Code's dark one: readable on either surface.
inline D2D1_COLOR_F GitMarkColor(GitMark mark, bool dark) {
    uint32_t rgb = 0;
    switch (mark) {
    case GitMark::Added: rgb = dark ? 0x73C991u : 0x1A7F37u; break;
    case GitMark::Renamed: rgb = dark ? 0x5EC4D8u : 0x0E7490u; break;
    case GitMark::Modified: rgb = dark ? 0xE2C08Du : 0x9A6700u; break;
    case GitMark::Deleted: rgb = dark ? 0xF0827Au : 0xB42318u; break;
    case GitMark::Conflict: rgb = dark ? 0xFF7B72u : 0xCF222Eu; break;
    case GitMark::Locked: rgb = dark ? 0xB5B9C4u : 0x5F6470u; break;
    default: return D2D1::ColorF(0, 0.0f);
    }
    return D2D1::ColorF(rgb, 1.0f);
}

}  // namespace pulse::ui
