// icc_profile.h - the facts the preview host reads from an embedded ICC
// profile (its colour space, whether it is sRGB by name) and the fast
// conversion of matrix/TRC RGB profiles to sRGB.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__)
#include <emmintrin.h>
#define PULSE_ICC_SSE2 1
#endif

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

namespace detail {

inline uint16_t Be16(const unsigned char* p) noexcept { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline double S15Fixed16(const unsigned char* p) noexcept { return static_cast<int32_t>(Be32(p)) / 65536.0; }

// The tag with this signature, or nullptr when missing or out of bounds.
inline const unsigned char* FindTag(const unsigned char* data, size_t size, uint32_t signature,
                                    size_t& length) noexcept {
    if (size < 132) return nullptr;
    const uint32_t count = Be32(data + 128);
    for (uint32_t i = 0; i < count && i < 100 && 144 + 12 * size_t{i} <= size; ++i) {
        const unsigned char* entry = data + 132 + 12 * size_t{i};
        if (Be32(entry) != signature) continue;
        const size_t offset = Be32(entry + 4);
        length = Be32(entry + 8);
        return offset <= size && length <= size - offset ? data + offset : nullptr;
    }
    return nullptr;
}

// A tone curve ('curv' or 'para') sampled at the 256 device values.
inline bool SampleCurve(const unsigned char* tag, size_t length, float (&out)[256]) {
    if (!tag || length < 12) return false;
    const uint32_t type = Be32(tag);
    if (type == 0x63757276) {   // 'curv': identity, one gamma or a table
        const size_t n = Be32(tag + 8);
        if (length < 12 + 2 * n) return false;
        for (int i = 0; i < 256; ++i) {
            const double x = i / 255.0;
            double y = x;
            if (n == 1) {
                y = std::pow(x, Be16(tag + 12) / 256.0);
            } else if (n > 1) {
                const double at = x * static_cast<double>(n - 1);
                const size_t k = (std::min)(static_cast<size_t>(at), n - 2);
                const double f = at - static_cast<double>(k);
                y = (Be16(tag + 12 + 2 * k) * (1 - f) + Be16(tag + 14 + 2 * k) * f) / 65535.0;
            }
            out[i] = static_cast<float>(std::clamp(y, 0.0, 1.0));
        }
        return true;
    }
    if (type == 0x70617261) {   // 'para': ICC parametric function types 0-4
        static constexpr int kParameters[] = {1, 3, 4, 5, 7};
        const unsigned fn = Be16(tag + 8);
        if (fn > 4 || length < 12 + 4 * size_t{static_cast<unsigned>(kParameters[fn])}) return false;
        double p[7]{};
        for (int k = 0; k < kParameters[fn]; ++k) p[k] = S15Fixed16(tag + 12 + 4 * k);
        const double g = p[0], a = p[1], b = p[2], c = p[3], d = p[4], e = p[5], f = p[6];
        if ((fn == 1 || fn == 2) && a == 0) return false;
        for (int i = 0; i < 256; ++i) {
            const double x = i / 255.0;
            const auto power = [&](double v) { return std::pow((std::max)(v, 0.0), g); };
            double y = 0;
            switch (fn) {
            case 0: y = std::pow(x, g); break;
            case 1: y = x >= -b / a ? power(a * x + b) : 0; break;
            case 2: y = x >= -b / a ? power(a * x + b) + c : c; break;
            case 3: y = x >= d ? power(a * x + b) : c * x; break;
            default: y = x >= d ? power(a * x + b) + e : c * x + f; break;
            }
            out[i] = static_cast<float>(std::clamp(y, 0.0, 1.0));
        }
        return true;
    }
    return false;
}

// Linear light -> sRGB byte, 16384 steps (under 0.25 of a byte near black).
inline const std::array<uint8_t, 16384>& SrgbEncode() {
    static const std::array<uint8_t, 16384> table = [] {
        std::array<uint8_t, 16384> t{};
        for (size_t i = 0; i < t.size(); ++i) {
            const double v = static_cast<double>(i) / (t.size() - 1);
            const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
            t[i] = static_cast<uint8_t>(std::lround(std::clamp(s, 0.0, 1.0) * 255));
        }
        return t;
    }();
    return table;
}

} // namespace detail

// Matrix/TRC RGB profiles (Adobe RGB, Display P3, ProPhoto, most camera and
// display profiles) converted to sRGB without the system colour engine:
// each channel's curve to linear light, one 3x3 matrix through the D50
// connection space, then the sRGB curve. For these profiles perceptual and
// relative colorimetric are the same transform, so the result matches WCS
// and LittleCMS while running several times faster. Profiles with lookup
// tables (A2B0..2) are left to WCS, which uses them.
class MatrixToSrgb {
public:
    bool Init(const unsigned char* data, size_t size) {
        if (ColourSpace(data, size) != Space::Rgb || Be32(data + 20) != 0x58595A20) return false;   // PCS 'XYZ '
        size_t length = 0;
        for (const uint32_t table : {0x41324230u, 0x41324231u, 0x41324232u})   // 'A2B0'..'A2B2'
            if (detail::FindTag(data, size, table, length)) return false;
        static constexpr uint32_t kColorants[3] = {0x7258595A, 0x6758595A, 0x6258595A};   // 'rXYZ' 'gXYZ' 'bXYZ'
        static constexpr uint32_t kCurves[3] = {0x72545243, 0x67545243, 0x62545243};      // 'rTRC' 'gTRC' 'bTRC'
        double source[3][3]{};
        for (int c = 0; c < 3; ++c) {
            const unsigned char* xyz = detail::FindTag(data, size, kColorants[c], length);
            if (!xyz || length < 20 || Be32(xyz) != 0x58595A20) return false;
            for (int r = 0; r < 3; ++r) source[r][c] = detail::S15Fixed16(xyz + 8 + 4 * r);
            const unsigned char* curve = detail::FindTag(data, size, kCurves[c], length);
            if (!detail::SampleCurve(curve, length, decode_[c])) return false;
        }
        // sRGB IEC61966-2.1 colorants (D50), as in the system's sRGB profile.
        static constexpr double kSrgb[3][3] = {{0.436066, 0.385147, 0.143066},
                                               {0.222488, 0.716873, 0.060608},
                                               {0.013916, 0.097076, 0.714096}};
        const auto& m = kSrgb;
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        const double inverse[3][3] = {
            {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det,
             (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det},
            {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det,
             (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det},
            {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det,
             (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                matrix_[r][c] = static_cast<float>(inverse[r][0] * source[0][c] + inverse[r][1] * source[1][c] +
                                                   inverse[r][2] * source[2][c]);
        return true;
    }

    // Straight (not premultiplied) BGRA in place; alpha is left alone. Runs
    // of one colour (flat artwork, screenshots, logos) reuse the previous
    // result; the system transform does the same, which made it faster than
    // a plain per-pixel loop on such pictures. The matrix and tables are read
    // into locals: stores through the byte pointer may alias anything.
    void Apply(unsigned char* bgra, size_t count) const {
        const uint8_t* encode = detail::SrgbEncode().data();
        const float* red = decode_[0];
        const float* green = decode_[1];
        const float* blue = decode_[2];
        uint32_t last_in = 0xFFFFFFFFu;   // never a 24-bit colour
        uint8_t out_b = 0, out_g = 0, out_r = 0;
#ifdef PULSE_ICC_SSE2
        // Lanes 0..2 hold the red, green and blue results of one pixel.
        const __m128 column_r = _mm_setr_ps(matrix_[0][0], matrix_[1][0], matrix_[2][0], 0.0f);
        const __m128 column_g = _mm_setr_ps(matrix_[0][1], matrix_[1][1], matrix_[2][1], 0.0f);
        const __m128 column_b = _mm_setr_ps(matrix_[0][2], matrix_[1][2], matrix_[2][2], 0.0f);
        const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
        const __m128 steps = _mm_set1_ps(16383.0f), half = _mm_set1_ps(0.5f);
#else
        const float m00 = matrix_[0][0], m01 = matrix_[0][1], m02 = matrix_[0][2];
        const float m10 = matrix_[1][0], m11 = matrix_[1][1], m12 = matrix_[1][2];
        const float m20 = matrix_[2][0], m21 = matrix_[2][1], m22 = matrix_[2][2];
        const auto byte = [&](float v) {
            return encode[static_cast<int>((std::min)((std::max)(v, 0.0f), 1.0f) * 16383.0f + 0.5f)];
        };
#endif
        for (size_t i = 0; i < count; ++i, bgra += 4) {
            const uint32_t colour = uint32_t{bgra[0]} | uint32_t{bgra[1]} << 8 | uint32_t{bgra[2]} << 16;
            if (colour != last_in) {
                last_in = colour;
#ifdef PULSE_ICC_SSE2
                __m128 v = _mm_add_ps(_mm_add_ps(_mm_mul_ps(column_r, _mm_set1_ps(red[bgra[2]])),
                                                 _mm_mul_ps(column_g, _mm_set1_ps(green[bgra[1]]))),
                                      _mm_mul_ps(column_b, _mm_set1_ps(blue[bgra[0]])));
                v = _mm_add_ps(_mm_mul_ps(_mm_min_ps(_mm_max_ps(v, zero), one), steps), half);
                alignas(16) int32_t index[4];
                _mm_store_si128(reinterpret_cast<__m128i*>(index), _mm_cvttps_epi32(v));
                out_r = encode[index[0]];
                out_g = encode[index[1]];
                out_b = encode[index[2]];
#else
                const float r = red[bgra[2]], g = green[bgra[1]], b = blue[bgra[0]];
                out_r = byte(m00 * r + m01 * g + m02 * b);
                out_g = byte(m10 * r + m11 * g + m12 * b);
                out_b = byte(m20 * r + m21 * g + m22 * b);
#endif
            }
            bgra[0] = out_b;
            bgra[1] = out_g;
            bgra[2] = out_r;
        }
    }

private:
    float decode_[3][256]{};
    float matrix_[3][3]{};
};

} // namespace pulse::preview::icc
