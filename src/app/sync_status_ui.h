#pragma once
// 同步状态: SVN working-copy marks and OneDrive placeholder states in the list.
// SVN is read straight from the working copy's .svn\wc.db (read-only, off
// the UI thread), so it works without svn.exe (TortoiseSVN leaves the command
// line tools out by default). Marks reuse the Git ones (git_mark.h).
#include "../ui/ui_renderer.h"
#include <string>

namespace pulse {
struct AppState;
namespace app { struct Tab; }

// Marks the rows of a folder inside an SVN working copy (when Git has not
// marked them) and flags folders inside a OneDrive root.
void FillSyncPane(AppState& s, const app::Tab& tab, ui::PaneViewModel& pane);
// "  ·  SVN r1234  ·  3 处更改" after the status bar text.
void AppendSvnStatusText(AppState& s, const app::Tab& tab, std::wstring& text);

}  // namespace pulse
