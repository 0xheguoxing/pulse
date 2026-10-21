// ai_pack.h - The AI enhancement pack: Pulse.AI.exe, which serves Pulse's
// tools to MCP clients (--mcp) and, later, runs the in-app assistant
// (--agent).
//
// Like the content pack (content_pack.h) it never ships in the installer.
// pulse.exe downloads it on request from the GitHub release of its own
// version into
//   %LOCALAPPDATA%\Pulse\packs\ai\<version>\
// and only starts it once installed.json names exactly this version and the
// file matches the size and SHA-256 compiled in (ai_pack_catalog.h, generated
// by pulse_pack_catalog from the same build). That pins the host to this
// app's broker protocol without a handshake.
#pragma once
#include "../common/windows_compat.h"
#include "content_pack.h"
#include "pack_catalog.h"
#include <string>

namespace pulse::app {

// The same states as the content pack; Bundled means Pulse.AI.exe sits beside
// pulse.exe (development builds).
using AiPackState = ContentPackState;

// The release this build installs; file_count is 0 when it has none.
const PackRelease& AiPackRelease() noexcept;

// The state for this process. A successful verification is cached, with the
// file held open (read sharing only), until ForgetVerifiedAiPack.
AiPackState QueryAiPackState();
inline bool AiPackInstallable(AiPackState state) { return ContentPackInstallable(state); }
inline bool AiPackReady(AiPackState state) {
    return state == AiPackState::Ready || state == AiPackState::Bundled;
}

// Pulse.AI.exe of a verified (or bundled) pack, else empty.
std::wstring LocateAiHost();

// True when the process runs the located host: same image path as
// LocateAiHost(), the very file verified (volume serial + file index of the
// held handle; development builds: the sibling file), same session and user.
bool IsAiHostProcess(DWORD process_id);
// The same for the client of a connected named pipe (the broker's verify_peer).
bool IsAiHostPipeClient(HANDLE pipe);

// Drops the cached verification (after an install), releasing the held file.
void ForgetVerifiedAiPack();

// Deletes every installed version under <packs root>\ai. Fails with a Win32
// code (typically ERROR_SHARING_VIOLATION while Pulse.AI.exe runs) and leaves
// what it could not delete. A Bundled host is never touched.
bool RemoveAiPack(DWORD& error);

} // namespace pulse::app
