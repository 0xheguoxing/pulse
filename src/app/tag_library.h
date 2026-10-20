// tag_library.h — 导入 / 导出标签库 (pulse_tag_redesign P2): the tags, their
// colors, groups, favorites and items in one JSON file, so another PC gets
// the groups the NTFS streams do not carry.
#pragma once
#include "app_runtime.h"

namespace pulse {
// Asks for a file and writes every tag to it.
void ExportTagLibrary(AppState& s);
// Asks for a file and merges it in: a tag matches by id, then by name (else
// it is created); a group by name. Local choices win (an existing tag keeps
// its color and group); items that are not on this PC are skipped.
void ImportTagLibrary(AppState& s);
}
