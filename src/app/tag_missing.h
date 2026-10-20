// tag_missing.h — 找不到的文件 (pulse_tag_redesign P2): tagged items that are
// gone, and the two ways out: take them off the tag, or say where they went.
#pragma once
#include "app_runtime.h"

#include <string>
#include <vector>

namespace pulse {
// The tag's paths that are gone from a local drive that is there. A share, a
// drive that is not connected or a removable disk that is out does not count
// (those come back). Answers are cached for 30 s.
std::vector<std::wstring> MissingTaggedPaths(const app::ColorTag& tag);
// The tag menu's 找不到的文件 (N) ▸ 从标签中移除 / 重新定位…; nothing when none
// is missing.
void AppendMissingTagMenu(const app::ColorTag& tag, std::vector<ui::FluentMenuItem>& items,
                          int clean_command, int relocate_command);
// Takes the missing items off these tags (the snack undoes).
void CleanMissingTagged(AppState& s, const std::vector<std::wstring>& tag_ids);
// Asks where the tag's missing items are now: the deepest folder holding them
// moved there (RemapPaths, so stars and recents follow), or else each file
// sits in it directly.
void RelocateMissingTagged(AppState& s, const std::wstring& tag_id);
}
