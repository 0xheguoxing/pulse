// ai_tool_protocol.h - The pipe between pulse.exe's tool broker (ai_broker.h)
// and Pulse.AI.exe. Header-only; both sides include it.
//
// Frames are a uint32 little-endian length followed by that many bytes of
// UTF-8 JSON, at most kMaxFrameBytes. Each JSON object has a "type":
//
//   pack -> core   hello    {client:{name,version}, pack_version}   first frame
//                  call     {id, tool, args}
//                  cancel   {id}
//   core -> pack   progress {id, state:"waiting_user"}
//                  result   {id, ok:true, data} | {id, ok:false, error:{code, message, retryable}}
//
// Both executables come from the same build (the pack is pinned by SHA-256),
// so there is no version negotiation; pack_version is for diagnostics.
#pragma once
#include "deadline_pipe.h"
#include "../common/json_value.h"
#include <windows.h>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

namespace pulse::ipc::ai {

inline constexpr uint32_t kMaxFrameBytes = 4u << 20;
// Once a frame's length has arrived, the rest has to follow within this.
inline constexpr DWORD kFrameBodyTimeoutMs = 10000;
inline constexpr DWORD kHelloTimeoutMs = 5000;
inline constexpr int64_t kMaxCallId = int64_t{1} << 53;

inline constexpr char kHello[] = "hello";
inline constexpr char kCall[] = "call";
inline constexpr char kCancel[] = "cancel";
inline constexpr char kProgress[] = "progress";
inline constexpr char kResult[] = "result";
inline constexpr char kWaitingUser[] = "waiting_user";

// \\.\pipe\Pulse.Tools.<session>.<hash of user SID and LOCALAPPDATA>, so
// another user or session never meets this one, and test instances (which
// redirect LOCALAPPDATA) never meet the real one.
inline std::wstring ToolPipeName(DWORD session, std::wstring_view user_sid, std::wstring_view local_app_data) {
    uint64_t hash = 1469598103934665603ull;  // FNV-1a 64
    auto mix = [&](std::wstring_view text) {
        for (wchar_t c : text) {
            const auto lower = static_cast<uint16_t>(std::towlower(c));
            hash = (hash ^ (lower & 0xFF)) * 1099511628211ull;
            hash = (hash ^ (lower >> 8)) * 1099511628211ull;
        }
        hash = (hash ^ 0x7C) * 1099511628211ull;
    };
    mix(user_sid);
    mix(local_app_data);
    wchar_t text[17];
    swprintf_s(text, L"%016llx", static_cast<unsigned long long>(hash));
    return L"\\\\.\\pipe\\Pulse.Tools." + std::to_wstring(session) + L"." + text;
}

enum class FrameStatus { Ok, Closed, TooLarge, BadJson, Failed };

// Writes one frame. `deadline` is absolute (GetTickCount64), 0 = none.
template<class Stopped>
bool WriteFrame(HANDLE pipe, const json::Value& message, ULONGLONG deadline, Stopped stopped) {
    const std::string body = json::Serialize(message);
    if (body.size() > kMaxFrameBytes) { SetLastError(ERROR_FILE_TOO_LARGE); return false; }
    std::string frame(4 + body.size(), '\0');
    const auto n = static_cast<uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) frame[i] = static_cast<char>((n >> (8 * i)) & 0xFF);
    std::copy(body.begin(), body.end(), frame.begin() + 4);
    return DeadlinePipeIo(pipe, reinterpret_cast<uint8_t*>(frame.data()), static_cast<DWORD>(frame.size()), true,
                          deadline, stopped);
}

// Reads one frame and parses it as a JSON object. The length may take until
// `idle_deadline` (0 = wait until stopped); the body gets kFrameBodyTimeoutMs.
template<class Stopped>
FrameStatus ReadFrame(HANDLE pipe, json::Value& message, ULONGLONG idle_deadline, Stopped stopped) {
    uint8_t header[4]{};
    if (!DeadlinePipeIo(pipe, header, 4, false, idle_deadline, stopped)) {
        const DWORD error = GetLastError();
        return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA
            ? FrameStatus::Closed : FrameStatus::Failed;
    }
    const uint32_t n = uint32_t{header[0]} | (uint32_t{header[1]} << 8) | (uint32_t{header[2]} << 16) |
                       (uint32_t{header[3]} << 24);
    if (n == 0 || n > kMaxFrameBytes) return FrameStatus::TooLarge;
    std::string body(n, '\0');
    if (!DeadlinePipeIo(pipe, reinterpret_cast<uint8_t*>(body.data()), n, false,
                        GetTickCount64() + kFrameBodyTimeoutMs, stopped))
        return FrameStatus::Failed;
    json::Value parsed;
    if (!json::Parse(body, parsed) || !parsed.is_object()) return FrameStatus::BadJson;
    message = std::move(parsed);
    return FrameStatus::Ok;
}

inline json::Value MakeHello(std::string_view client_name, std::string_view client_version, std::string_view pack_version) {
    json::Value client = json::Value::Object();
    client.set("name", client_name);
    client.set("version", client_version);
    json::Value m = json::Value::Object();
    m.set("type", kHello);
    m.set("client", std::move(client));
    m.set("pack_version", pack_version);
    return m;
}

inline json::Value MakeCall(int64_t id, std::string_view tool, json::Value args) {
    json::Value m = json::Value::Object();
    m.set("type", kCall);
    m.set("id", id);
    m.set("tool", tool);
    m.set("args", args.is_null() ? json::Value::Object() : std::move(args));
    return m;
}

inline json::Value MakeCancel(int64_t id) {
    json::Value m = json::Value::Object();
    m.set("type", kCancel);
    m.set("id", id);
    return m;
}

inline json::Value MakeProgress(int64_t id, std::string_view state) {
    json::Value m = json::Value::Object();
    m.set("type", kProgress);
    m.set("id", id);
    m.set("state", state);
    return m;
}

inline json::Value MakeResult(int64_t id, json::Value data) {
    json::Value m = json::Value::Object();
    m.set("type", kResult);
    m.set("id", id);
    m.set("ok", true);
    m.set("data", std::move(data));
    return m;
}

inline json::Value MakeError(int64_t id, std::string_view code, std::string_view message, bool retryable) {
    json::Value error = json::Value::Object();
    error.set("code", code);
    error.set("message", message);
    error.set("retryable", retryable);
    json::Value m = json::Value::Object();
    m.set("type", kResult);
    m.set("id", id);
    m.set("ok", false);
    m.set("error", std::move(error));
    return m;
}

} // namespace pulse::ipc::ai
