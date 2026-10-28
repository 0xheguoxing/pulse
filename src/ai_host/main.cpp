// main.cpp - Pulse.AI.exe, the host of the AI enhancement pack (ai_pack.h).
//
// pulse.exe never ships it; it is downloaded into packs\ai\<version>\ and
// started on demand:
//   --mcp --server <pulse.exe> [--pipe <name>]
//              MCP server over stdio (mcp_server.h), started by
//              pulse.exe --mcp-stdio (ai_launcher.h). It talks to the tool
//              broker on <name> and only to a broker run by <pulse.exe>.
//   --agent    the in-app assistant (OpenAI-compatible models; phase 2)
//   --version  prints the version and exits
// A console program without a window: the launcher starts it with
// CREATE_NO_WINDOW on the client's pipes. Nothing here has UI, so it carries
// no user-facing text; messages to AI clients are English protocol text.
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include "mcp_server.h"
#include "pulse_version.h"

namespace {

int Usage() {
    std::fputs("usage: Pulse.AI.exe --mcp --server <pulse.exe> [--pipe <name>] | --agent | --version\n", stderr);
    return ERROR_INVALID_PARAMETER;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Only System32 for implicit and delay loads: the pack directory is user
    // writable, so never pick DLLs up from beside the executable.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc < 2) return Usage();
    const wchar_t* mode = argv[1];
    if (!wcscmp(mode, L"--version") && argc == 2) {
        std::printf("Pulse.AI %s\n", PULSE_VERSION_STRING_A);
        return 0;
    }
    if (!wcscmp(mode, L"--agent") && argc == 2) {
        std::fputs("Pulse.AI: this mode is not implemented in this build\n", stderr);
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
    if (wcscmp(mode, L"--mcp") != 0) return Usage();

    pulse::ai_host::McpServerOptions options;
    for (int i = 2; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (!wcscmp(argv[i], L"--server") && has_value && options.server_image.empty()) {
            options.server_image = argv[++i];
        } else if (!wcscmp(argv[i], L"--pipe") && has_value && options.pipe_name.empty()) {
            options.pipe_name = argv[++i];
        } else {
            return Usage();
        }
    }
    if (options.server_image.empty()) return Usage();
    // Only Pulse's own pipe namespace.
    if (!options.pipe_name.empty() && options.pipe_name.rfind(L"\\\\.\\pipe\\Pulse.Tools.", 0) != 0) return Usage();
    options.input = GetStdHandle(STD_INPUT_HANDLE);
    options.output = GetStdHandle(STD_OUTPUT_HANDLE);
    options.diagnostics = GetStdHandle(STD_ERROR_HANDLE);
    if (!options.input || options.input == INVALID_HANDLE_VALUE || !options.output ||
        options.output == INVALID_HANDLE_VALUE)
        return ERROR_INVALID_HANDLE;
    options.version = PULSE_VERSION_STRING_A;
    return pulse::ai_host::RunMcpServer(options);
}
