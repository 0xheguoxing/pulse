// tag_manager.h — 管理标签: the tag manager panel and the actions it hands back.
#pragma once
#include "app_runtime.h"

namespace pulse {
// Opens the 管理标签 panel at screen_pt (所有标签 ›, the 标签 section menu).
void ShowTagManager(AppState& s, POINT screen_pt);
// --tagmanagershot: the panel with sample tags, rendered to a PNG.
bool SaveTagManagerSnapshot(AppState& s, const wchar_t* png_path, bool dark);
}
