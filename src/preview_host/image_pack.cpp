#include "image_pack.h"
#include "../common/ffmpeg_tool.h"
#include "../common/image_pack_protocol.h"
#include "../common/preview_extensions.h"
#include "../common/preview_packs.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace pulse::preview {
namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

bool HasRow(const std::vector<PreviewPropertyValue>& rows, const std::wstring& label) {
    return std::any_of(rows.begin(), rows.end(), [&](const PreviewPropertyValue& row) { return row.label == label; });
}

// ---- Concurrency -------------------------------------------------------------
// Thumbnails of a folder share a few slots; the picture the user is looking at
// (Quick Look, details pane) has a slot of its own and never queues behind
// them.

HANDLE GridSlots() {
    static const HANDLE slots = [] {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const LONG n = static_cast<LONG>(ImagePackConcurrency(info.dwNumberOfProcessors));
        return CreateSemaphoreW(nullptr, n, n, nullptr);
    }();
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

bool StartPack(ffmpeg::Process& process, const std::wstring& arguments, SIZE_T memory_limit, DWORD stdout_buffer,
               DWORD priority = BELOW_NORMAL_PRIORITY_CLASS) {
    if (GetTickCount64() < g_backoff_until.load()) return false;
    const std::wstring exe = packs::PackToolPath(packs::PackId::Images, L"pulse-imgpack.exe");
    if (exe.empty()) return false;
    ffmpeg::LaunchOptions options;
    options.memory_limit = memory_limit;
    options.stdout_buffer = stdout_buffer;
    options.priority = priority;
    options.discard_stderr = true;   // a line at most; nobody needs to drain it
    if (process.Start(exe, arguments, options)) {
        g_launch_failures = 0;
        return true;
    }
    if (g_launch_failures.fetch_add(1) + 1 >= kLaunchFailureLimit) {
        g_backoff_until = GetTickCount64() + kLaunchBackoffMs;
        g_launch_failures = 0;
        diagnostics::runtime::Event("image_pack_backoff", {{"error", GetLastError()}});
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
    const UINT limit = cap ? cap : imgpack::kMaxCap;
    return h.magic == imgpack::kMagic && h.version == imgpack::kVersion && h.width && h.height &&
           h.width <= limit && h.height <= limit && h.stride == h.width * 4;
}

void FillFrame(const imgpack::FrameHeader& h, ImagePackFrame& frame) {
    frame.width = h.width;
    frame.height = h.height;
    frame.stride = h.stride;
    frame.source_width = h.source_width ? h.source_width : h.width;
    frame.source_height = h.source_height ? h.source_height : h.height;
    frame.flags = h.flags;
}

// Brands of HEIF still images (ISO/IEC 23008-12) and AVIF.
std::wstring_view HeifBrand(const uint8_t* b, size_t size) {
    if (size < 12 || std::memcmp(b + 4, "ftyp", 4) != 0) return {};
    static const char* const kHeif[] = {"heic", "heix", "heim", "heis", "hevc", "hevx", "hevm", "hevs",
                                        "mif1", "msf1", "mif2", "miaf"};
    if (std::memcmp(b + 8, "avif", 4) == 0 || std::memcmp(b + 8, "avis", 4) == 0) return L"AVIF";
    for (const char* brand : kHeif)
        if (std::memcmp(b + 8, brand, 4) == 0) return L"HEIF";
    return {};
}

std::wstring Yes(std::string_view value, const wchar_t* text) { return value.empty() ? std::wstring() : text; }

} // namespace

// ---- Public API ------------------------------------------------------------

bool IsImagePackExtension(std::wstring_view extension) { return imgpack::IsImagePackExtension(extension); }

bool IsImagePackOnlyExtension(std::wstring_view extension) {
    return imgpack::IsImagePackExtension(extension) && !IsImageExtension(extension);
}

bool ImagePackAvailable() {
    return packs::ResolvePack(packs::PackId::Images).source != packs::ToolSource::None;
}

unsigned ImagePackConcurrency(unsigned cpus) {
    // Each decoder runs two threads: leave most of the machine to the UI.
    return std::clamp(cpus / 4, 1u, 3u);
}

// "#?" and the writing program's name (RADIANCE, RGBE, PFSTOOLS, ...), or
// rarely no magic and straight the FORMAT line. ENVI and Analyze .hdr
// headers start differently and stay with the other previewers.
static bool LooksLikeRadiance(const uint8_t* b, size_t size) {
    if (size >= 18 && std::memcmp(b, "FORMAT=32-bit_rle_", 18) == 0) return true;
    if (size < 3 || b[0] != '#' || b[1] != '?') return false;
    size_t i = 2;
    for (; i < size && i < 66; ++i) {
        const uint8_t c = b[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.' || c == ' '))
            break;
    }
    if (i == 2) return false;
    return i == size || b[i] == '\n' || b[i] == '\r';   // the name may run past a short prefix
}

std::wstring_view SniffImagePackFormat(const uint8_t* b, size_t size) {
    if (!b) return {};
    if (const std::wstring_view heif = HeifBrand(b, size); !heif.empty()) return heif;
    if (size >= 2 && b[0] == 0xFF && b[1] == 0x0A) return L"JPEG XL";
    static const uint8_t kJxlBox[12] = {0, 0, 0, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A};
    if (size >= 12 && std::memcmp(b, kJxlBox, 12) == 0) return L"JPEG XL";
    if (size >= 4 && b[0] == 0x76 && b[1] == 0x2F && b[2] == 0x31 && b[3] == 0x01) return L"OpenEXR";
    if (LooksLikeRadiance(b, size)) return L"Radiance HDR";
    if (size >= 14 && std::memcmp(b, "qoif", 4) == 0) return L"QOI";
    return {};
}

bool LooksLikeImagePackFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    uint8_t head[80]{};   // a Radiance magic line carries the program name
    DWORD got = 0;
    const BOOL ok = ReadFile(file, head, sizeof(head), &got, nullptr);
    CloseHandle(file);
    return ok && !SniffImagePackFormat(head, got).empty();
}

bool ParseImagePackFrame(const std::vector<uint8_t>& output, UINT cap, ImagePackFrame& frame) {
    imgpack::FrameHeader header;
    if (output.size() < sizeof(header)) return false;
    std::memcpy(&header, output.data(), sizeof(header));
    if (!ValidHeader(header, cap)) return false;
    const size_t bytes = static_cast<size_t>(header.stride) * header.height;
    if (output.size() != sizeof(header) + bytes) return false;
    FillFrame(header, frame);
    frame.pixels.assign(output.begin() + sizeof(header), output.end());
    return true;
}

namespace {

// One decode through pulse-imgpack. `slot_wait_ms` is how long to queue for a
// decoder slot; `exit_code` gets the pack's exit code (0 when none ran).
bool RunPackDecode(const std::wstring& path, UINT cap, bool grid, DWORD slot_wait_ms, ImagePackFrame& frame,
                   DWORD& exit_code, DWORD priority = BELOW_NORMAL_PRIORITY_CLASS) {
    exit_code = 0;
    frame = ImagePackFrame{};
    cap = std::clamp<UINT>(cap, 16, imgpack::kMaxCap);
    const ULONGLONG started = GetTickCount64();
    SlotLease slot(grid ? GridSlots() : ViewSlot(), slot_wait_ms);
    if (!slot.held()) {
        if (slot_wait_ms) diagnostics::runtime::Event("image_pack_busy", {{"grid", grid ? 1u : 0u}});
        return false;
    }
    const std::wstring arguments = L"decode " + ffmpeg::QuoteArgument(path) + L" " + std::to_wstring(cap);
    // Grid pictures are small, a full view may hold a 100 MP 16-bit decode.
    const SIZE_T memory = static_cast<SIZE_T>(grid ? 1024 : 3072) * 1024 * 1024;
    bool ok = false;
    bool timed_out = false;
    DWORD code = 0;
    for (int attempt = 0;; ++attempt) {
        ffmpeg::Process process;
        if (!StartPack(process, arguments, memory, 1u << 20, priority)) return false;
        ok = false;
        {
            Watchdog watchdog(process, grid ? 10000 : 30000);
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
        // Starved before main() (every decoder starting at once on a full
        // machine): one more try after a breath instead of a blank tile.
        if (attempt == 0 && !timed_out && ffmpeg::StartupFailed(code)) {
            diagnostics::runtime::Event("image_pack_startup_failed", {{"code", static_cast<uint64_t>(code)}});
            frame = ImagePackFrame{};
            Sleep(150);
            continue;
        }
        break;
    }
    exit_code = timed_out ? 0 : code;
    ok = ok && !timed_out && code == imgpack::kExitOk;
    diagnostics::runtime::Event(ok ? "image_pack_frame" : "image_pack_frame_failed",
        {{"ms", GetTickCount64() - started}, {"grid", grid ? 1u : 0u}, {"code", static_cast<uint64_t>(code)},
         {"thumb", (frame.flags & imgpack::kFlagEmbeddedThumbnail) ? 1u : 0u}});
    if (!ok) frame = ImagePackFrame{};
    return ok;
}

// Pack folder that answered "unsupported" to a PNG / WebP: an older pack.
// Another folder (an update) is asked again.
std::mutex g_accelerate_lock;
std::wstring g_accelerate_refused;

uint32_t Be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint32_t Le24(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16); }

} // namespace

bool ImagePackDecode(const std::wstring& path, UINT cap, bool grid, ImagePackFrame& frame) {
    DWORD code = 0;
    return RunPackDecode(path, cap, grid, grid ? 8000 : 15000, frame, code);
}

uint64_t AcceleratedSourcePixels(const uint8_t* b, size_t size) {
    if (!b) return 0;
    if (size >= 33 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(b + 12, "IHDR", 4) == 0) {
        const uint32_t w = Be32(b + 16), h = Be32(b + 20);
        if (!w || !h || b[28] != 0) return 0;   // interlaced: WIC
        // Chunks before the first IDAT: APNG's acTL and eXIf both come first.
        size_t pos = 8;
        while (pos + 8 <= size) {
            const uint32_t length = Be32(b + pos);
            const uint8_t* type = b + pos + 4;
            if (std::memcmp(type, "IDAT", 4) == 0) return static_cast<uint64_t>(w) * h;
            if (std::memcmp(type, "acTL", 4) == 0 || std::memcmp(type, "eXIf", 4) == 0) return 0;
            if (length > size) return 0;
            pos += 12 + static_cast<size_t>(length);
        }
        return 0;   // IDAT not within the bytes read: leave it to WIC
    }
    if (size >= 30 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) {
        const uint8_t* chunk = b + 12;
        const uint8_t* d = b + 20;
        if (std::memcmp(chunk, "VP8X", 4) == 0) {
            if (d[0] & (0x02 | 0x08)) return 0;   // animation, Exif
            return static_cast<uint64_t>(Le24(d + 4) + 1) * (Le24(d + 7) + 1);
        }
        if (std::memcmp(chunk, "VP8 ", 4) == 0) {
            if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) return 0;
            const uint32_t w = (d[6] | (d[7] << 8)) & 0x3fff, h = (d[8] | (d[9] << 8)) & 0x3fff;
            return static_cast<uint64_t>(w) * h;
        }
        if (std::memcmp(chunk, "VP8L", 4) == 0) {
            if (d[0] != 0x2f) return 0;
            const uint32_t bits = uint32_t{d[1]} | (uint32_t{d[2]} << 8) | (uint32_t{d[3]} << 16) | (uint32_t{d[4]} << 24);
            return static_cast<uint64_t>((bits & 0x3fff) + 1) * (((bits >> 14) & 0x3fff) + 1);
        }
    }
    return 0;
}

bool ImagePackAccelerate(const std::wstring& path, UINT cap, bool grid, ImagePackFrame& frame) {
    frame = ImagePackFrame{};
    if (!ImagePackAvailable()) return false;
    const std::wstring exe = packs::PackToolPath(packs::PackId::Images, L"pulse-imgpack.exe");
    if (exe.empty()) return false;
    {
        std::lock_guard<std::mutex> guard(g_accelerate_lock);
        if (exe == g_accelerate_refused) return false;
    }
    // Headers, APNG's acTL and the chunks ahead of IDAT (an ICC profile, text).
    uint8_t head[16384];
    DWORD got = 0;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool read = ReadFile(file, head, sizeof(head), &got, nullptr) != FALSE;
    CloseHandle(file);
    if (!read) return false;
    const bool webp = got >= 12 && std::memcmp(head + 8, "WEBP", 4) == 0;
    if (AcceleratedSourcePixels(head, got) < (webp ? imgpack::kAccelerateMinPixelsWebp : imgpack::kAccelerateMinPixels))
        return false;
    // A full grid queue means WIC starts now instead of waiting for a slot.
    DWORD code = 0;
    // Same priority as this process: the WIC decode it replaces runs here, and a
    // below-normal decoder starves behind a busy machine (a build: 5x slower).
    if (RunPackDecode(path, cap, grid, grid ? 0 : 2000, frame, code, GetPriorityClass(GetCurrentProcess()))) return true;
    if (code == imgpack::kExitUnsupported) {
        std::lock_guard<std::mutex> guard(g_accelerate_lock);
        g_accelerate_refused = exe;
        diagnostics::runtime::Event("image_pack_no_accelerate", {{"code", static_cast<uint64_t>(code)}});
    }
    return false;
}

std::vector<PreviewPropertyValue> ParseImagePackProbe(std::string_view output) {
    std::string format, width, height, bits, alpha, hdr, primaries, images, animated, lossless, channels,
        compression, parts, depth;
    size_t pos = 0;
    while (pos < output.size()) {
        size_t end = output.find('\n', pos);
        if (end == std::string_view::npos) end = output.size();
        std::string_view line = output.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq);
        const std::string value(line.substr(eq + 1));
        if (key == "format") format = value;
        else if (key == "width") width = value;
        else if (key == "height") height = value;
        else if (key == "bit_depth") bits = value;
        else if (key == "alpha") alpha = value;
        else if (key == "hdr") hdr = value;
        else if (key == "primaries") primaries = value;
        else if (key == "images") images = value;
        else if (key == "animated") animated = value;
        else if (key == "lossless") lossless = value;
        else if (key == "channels") channels = value;
        else if (key == "compression") compression = value;
        else if (key == "parts") parts = value;
        else if (key == "depth") depth = value;
    }
    std::vector<PreviewPropertyValue> rows;
    if (!width.empty() && !height.empty() && width != "0")
        rows.push_back({L"尺寸", Utf8ToWide(width) + L" x " + Utf8ToWide(height)});
    if (!format.empty()) {
        std::wstring text = Utf8ToWide(format);
        const int depth_bits = atoi(bits.c_str());
        if (depth_bits > 8) text += L" · " + std::to_wstring(depth_bits) + L" 位";
        if (!hdr.empty()) text += hdr == "yes" ? std::wstring(L" · HDR") : L" · HDR " + Utf8ToWide(hdr);
        if (!primaries.empty()) text += L" · " + Utf8ToWide(primaries);
        text += Yes(lossless, L" · 无损") + Yes(animated, L" · 动画") + Yes(alpha, L" · 透明") + Yes(depth, L" · 景深");
        rows.push_back({L"格式", text});
    }
    if (!compression.empty()) rows.push_back({L"压缩", Utf8ToWide(compression)});
    if (!channels.empty()) rows.push_back({L"通道", Utf8ToWide(channels)});
    if (!images.empty()) rows.push_back({L"图像", Utf8ToWide(images) + L" 张"});
    if (!parts.empty()) rows.push_back({L"图层", Utf8ToWide(parts) + L" 个部分"});
    return rows;
}

bool ImagePackProperties(const std::wstring& path, std::vector<PreviewPropertyValue>& rows, size_t max_rows) {
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
