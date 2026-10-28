// ai_launcher.h - pulse.exe --mcp-stdio, the command AI clients are given
// (Claude Desktop, Claude Code, ...):
//   {"mcpServers":{"pulse":{"command":"<pulse.exe>","args":["--mcp-stdio"]}}}
//
// It runs before anything else in wWinMain (no window, no D2D, no single
// instance): checks the AI pack, then starts the verified Pulse.AI.exe --mcp
// on the client's stdin/stdout/stderr inside a kill-on-close job object, waits
// for it and returns its exit code. The pack stays held (QueryAiPackState)
// while it runs, so the file cannot be swapped underneath.
#pragma once
#include <windows.h>

namespace pulse::app::ai {

inline constexpr int kLauncherPackUnusable = 3;    // not installed, outdated, damaged or unavailable
inline constexpr int kLauncherStartFailed = 4;     // Pulse.AI.exe could not be started
inline constexpr int kLauncherNoStdio = 5;         // started without standard input/output

bool IsMcpStdioLaunch(int argc, wchar_t** argv);
int RunMcpStdioLauncher(HINSTANCE instance);

} // namespace pulse::app::ai
