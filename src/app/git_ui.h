#pragma once
// In-app Git, the window side: status requests (throttled, off the UI
// thread), row marks for the list, the branch in the status bar, and the
// pull / push / refresh commands of the folder and project-row menus.
#include "../ui/fluent_menu.h"
#include "../ui/ui_renderer.h"
#include <windows.h>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace app { struct Tab; }

constexpr int kCmdGitPull = 38100;
constexpr int kCmdGitPush = 38101;
constexpr int kCmdGitRefresh = 38102;

// Marks the rows of a folder inside a repository, and asks git again when
// the listing changed or the last answer is old.
void FillGitPane(AppState& s, const app::Tab& tab, ui::PaneViewModel& pane);
// "  ·  Git main ↑1  ·  3 处更改" after the status bar text.
void AppendGitStatusText(const app::Tab& tab, std::wstring& text);
// A Git flyout (pull / push / refresh) after 在终端中打开, for a folder in a repo.
void AppendGitMenu(const app::Tab& tab, std::vector<ui::FluentMenuItem>& items);
// Runs a kCmdGit* command for the repository at `root`; false for any other.
bool HandleGitCommand(AppState& s, int command, const std::wstring& root);
// The sidebar 项目 row's menu; false when `path` is not that row's repository.
bool ShowGitProjectMenu(AppState& s, const std::wstring& path, POINT screen_pt);
// WM_GIT_RESULT: a finished status / pull / push.
void ReceiveGitResult(AppState& s, LPARAM lparam);

}  // namespace pulse
