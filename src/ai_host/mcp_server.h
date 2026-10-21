// mcp_server.h - Pulse.AI.exe --mcp: an MCP server on stdin/stdout.
//
// Newline-delimited JSON-RPC 2.0 (stdio transport). Dual-era:
//   modern (2026-07-28): no handshake; every request may carry
//     _meta["io.modelcontextprotocol/protocolVersion"]; server/discover;
//     results carry resultType and serverInfo; an unknown version gets
//     UnsupportedProtocolVersionError (-32022) with the supported list.
//   legacy (2025-11-25 and earlier): initialize / notifications/initialized,
//     ping.
// tools/list and tools/call work either way; notifications/cancelled cancels
// a call (no answer is sent for it); a progressToken gets a progress
// notification while Pulse waits for the user. Calls run concurrently (at
// most kMaxCalls) and answer out of order.
//
// stdout carries protocol only. stderr gets short diagnostics without
// arguments or paths. Returns when stdin ends.
#pragma once
#include <windows.h>
#include <string>

namespace pulse::ai_host {

struct McpServerOptions {
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE diagnostics = nullptr;     // stderr; may be null
    std::wstring pipe_name;           // the broker; empty: every call is pulse_not_running
    std::wstring server_image;        // the pulse.exe that must own the pipe
    std::string version;              // Pulse.AI's version
};

inline constexpr size_t kMaxCalls = 4;
inline constexpr size_t kMaxLineBytes = 4u << 20;

int RunMcpServer(const McpServerOptions& options);

} // namespace pulse::ai_host
