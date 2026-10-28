#include "wall_aspects.h"
#include "../common/preview_extensions.h"
#include "../fs/cloud_state.h"
#include <objbase.h>
#include <propidl.h>
#include <propsys.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string_view>

namespace pulse::ui {

bool IsWallPicture(const std::wstring& name, bool is_dir) {
    if (is_dir) return false;
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) return false;
    std::wstring ext(name, dot, std::wstring::npos);
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return preview::IsImageExtension(ext) || preview::IsVideoExtension(ext);
}

namespace {

constexpr size_t kMaxKnown = 200000;     // ~16 bytes of payload each
constexpr size_t kMaxProbe = 50000;      // per request; the rest come from thumbnails
constexpr ULONGLONG kNotifyMs = 150;

uint16_t Be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[1] << 8 | p[0]); }
uint32_t Be32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint32_t Le32(const uint8_t* p) { return static_cast<uint32_t>(p[3]) << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
uint32_t Le24(const uint8_t* p) { return static_cast<uint32_t>(p[2]) << 16 | p[1] << 8 | p[0]; }

float Ratio(uint32_t w, uint32_t h, bool swap = false) {
    if (w == 0 || h == 0) return 0.0f;
    return swap ? static_cast<float>(h) / static_cast<float>(w) : static_cast<float>(w) / static_cast<float>(h);
}

// EXIF orientation from an APP1 payload that starts after "Exif\0\0"; 0 = none.
int ExifOrientation(const uint8_t* t, size_t n) {
    if (n < 8) return 0;
    const bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return 0;
    auto u16 = [&](size_t at) { return le ? Le16(t + at) : Be16(t + at); };
    auto u32 = [&](size_t at) { return le ? Le32(t + at) : Be32(t + at); };
    if (u16(2) != 42) return 0;
    const size_t ifd = u32(4);
    if (ifd + 2 > n) return 0;
    const size_t entries = u16(ifd);
    for (size_t i = 0; i < entries; ++i) {
        const size_t e = ifd + 2 + i * 12;
        if (e + 12 > n) break;
        if (u16(e) == 0x0112) return u16(e + 8);  // SHORT, left-justified in the value
    }
    return 0;
}

// read_at(offset, out, n) -> bytes read. Only the bytes needed are read.
template <class ReadAt>
float ParseAspect(ReadAt&& read_at) {
    uint8_t h[32] = {};
    const size_t got = read_at(0, h, sizeof(h));
    if (got >= 24 && std::memcmp(h, "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(h + 12, "IHDR", 4) == 0)
        return Ratio(Be32(h + 16), Be32(h + 20));
    if (got >= 10 && (std::memcmp(h, "GIF87a", 6) == 0 || std::memcmp(h, "GIF89a", 6) == 0))
        return Ratio(Le16(h + 6), Le16(h + 8));
    if (got >= 26 && h[0] == 'B' && h[1] == 'M') {
        if (Le32(h + 14) == 12) return Ratio(Le16(h + 18), Le16(h + 20));
        const int32_t w = static_cast<int32_t>(Le32(h + 18)), hh = static_cast<int32_t>(Le32(h + 22));
        return Ratio(static_cast<uint32_t>(std::abs(w)), static_cast<uint32_t>(std::abs(hh)));
    }
    if (got >= 30 && std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WEBP", 4) == 0) {
        if (std::memcmp(h + 12, "VP8 ", 4) == 0) return Ratio(Le16(h + 26) & 0x3FFF, Le16(h + 28) & 0x3FFF);
        if (std::memcmp(h + 12, "VP8L", 4) == 0 && h[20] == 0x2F) {
            const uint8_t* b = h + 21;
            const uint32_t w = 1 + (((b[1] & 0x3Fu) << 8) | b[0]);
            const uint32_t hh = 1 + (((b[3] & 0x0Fu) << 10) | (static_cast<uint32_t>(b[2]) << 2) | ((b[1] & 0xC0u) >> 6));
            return Ratio(w, hh);
        }
        if (std::memcmp(h + 12, "VP8X", 4) == 0) return Ratio(1 + Le24(h + 24), 1 + Le24(h + 27));
        return 0.0f;
    }
    if (got < 4 || h[0] != 0xFF || h[1] != 0xD8) return 0.0f;
    // JPEG: walk the segments to the frame header, noting the EXIF orientation.
    uint64_t pos = 2;
    int orientation = 0;
    for (int guard = 0; guard < 256; ++guard) {
        uint8_t m[4];
        if (read_at(pos, m, 4) < 4) return 0.0f;
        if (m[0] != 0xFF) return 0.0f;
        if (m[1] == 0xFF) { ++pos; continue; }                 // fill byte
        const uint8_t marker = m[1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { pos += 2; continue; }
        if (marker == 0xD9 || marker == 0xDA) return 0.0f;     // image data before any frame
        const uint16_t len = Be16(m + 2);
        if (len < 2) return 0.0f;
        const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (sof) {
            uint8_t f[5];
            if (read_at(pos + 4, f, 5) < 5) return 0.0f;
            const bool swap = orientation >= 5 && orientation <= 8;
            return Ratio(Be16(f + 3), Be16(f + 1), swap);
        }
        if (marker == 0xE1 && orientation == 0 && len > 14) {
            std::vector<uint8_t> app(std::min<size_t>(len - 2, 65533));
            const size_t n = read_at(pos + 4, app.data(), app.size());
            if (n > 6 && std::memcmp(app.data(), "Exif\0\0", 6) == 0)
                orientation = ExifOrientation(app.data() + 6, n - 6);
        }
        pos += 2 + len;
    }
    return 0.0f;
}

bool HeaderFormat(const std::wstring& path) {
    const size_t dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    static constexpr std::wstring_view known[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".png",
                                                  L".gif", L".bmp", L".dib", L".webp"};
    return std::find(std::begin(known), std::end(known), ext) != std::end(known);
}

float ShellAspect(const std::wstring& path) {
    // Local keys: propkey.h's would need INITGUID in one translation unit.
    static constexpr GUID kImage = {0x6444048F, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
    static constexpr GUID kVideo = {0x64440491, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}};
    static constexpr GUID kPhoto = {0x14B81DA1, 0x0135, 0x4D31, {0x96, 0xD9, 0x6C, 0xBF, 0xC9, 0x67, 0x1A, 0x99}};
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))) || !store)
        return 0.0f;
    auto number = [&](const GUID& fmtid, DWORD pid) -> uint32_t {
        PROPVARIANT v;
        PropVariantInit(&v);
        uint32_t out = 0;
        if (SUCCEEDED(store->GetValue(PROPERTYKEY{fmtid, pid}, &v))) {
            if (v.vt == VT_UI4) out = v.ulVal;
            else if (v.vt == VT_UI2) out = v.uiVal;
            else if (v.vt == VT_I4 && v.lVal > 0) out = static_cast<uint32_t>(v.lVal);
        }
        PropVariantClear(&v);
        return out;
    };
    float aspect = 0.0f;
    if (const uint32_t w = number(kImage, 3), h = number(kImage, 4); w && h) {
        const uint32_t orientation = number(kPhoto, 274);
        aspect = Ratio(w, h, orientation >= 5 && orientation <= 8);
    } else {
        aspect = Ratio(number(kVideo, 3), number(kVideo, 4));
    }
    store->Release();
    return aspect;
}

}  // namespace

float HeaderAspect(const uint8_t* data, size_t size) noexcept {
    return ParseAspect([&](uint64_t at, void* out, size_t n) -> size_t {
        if (at >= size) return 0;
        const size_t take = std::min<size_t>(n, size - static_cast<size_t>(at));
        std::memcpy(out, data + at, take);
        return take;
    });
}

float ProbeFileAspect(const std::wstring& path) {
    // A cloud placeholder's header (or the property handler behind
    // ShellAspect) is file content: reading it would make the sync engine
    // download the picture just to size a tile. Unknown keeps the default.
    if (const DWORD attrs = GetFileAttributesW(path.c_str());
        attrs != INVALID_FILE_ATTRIBUTES && fs::IsCloudPlaceholder(attrs))
        return 0.0f;
    if (!HeaderFormat(path)) return ShellAspect(path);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0.0f;
    const float aspect = ParseAspect([&](uint64_t at, void* out, size_t n) -> size_t {
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(at);
        o.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD read = 0;
        return ReadFile(file, out, static_cast<DWORD>(n), &read, &o) ? read : 0;
    });
    CloseHandle(file);
    return aspect;
}

WallAspects& WallAspects::Instance() {
    // Never destroyed: a probe may still be running while the process exits.
    static WallAspects* instance = new WallAspects();
    return *instance;
}

uint64_t WallAspects::Hash(const std::wstring& path) noexcept {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (wchar_t c : path) { h ^= static_cast<uint16_t>(c); h *= 1099511628211ull; }
    return h ? h : 1;
}

void WallAspects::Lookup(const uint64_t* hashes, float* out, size_t count) const {
    std::lock_guard lock(mutex_);
    if (known_.empty()) return;
    for (size_t i = 0; i < count; ++i) {
        if (!hashes[i]) continue;
        const auto it = known_.find(hashes[i]);
        if (it != known_.end()) out[i] = it->second;
    }
}

bool WallAspects::Store(uint64_t hash, float aspect, float tolerance) {
    if (!hash || !(aspect > 0.0f) || !std::isfinite(aspect)) return false;
    std::lock_guard lock(mutex_);
    const auto it = known_.find(hash);
    if (it != known_.end()) {
        if (std::fabs(it->second - aspect) <= tolerance * aspect) return false;
        it->second = aspect;
    } else {
        if (known_.size() >= kMaxKnown) known_.clear();
        known_.emplace(hash, aspect);
    }
    version_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool WallAspects::Note(uint64_t hash, float aspect) { return Store(hash, aspect, 0.02f); }

size_t WallAspects::SizeForTest() const {
    std::lock_guard lock(mutex_);
    return known_.size();
}

void WallAspects::Probe(std::vector<std::pair<uint64_t, std::wstring>> files, HWND notify) {
    if (files.size() > kMaxProbe) files.resize(kMaxProbe);
    std::reverse(files.begin(), files.end());  // folder order first
    std::lock_guard lock(mutex_);
    queue_ = std::move(files);
    notify_ = notify;
    if (running_ || queue_.empty()) return;
    running_ = true;
    if (!TrySubmitThreadpoolCallback(&WallAspects::Work, this, nullptr)) running_ = false;
}

void CALLBACK WallAspects::Work(PTP_CALLBACK_INSTANCE, void* context) {
    static_cast<WallAspects*>(context)->Run();
}

void WallAspects::Run() {
    // Low CPU and I/O priority: the wall works without these, they only spare
    // the rows a later reflow.
    const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    ULONGLONG last_notify = GetTickCount64();
    bool dirty = false;
    for (;;) {
        std::pair<uint64_t, std::wstring> file;
        HWND notify = nullptr;
        {
            std::lock_guard lock(mutex_);
            notify = notify_;
            if (queue_.empty()) { running_ = false; break; }
            file = std::move(queue_.back());
            queue_.pop_back();
            if (known_.contains(file.first)) continue;
        }
        if (Store(file.first, ProbeFileAspect(file.second), 0.0f)) dirty = true;
        const ULONGLONG now = GetTickCount64();
        if (dirty && now - last_notify >= kNotifyMs) {
            if (notify) InvalidateRect(notify, nullptr, FALSE);
            last_notify = now;
            dirty = false;
        }
    }
    if (dirty) {
        std::lock_guard lock(mutex_);
        if (notify_) InvalidateRect(notify_, nullptr, FALSE);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

}  // namespace pulse::ui
