// shell_bitmap.h - Shell thumbnail bitmaps (IThumbnailProvider, the
// thumbnail cache, IShellItemImageFactory) as top-down premultiplied BGRA.
#pragma once

#include <windows.h>
#include <thumbcache.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulse::preview {

// What the alpha byte of a Shell bitmap means depends on the provider, and
// its WTS_ALPHATYPE label alone does not settle it:
//  - WTSAT_RGB promises no transparency, so the alpha byte is ignored and the
//    bitmap shown opaque. Explorer's photo provider labels WebP and PNG
//    thumbnails this way yet keeps their straight alpha (white, zero-alpha
//    pixels), which Explorer also shows over white.
//  - Otherwise, premultiplied pixels never exceed their alpha, so a colour
//    channel above it marks straight alpha (providers labelled ARGB, or the
//    thumbnail cache's copy of a photo-provider result); that bitmap is
//    premultiplied here. An all-zero alpha plane is an opaque bitmap.
// Reading straight pixels as premultiplied added them onto the background:
// washed-out or white thumbnails.
inline void NormalizeShellAlpha(std::vector<uint8_t>& px, WTS_ALPHATYPE alpha_type) {
    bool any_alpha = false, straight = false;
    if (alpha_type != WTSAT_RGB) {
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            const uint8_t a = px[i + 3];
            if (a) any_alpha = true;
            if (px[i] > a || px[i + 1] > a || px[i + 2] > a) straight = true;
        }
    }
    if (!any_alpha) {
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        return;
    }
    if (!straight) return;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        const uint32_t a = px[i + 3];
        if (a == 255) continue;
        for (size_t c = 0; c < 3; ++c)
            px[i + c] = static_cast<uint8_t>((px[i + c] * a + 127u) / 255u);
    }
}

// Any HBITMAP - DIB section or device bitmap, any depth - copied top-down as
// 32bpp and normalized as above. Bitmaps over 16384 px on a side are refused.
inline bool ShellBitmapToPbgra(HBITMAP bitmap, WTS_ALPHATYPE alpha_type, std::vector<uint8_t>& out,
                               UINT& width, UINT& height, UINT& stride) {
    out.clear();
    BITMAP info{};
    if (!bitmap || !GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight == 0)
        return false;
    const LONG w = info.bmWidth;
    const LONG h = info.bmHeight < 0 ? -info.bmHeight : info.bmHeight;
    if (w > 16384 || h > 16384) return false;
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = w;
    header.bmiHeader.biHeight = -h;   // top-down
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    HDC dc = CreateCompatibleDC(nullptr);
    const int rows = dc ? GetDIBits(dc, bitmap, 0, static_cast<UINT>(h), out.data(), &header,
                                    DIB_RGB_COLORS) : 0;
    if (dc) DeleteDC(dc);
    if (rows != h) {
        out.clear();
        return false;
    }
    NormalizeShellAlpha(out, alpha_type);
    width = static_cast<UINT>(w);
    height = static_cast<UINT>(h);
    stride = width * 4;
    return true;
}

} // namespace pulse::preview
