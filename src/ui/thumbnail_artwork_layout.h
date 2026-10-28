#pragma once
#include "icon_artwork_bounds.h"
#include "../common/preview_extensions.h"
#include <d2d1.h>
#include <d2d1helper.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

namespace pulse::ui {
inline D2D1_RECT_F ThumbnailArtworkRect(const D2D1_RECT_F& target,
                                      const IconArtworkBounds& bounds) {
    const float width = target.right - target.left;
    const float height = target.bottom - target.top;
    return D2D1::RectF(target.left + width * bounds.left,
        target.top + height * bounds.top, target.left + width * bounds.right,
        target.top + height * bounds.bottom);
}

// Pictures - photos, camera RAW, PSD documents - fill their own canvas:
// transparent margins are part of the image, so grid bottom alignment rests
// the whole bitmap on the baseline. Measuring their alpha ink pushed
// cut-outs with an empty lower band (Photoshop WebP / PNG exports) below
// their neighbours. Icon files and shell or document artwork keep the ink.
inline bool IsPictureCanvasPath(std::wstring_view path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos || (slash != std::wstring_view::npos && dot < slash))
        return false;
    std::wstring ext(path.substr(dot));
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    if (ext == L".ico" || ext == L".cur") return false;
    return ::pulse::preview::IsImageExtension(ext) || ::pulse::preview::IsPsdExtension(ext) ||
           ::pulse::preview::IsFamilyExtension(::pulse::preview::PreviewFamily::Raw, ext);
}

inline IconArtworkBounds ThumbnailArtworkBounds(std::wstring_view path,
    std::span<const uint8_t> bgra, size_t width, size_t height, size_t stride) {
    return IsPictureCanvasPath(path) ? IconArtworkBounds{}
                                     : MeasureIconArtwork(bgra, width, height, stride);
}

inline D2D1_RECT_F ContainedThumbnailRect(const D2D1_RECT_F& dest, uint32_t w, uint32_t h,
    const IconArtworkBounds& bounds, bool align_artwork_bottom) {
    const float dest_width = (std::max)(1.0f, dest.right - dest.left);
    const float dest_height = (std::max)(1.0f, dest.bottom - dest.top);
    D2D1_RECT_F fitted = dest;
    const float source_aspect = static_cast<float>(w) / static_cast<float>((std::max)(1u, h));
    const float dest_aspect = dest_width / dest_height;
    if (source_aspect > dest_aspect) {
        const float height = dest_width / source_aspect;
        const float center = (dest.top + dest.bottom) * 0.5f;
        fitted.top = center - height * 0.5f;
        fitted.bottom = center + height * 0.5f;
    } else if (source_aspect < dest_aspect) {
        const float width = dest_height * source_aspect;
        const float center = (dest.left + dest.right) * 0.5f;
        fitted.left = center - width * 0.5f;
        fitted.right = center + width * 0.5f;
    }
    if (align_artwork_bottom) {
        const float offset = dest.bottom - ThumbnailArtworkRect(fitted, bounds).bottom;
        fitted.top += offset;
        fitted.bottom += offset;
    }
    return fitted;
}
} // namespace pulse::ui
