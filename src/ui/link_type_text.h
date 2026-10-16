#pragma once
#include "../fs/fs_enum.h"
#include "../common/localization.h"

namespace pulse::ui {

inline std::wstring LinkTypeText(fs::LinkKind kind) {
    if (kind == fs::LinkKind::SymbolicLink) return l10n::Get(l10n::StringId::SymbolicLink).c_str();
    if (kind == fs::LinkKind::Junction) return l10n::Get(l10n::StringId::Junction).c_str();
    return {};
}

} // namespace pulse::ui
