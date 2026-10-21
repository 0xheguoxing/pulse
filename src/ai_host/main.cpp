// main.cpp - Pulse.AI.exe, the host of the AI enhancement pack (ai_pack.h).
//
// pulse.exe never ships it; it is downloaded into packs\ai\<version>\ and
// started on demand:
//   --mcp      MCP server over stdio for AI clients (launched through
//              pulse.exe --mcp-stdio), talking to pulse.exe's tool broker
//   --agent    the in-app assistant (OpenAI-compatible models)
//   --version  prints the version and exits
// This is the M1 shell: the modes are reserved and report "not implemented"
// (ERROR_CALL_NOT_IMPLEMENTED) until the broker and the MCP server land.
// Nothing here has UI, so it carries no user-facing text.
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "pulse_version.h"

int wmain(int argc, wchar_t** argv) {
    // Only System32 for implicit and delay loads: the pack directory is user
    // writable, so never pick DLLs up from beside the executable.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2) {
        std::fputs("usage: Pulse.AI.exe --mcp | --agent | --version\n", stderr);
        return ERROR_INVALID_PARAMETER;
    }
    const wchar_t* mode = argv[1];
    if (!wcscmp(mode, L"--version")) {
        std::printf("Pulse.AI %s\n", PULSE_VERSION_STRING_A);
        return 0;
    }
    if (!wcscmp(mode, L"--mcp") || !wcscmp(mode, L"--agent")) {
        std::fputs("Pulse.AI: this mode is not implemented in this build\n", stderr);
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    std::fputs("usage: Pulse.AI.exe --mcp | --agent | --version\n", stderr);
    return ERROR_INVALID_PARAMETER;
}
