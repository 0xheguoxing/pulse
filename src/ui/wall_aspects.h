#pragma once
// Photo wall aspect ratios (Pulse_视图切换与图标密度方案.html, P2): width /
// height of pictures and videos. Decoded thumbnails report theirs while they
// are drawn; a background probe reads the rest from the file header (JPEG with
// its EXIF orientation, PNG, GIF, BMP, WebP) or, for other formats, the
// shell's System.Image.Dimensions / System.Video.FrameWidth+Height.
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::ui {

// Files the photo wall shows as pictures (images and videos), by extension.
bool IsWallPicture(const std::wstring& name, bool is_dir);

// Header parse over a whole buffer (tests); 0 when the format is unknown or
// the buffer ends first. EXIF orientations 5..8 swap width and height.
float HeaderAspect(const uint8_t* data, size_t size) noexcept;
// Reads only the bytes it needs; falls back to the shell's properties for
// formats the header parser does not know. 0 when nothing tells.
float ProbeFileAspect(const std::wstring& path);

class WallAspects {
public:
    static WallAspects& Instance();
    // Key of a path (never 0).
    static uint64_t Hash(const std::wstring& path) noexcept;
    // out[i] = known aspect of hashes[i], left alone when not known (0 hashes skipped).
    void Lookup(const uint64_t* hashes, float* out, size_t count) const;
    // A decoded thumbnail's aspect; true when it differs from what was known
    // by more than rounding (a 256 px decode of a 1500x1000 photo is 1.506).
    bool Note(uint64_t hash, float aspect);
    // Changes with every new or changed aspect.
    uint64_t Version() const noexcept { return version_.load(std::memory_order_acquire); }
    // Probe these files in the background, replacing the previous request
    // (only the folder on screen matters). notify is invalidated as results
    // come in, at most every 150 ms.
    void Probe(std::vector<std::pair<uint64_t, std::wstring>> files, HWND notify);
    size_t SizeForTest() const;

private:
    WallAspects() = default;
    static void CALLBACK Work(PTP_CALLBACK_INSTANCE, void* context);
    void Run();
    bool Store(uint64_t hash, float aspect, float tolerance);

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, float> known_;
    std::atomic<uint64_t> version_{1};
    std::vector<std::pair<uint64_t, std::wstring>> queue_;  // probed from the back
    HWND notify_ = nullptr;
    bool running_ = false;
};

}  // namespace pulse::ui
