// menu_icon.h — bitmap icon of one context-menu row, shared by pulse_shell's
// answer (Explorer handler bitmaps), the ops/ipc layers and the menu renderer.
//
// Pixels are 32bpp premultiplied BGRA, top-down, width * 4 bytes per row:
// what WIC's 32bppPBGRA converter and a D2D B8G8R8A8 premultiplied bitmap
// both use, so no layer converts twice. Rows share one buffer through
// MenuIconRef; the renderer keys its device bitmaps by that pointer.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace pulse::common {

constexpr uint32_t kMaxMenuIconEdge = 64;  // 16 DIP at up to 400% scale

struct MenuIconPixels {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;

    bool Valid() const noexcept {
        return width > 0 && height > 0 && width <= kMaxMenuIconEdge &&
               height <= kMaxMenuIconEdge &&
               bgra.size() == static_cast<size_t>(width) * height * 4u;
    }
};

using MenuIconRef = std::shared_ptr<const MenuIconPixels>;

} // namespace pulse::common
