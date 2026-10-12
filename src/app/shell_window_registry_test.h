#pragma once
#ifdef PULSE_SHELL_WINDOW_REGISTRY_TEST
#include "shell_window_plan.h"
#include <shlobj.h>
#include <exdisp.h>
namespace pulse::app {
HRESULT ShellRegistryTestCreate(IShellWindows** out);
HRESULT ShellRegistryTestInitialize();
HANDLE ShellRegistryTestStart(LPTHREAD_START_ROUTINE start, void* param, DWORD* thread_id);
PIDLIST_ABSOLUTE ShellRegistryTestFolder(const std::wstring& path);
// The known-folder form of `path`, or null when it has none.
PIDLIST_ABSOLUTE ShellRegistryTestFolderAlias(const std::wstring& path);
void ShellRegistryTestApplied(const std::vector<ShellWindowEntry>& acknowledged);
}
#endif
