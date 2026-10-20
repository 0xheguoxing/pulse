// icc_profile.h - the facts the preview host reads from an embedded ICC
// profile: its colour space and whether it is sRGB by name.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pulse::preview::icc {

// Larger embedded profiles are ignored (press CMYK runs ~0.5 MB).
constexpr size_t kProfileLimit = 4u * 1024u * 1024u;

enum class Space { Other, Rgb, Gray, Cmyk };

inline uint32_t Be32(const unsigned char* p) noexcept {
    return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
}

inline Space ColourSpace(const unsigned char* data, size_t size) noexcept {
    if (size < 132) return Space::Other;
    switch (Be32(data + 16)) {
    case 0x52474220: return Space::Rgb;    // 'RGB '
    case 0x47524159: return Space::Gray;   // 'GRAY'
    case 0x434D594B: return Space::Cmyk;   // 'CMYK'
    default: return Space::Other;
    }
}

// The profile description in ASCII: the 'desc' text (v2) or the first
// 'mluc' record (v4). Empty when the tag is missing or out of bounds.
inline std::string Description(const unsigned char* data, size_t size) {
    if (size < 132) return {};
    const uint32_t count = Be32(data + 128);
    for (uint32_t i = 0; i < count && i < 100 && 144 + 12 * size_t{i} <= size; ++i) {
        const unsigned char* entry = data + 132 + 12 * size_t{i};
        if (Be32(entry) != 0x64657363) continue;   // 'desc'
        const size_t offset = Be32(entry + 4), length = Be32(entry + 8);
        if (offset > size || length > size - offset || length < 12) return {};
        const unsigned char* tag = data + offset;
        std::string text;
        if (Be32(tag) == 0x64657363) {               // textDescriptionType
            const size_t chars = (std::min)(size_t{Be32(tag + 8)}, length - 12);
            for (size_t k = 0; k < chars && tag[12 + k]; ++k) text += static_cast<char>(tag[12 + k]);
        } else if (Be32(tag) == 0x6D6C7563 && length >= 28) {   // multiLocalizedUnicodeType, UTF-16BE
            const size_t chars = Be32(tag + 20), at = Be32(tag + 24);
            if (at <= length && chars <= length - at)
                for (size_t k = 0; k + 1 < chars; k += 2)
                    text += tag[at + k] ? '?' : static_cast<char>(tag[at + k + 1]);
        }
        return text;
    }
    return {};
}

// sRGB by name ("sRGB IEC61966-2.1", "sRGB built-in", ...); a linear sRGB
// profile is not. Pixels tagged so are shown as they are, since converting
// sRGB to sRGB only adds rounding. A misnamed profile is shown unconverted,
// as every profile was before.
inline bool IsSrgb(const unsigned char* data, size_t size) {
    if (ColourSpace(data, size) != Space::Rgb) return false;
    const std::string name = Description(data, size);
    return name.find("sRGB") != std::string::npos && name.find("inear") == std::string::npos;
}

} // namespace pulse::preview::icc
