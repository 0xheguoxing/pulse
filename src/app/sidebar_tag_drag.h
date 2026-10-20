#pragma once
#include "app_runtime.h"

// Sidebar tag rows reorder through the shared sidebar row gesture (pinDrag*),
// the one quick-access and vertical-tab rows use, so they get the same
// lift-and-make-room motion (pulse_motion_v2 "qareorder") instead of a motion
// of their own.
namespace pulse {

bool IsSidebarTagPath(const std::wstring& path);
// Measures the insertion slot among the tag rows; false when the gesture is
// not on a tag row.
bool UpdateSidebarTagDrag(AppState& s, int my);
// insert_at is the gap in display order (0 = before the first tag row).
void CommitSidebarTagDrag(AppState& s, const std::wstring& path, size_t insert_at);

}  // namespace pulse
