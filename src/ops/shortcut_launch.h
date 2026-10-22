// shortcut_launch.h - Items dropped on a program shortcut start it the way the
// shortcut would (Explorer's behaviour): its own arguments first, then the
// dropped items; its start-in folder; run as administrator when it says so.
#pragma once
#include <string>
#include <string_view>

namespace pulse::ops {

struct ShortcutLaunch {
    std::wstring arguments;  // environment variables expanded
    std::wstring directory;  // start-in folder, expanded; empty when unset
    bool run_as = false;     // 以管理员身份运行 (SLDF_RUNAS_USER)
};

// Reads `lnk_path` (no target tracking, no UI). The caller owns COM.
bool ReadShortcutLaunch(const std::wstring& lnk_path, ShortcutLaunch& out);

// "<shortcut arguments> <dropped arguments>", either part may be empty.
std::wstring CombineShortcutArguments(std::wstring_view own, std::wstring_view dropped);

} // namespace pulse::ops
