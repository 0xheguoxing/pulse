#include "raw_pack.h"
#include "../common/ffmpeg_tool.h"
#include "../common/image_pack_protocol.h"
#include "../common/preview_extensions.h"
#include "../common/preview_packs.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace pulse::preview {
namespace {

bool HasRow(const std::vector<PreviewPropertyValue>& rows, const std::wstring& label) {
    return std::any_of(rows.begin(), rows.end(), [&](const PreviewPropertyValue& row) { return row.label == label; });
}

// ---- Concurrency -------------------------------------------------------------
// Thumbnails of a folder share a few slots; the picture the user is looking at
// (Quick Look, details pane) has a slot of its own and never queues behind
// them.

HANDLE GridSlots() {
    static const HANDLE slots = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    return slots;
}

HANDLE ViewSlot() {
    static const HANDLE slot = CreateSemaphoreW(nullptr, 1, 1, nullptr);
    return slot;
}

class SlotLease {
public:
    SlotLease(HANDLE semaphore, DWORD wait_ms)
        : semaphore_(semaphore), held_(semaphore && WaitForSingleObject(semaphore, wait_ms) == WAIT_OBJECT_0) {}
    ~SlotLease() { if (held_) ReleaseSemaphore(semaphore_, 1, nullptr); }
    SlotLease(const SlotLease&) = delete;
    SlotLease& operator=(const SlotLease&) = delete;
    bool held() const { return held_; }
private:
    HANDLE semaphore_;
    bool held_;
};

// Launch failures (blocked by antivirus, damaged pack) back off instead of
// costing a CreateProcess per thumbnail.
std::atomic<uint32_t> g_launch_failures{0};
std::atomic<ULONGLONG> g_backoff_until{0};
constexpr uint32_t kLaunchFailureLimit = 3;
constexpr ULONGLONG kLaunchBackoffMs = 60000;

bool StartPack(ffmpeg::Process& process, const std::wstring& arguments, SIZE_T memory_limit, DWORD stdout_buffer) {
    if (GetTickCount64() < g_backoff_until.load()) return false;
    const std::wstring exe = packs::PackToolPath(packs::PackId::Raw, L"pulse-rawpack.exe");
    if (exe.empty()) return false;
    ffmpeg::LaunchOptions options;
    options.memory_limit = memory_limit;
    options.stdout_buffer = stdout_buffer;
    options.discard_stderr = true;   // a line at most; nobody needs to drain it
    if (process.Start(exe, arguments, options)) {
        g_launch_failures = 0;
        return true;
    }
    if (g_launch_failures.fetch_add(1) + 1 >= kLaunchFailureLimit) {
        g_backoff_until = GetTickCount64() + kLaunchBackoffMs;
        g_launch_failures = 0;
        diagnostics::runtime::Event("raw_pack_backoff", {{"error", GetLastError()}});
    }
    return false;
}

// Kills the decoder when it overruns; a blocked pipe read then returns.
class Watchdog {
public:
    Watchdog(ffmpeg::Process& process, DWORD timeout_ms)
        : thread_([this, &process, timeout_ms] {
              if (!process.Wait(timeout_ms)) {
                  timed_out_ = true;
                  process.Terminate(ERROR_TIMEOUT);
              }
          }) {}
    ~Watchdog() { thread_.join(); }
    bool timed_out() const { return timed_out_; }
private:
    std::atomic<bool> timed_out_{false};
    std::thread thread_;
};

bool ValidHeader(const imgpack::FrameHeader& h, UINT cap) {
    const UINT limit = std::min(cap ? cap : imgpack::kMaxCap, imgpack::kMaxCap);
    return h.magic == imgpack::kMagic && h.version == imgpack::kVersion && h.width && h.height &&
           h.width <= limit && h.height <= limit && h.stride == h.width * 4 && h.source_width && h.source_height &&
           uint64_t(h.source_width) * h.source_height <= imgpack::kMaxSourcePixels;
}

void FillFrame(const imgpack::FrameHeader& h, ImagePackFrame& frame) {
    frame.width = h.width;
    frame.height = h.height;
    frame.stride = h.stride;
    frame.source_width = h.source_width ? h.source_width : h.width;
    frame.source_height = h.source_height ? h.source_height : h.height;
    frame.flags = h.flags;
}

} // namespace

bool RawPackAvailable() { return packs::ResolvePack(packs::PackId::Raw).source != packs::ToolSource::None; }

bool RawPackDecode(const std::wstring& path, UINT cap, bool allow_full_decode, ImagePackFrame& frame) {
    const bool grid = !allow_full_decode;
    frame = ImagePackFrame{};
    cap = std::clamp<UINT>(cap, 16, imgpack::kMaxCap);
    const ULONGLONG started = GetTickCount64();
    SlotLease slot(grid ? GridSlots() : ViewSlot(), grid ? 0 : 1000);
    if (!slot.held()) {
        diagnostics::runtime::Event("raw_pack_busy", {{"grid", grid ? 1u : 0u}});
        return false;
    }
    const std::wstring arguments = (allow_full_decode ? L"decode-full " : L"decode ") + ffmpeg::QuoteArgument(path) + L" " + std::to_wstring(cap);
    // Embedded previews stay cheap; only explicit large views may demosaic.
    const SIZE_T memory = static_cast<SIZE_T>(grid ? 512 : 1536) * 1024 * 1024;
    bool ok = false;
    bool timed_out = false;
    DWORD code = 0;
    for (int attempt = 0;; ++attempt) {
        ffmpeg::Process process;
        if (!StartPack(process, arguments, memory, 1u << 20)) return false;
        ok = false;
        {
            Watchdog watchdog(process, grid ? 3000 : 20000);
            imgpack::FrameHeader header;
            if (process.ReadOutExact(&header, sizeof(header)) && ValidHeader(header, cap)) {
                FillFrame(header, frame);
                frame.pixels.resize(static_cast<size_t>(header.stride) * header.height);
                ok = process.ReadOutExact(frame.pixels.data(), static_cast<DWORD>(frame.pixels.size()));
            }
            // Drain to the end so a well-behaved decoder exits with its code.
            uint8_t rest[256];
            while (process.ReadOut(rest, sizeof(rest))) ok = false;   // more than announced
            process.Wait(INFINITE);   // the watchdog bounds this
            timed_out = watchdog.timed_out();
        }
        code = process.ExitCode();
        // Starved before main(): one more try (see ffmpeg::StartupFailed).
        if (attempt == 0 && !timed_out && ffmpeg::StartupFailed(code)) {
            diagnostics::runtime::Event("raw_pack_startup_failed", {{"code", static_cast<uint64_t>(code)}});
            frame = ImagePackFrame{};
            Sleep(150);
            continue;
        }
        break;
    }
    ok = ok && !timed_out && code == imgpack::kExitOk;
    diagnostics::runtime::Event(ok ? "raw_pack_frame" : "raw_pack_frame_failed",
        {{"ms", GetTickCount64() - started}, {"grid", grid ? 1u : 0u}, {"code", static_cast<uint64_t>(code)},
         {"thumb", (frame.flags & imgpack::kFlagEmbeddedThumbnail) ? 1u : 0u}});
    if (!ok) frame = ImagePackFrame{};
    return ok;
}

bool RawPackProperties(const std::wstring& path, std::vector<PreviewPropertyValue>& rows, size_t max_rows) {
    if (rows.size() >= max_rows) return false;
    ffmpeg::Process process;
    // Headers only: small, quick, and outside the decode slots.
    if (!StartPack(process, L"probe " + ffmpeg::QuoteArgument(path), static_cast<SIZE_T>(256) * 1024 * 1024,
                   64 * 1024))
        return false;
    std::string text;
    {
        Watchdog watchdog(process, 5000);
        char buffer[4096];
        while (const DWORD got = process.ReadOut(buffer, sizeof(buffer)))
            if (text.size() < 64 * 1024) text.append(buffer, got);
        process.Wait(INFINITE);
        if (watchdog.timed_out()) return false;
    }
    if (process.ExitCode() != imgpack::kExitOk) return false;
    bool added = false;
    for (PreviewPropertyValue& row : ParseImagePackProbe(text)) {
        if (rows.size() >= max_rows) break;
        if (HasRow(rows, row.label)) continue;
        rows.push_back(std::move(row));
        added = true;
    }
    return added;
}

} // namespace pulse::preview
