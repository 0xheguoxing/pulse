#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulse::ui {

struct IconArtworkBounds {
    float left = 0.0f;
    float top = 0.0f;
    float right = 1.0f;
    float bottom = 1.0f;
    // Nonzero: the icon has no image this large, only one of this many
    // pixels a side (UndersizedIconSide); it is drawn at most twice that.
    uint32_t native_side = 0;
};

// Pixel edges, not pixel centers: a fully opaque image occupies [0, 1].
inline IconArtworkBounds MeasureIconArtwork(std::span<const uint8_t> bgra,
                                           size_t width, size_t height,
                                           size_t stride = 0) noexcept {
    if (!width || !height || width > bgra.size() / 4 / height) return {};
    if (!stride) stride = width * 4;
    if (stride < width * 4 || height > bgra.size() / stride) return {};
    size_t left = width, top = height, right = 0, bottom = 0;
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            if (!bgra[y * stride + x * 4 + 3]) continue;
            if (x < left) left = x;
            if (y < top) top = y;
            if (x + 1 > right) right = x + 1;
            if (y + 1 > bottom) bottom = y + 1;
        }
    }
    if (!right || !bottom) return {};
    return { static_cast<float>(left) / static_cast<float>(width),
             static_cast<float>(top) / static_cast<float>(height),
             static_cast<float>(right) / static_cast<float>(width),
             static_cast<float>(bottom) / static_cast<float>(height) };
}

// The shell's 256-pixel image list has nothing larger for an icon without a
// 256-pixel image: it holds the icon's largest image, unscaled, in the
// top-left corner of an otherwise transparent frame, which grid views then
// showed small in the cell's bottom-left corner. The side of that image (a
// standard icon size), or 0 when the artwork reaches past the frame's
// top-left quarter, as real large artwork does.
inline uint32_t UndersizedIconSide(const IconArtworkBounds& bounds, size_t width, size_t height) noexcept {
    if (width != height || width < 128 || bounds.right > 0.5f || bounds.bottom > 0.5f) return 0;
    const float extent = (std::max)(bounds.right, bounds.bottom) * static_cast<float>(width);
    for (const uint32_t side : {16u, 20u, 24u, 32u, 40u, 48u, 64u, 72u, 96u, 128u})
        if (extent <= static_cast<float>(side) + 0.01f) return side * 2 <= width ? side : 0;
    return 0;
}

// An undersized icon (UndersizedIconSide) cut to its own image, with its
// artwork measured anew and its native side recorded; anything else is left
// as it is. `bgra` is tightly packed, `width` x `height`.
inline void CropUndersizedIcon(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
                               IconArtworkBounds& bounds) {
    const uint32_t side = UndersizedIconSide(bounds, width, height);
    if (!side || bgra.size() < static_cast<size_t>(width) * height * 4u) return;
    std::vector<uint8_t> cropped(static_cast<size_t>(side) * side * 4u);
    for (uint32_t y = 0; y < side; ++y)
        std::copy_n(bgra.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(y) * width * 4u), side * 4u,
                    cropped.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(y) * side * 4u));
    bgra = std::move(cropped);
    width = height = side;
    bounds = MeasureIconArtwork(bgra, side, side);
    bounds.native_side = side;
}

// The side an icon is drawn at in a square of `cell` pixels: one with only a
// small image grows to at most twice its size, so it is not blurred past
// recognition; anything else fills the square.
inline float IconDrawSide(float cell, uint32_t native_side) noexcept {
    return native_side ? (std::min)(cell, 2.0f * static_cast<float>(native_side)) : cell;
}

} // namespace pulse::ui
