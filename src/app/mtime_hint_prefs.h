#pragma once
// AppPrefs -> the list's 修改时间提示 settings (ui/mtime_hint.h).
#include "app_prefs.h"
#include "../ui/mtime_hint.h"

namespace pulse::app {

inline ui::MtimeHint MtimeHintFromPrefs(const AppPrefs& p) {
    ui::MtimeHint hint;
    hint.style = p.list_mtime_hint;
    hint.range_days = ui::ClampMtimeRange(p.list_mtime_range);
    hint.palette = p.list_mtime_palette;
    hint.fade = p.list_mtime_fade;
    // Folder times inside come from change tracking; without it a folder has its own time only.
    hint.tree = p.list_mtime_tree && p.change_tracking_enabled;
    hint.other_views = p.list_mtime_other_views;
    hint.bold = p.list_mtime_bold;
    hint.flash = p.list_mtime_flash;
    return hint;
}

}  // namespace pulse::app
