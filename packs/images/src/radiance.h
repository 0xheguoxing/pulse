// Radiance RGBE / XYZE (.hdr, .pic) reader of the image preview pack.
//
// Streams scanlines straight from the mapped file: no full-size float copy,
// so a 16K panorama needs a few rows of memory instead of 2 GB (stb_image
// refused those outright and only knew one of the eight orientations).
// Reads what Radiance itself writes and what other tools write:
//   - any "#?PROGRAM" magic (RADIANCE, RGBE, ...) or none at all,
//   - LF or CRLF header lines,
//   - FORMAT=32-bit_rle_rgbe or 32-bit_rle_xyze (missing FORMAT means RGBE),
//   - all eight orientations ("-Y h +X w", "+Y h +X w", "+X w -Y h", ...),
//   - adaptive RLE, old-style RLE (1,1,1 repeat pixels) and flat scanlines.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace radiance {

enum class Result { Ok, NotRadiance, Unsupported, Damaged, TooLarge };

struct Header {
    uint32_t width = 0, height = 0;   // of the upright picture
    uint32_t scan_count = 0;          // scanlines in the file
    uint32_t scan_length = 0;         // pixels per scanline
    bool xyz = false;                 // FORMAT=32-bit_rle_xyze
    bool columns = false;             // scanlines run top-down ("+X w -Y h")
    bool reverse_scans = false;       // scanlines come last-first
    bool reverse_pixels = false;      // pixels within a scanline come last-first
    size_t data = 0;                  // offset of the first scanline
};

namespace detail {

inline bool IsNameChar(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '.' || c == ' ';
}

// One header line without its LF / CRLF; false at the end of the buffer.
inline bool NextLine(const uint8_t* d, size_t n, size_t& pos, const char*& text, size_t& length) {
    if (pos >= n) return false;
    const void* lf = std::memchr(d + pos, '\n', n - pos);
    if (!lf) return false;
    const size_t end = static_cast<size_t>(static_cast<const uint8_t*>(lf) - d);
    text = reinterpret_cast<const char*>(d + pos);
    length = end - pos;
    if (length && text[length - 1] == '\r') --length;
    pos = end + 1;
    return true;
}

inline bool StartsWith(const char* text, size_t length, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return length >= n && std::memcmp(text, prefix, n) == 0;
}

inline const char* SkipSpaces(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}

// "[-+][XY] <n>" -> sign, axis, n.
inline bool ParseAxis(const char*& p, const char* end, char& sign, char& axis, uint32_t& value) {
    p = SkipSpaces(p, end);
    if (end - p < 2 || (p[0] != '-' && p[0] != '+') || (p[1] != 'X' && p[1] != 'Y')) return false;
    sign = p[0];
    axis = p[1];
    p = SkipSpaces(p + 2, end);
    uint64_t v = 0;
    const char* digits = p;
    while (p < end && *p >= '0' && *p <= '9' && v <= (1u << 30)) v = v * 10 + static_cast<uint64_t>(*p++ - '0');
    if (p == digits || v == 0 || v > (1u << 30)) return false;
    value = static_cast<uint32_t>(v);
    return true;
}

// 2^(e-136): the scale of an 8-bit mantissa with exponent byte e.
inline const float* ExponentTable() {
    static const struct Table {
        float v[256];
        Table() {
            v[0] = 0.0f;
            for (int e = 1; e < 256; ++e) v[e] = static_cast<float>(std::ldexp(1.0, e - 136));
        }
    } table;
    return table.v;
}

// One scanline of `length` RGBE pixels into `scan`. `flat` files hold plain
// pixels only: a dark (1,1,1,e) pixel there is a colour, not a repeat marker.
inline bool ReadScanline(const uint8_t* d, size_t n, size_t& p, uint8_t* scan, uint32_t length, bool flat) {
    if (!flat && length >= 8 && length <= 0x7fff && n - p >= 4 && d[p] == 2 && d[p + 1] == 2 && !(d[p + 2] & 0x80)) {
        if (((static_cast<uint32_t>(d[p + 2]) << 8) | d[p + 3]) != length) return false;
        p += 4;
        for (int c = 0; c < 4; ++c) {
            uint32_t x = 0;
            while (x < length) {
                if (p >= n) return false;
                const uint32_t code = d[p++];
                if (code > 128) {
                    const uint32_t run = code - 128;
                    if (run > length - x || p >= n) return false;
                    const uint8_t value = d[p++];
                    for (uint32_t k = 0; k < run; ++k) scan[(x++) * 4 + c] = value;
                } else {
                    if (code == 0 || code > length - x || code > n - p) return false;
                    for (uint32_t k = 0; k < code; ++k) scan[(x++) * 4 + c] = d[p++];
                }
            }
        }
        return true;
    }
    // Flat or old-style RLE: a (1,1,1,count) pixel repeats the one before it;
    // consecutive repeat pixels make the count's next byte.
    uint32_t x = 0;
    int shift = 0;
    while (x < length) {
        if (n - p < 4) return false;
        const uint8_t* q = d + p;
        p += 4;
        if (!flat && x > 0 && q[0] == 1 && q[1] == 1 && q[2] == 1) {
            if (shift > 24) return false;
            const uint64_t count = static_cast<uint64_t>(q[3]) << shift;
            if (count > length - x) return false;
            for (uint64_t k = 0; k < count; ++k, ++x) std::memcpy(scan + x * 4, scan + (x - 1) * 4, 4);
            shift += 8;
        } else {
            std::memcpy(scan + x * 4, q, 4);
            ++x;
            shift = 0;
        }
    }
    return true;
}

// Radiance's own primaries (white E): XYZ -> linear RGB.
inline void XyzToRgb(float& x, float& y, float& z) {
    const float r = 2.5653128f * x - 1.1668496f * y - 0.3984632f * z;
    const float g = -1.0221082f * x + 1.9782866f * y + 0.0438216f * z;
    const float b = 0.0747244f * x - 0.2519396f * y + 1.1772152f * z;
    x = r; y = g; z = b;
}

} // namespace detail

// Radiance files start with "#?" and a program name, or (rarely) straight
// with the header variables.
inline bool LooksLikeRadiance(const uint8_t* d, size_t n) {
    if (n >= 3 && d[0] == '#' && d[1] == '?') {
        size_t i = 2;
        while (i < n && i < 66 && detail::IsNameChar(d[i])) ++i;
        if (i == 2 || i >= n) return false;
        return d[i] == '\n' || (d[i] == '\r' && i + 1 < n && d[i + 1] == '\n');
    }
    return n >= 18 && std::memcmp(d, "FORMAT=32-bit_rle_", 18) == 0;
}

inline Result ParseHeader(const uint8_t* d, size_t n, Header& h) {
    h = Header{};
    if (!LooksLikeRadiance(d, n)) return Result::NotRadiance;
    constexpr size_t kMaxHeader = 1u << 20;   // long command histories exist, not megabytes of them
    const size_t limit = n < kMaxHeader ? n : kMaxHeader;
    size_t pos = 0;
    const char* text = nullptr;
    size_t length = 0;
    if (d[0] == '#' && !detail::NextLine(d, limit, pos, text, length)) return Result::Damaged;
    for (;;) {
        if (!detail::NextLine(d, limit, pos, text, length)) return Result::Damaged;
        if (length == 0) break;
        if (detail::StartsWith(text, length, "FORMAT=")) {
            const char* value = detail::SkipSpaces(text + 7, text + length);
            size_t value_length = static_cast<size_t>(text + length - value);
            while (value_length && (value[value_length - 1] == ' ' || value[value_length - 1] == '\t')) --value_length;
            if (value_length == 15 && std::memcmp(value, "32-bit_rle_rgbe", 15) == 0) h.xyz = false;
            else if (value_length == 15 && std::memcmp(value, "32-bit_rle_xyze", 15) == 0) h.xyz = true;
            else return Result::Unsupported;
        }
    }
    if (!detail::NextLine(d, n, pos, text, length)) return Result::Damaged;
    const char* p = text;
    const char* end = text + length;
    char sign1, axis1, sign2, axis2;
    uint32_t count, scan;
    if (!detail::ParseAxis(p, end, sign1, axis1, count) || !detail::ParseAxis(p, end, sign2, axis2, scan) ||
        axis1 == axis2 || detail::SkipSpaces(p, end) != end)
        return Result::Damaged;
    h.scan_count = count;
    h.scan_length = scan;
    h.columns = axis1 == 'X';
    h.width = h.columns ? count : scan;
    h.height = h.columns ? scan : count;
    // +Y points up, +X right: "-Y" scans start at the top, "+X" at the left.
    h.reverse_scans = h.columns ? sign1 == '-' : sign1 == '+';
    h.reverse_pixels = h.columns ? sign2 == '+' : sign2 == '-';
    h.data = pos;
    return Result::Ok;
}

// Calls row(y, rgba) once for every picture row (y counts from the top, in
// any order); rgba holds width linear-light pixels, alpha 1.
template <class RowFn>
Result Decode(const uint8_t* d, size_t n, const Header& h, RowFn&& row) {
    if (!h.scan_count || !h.scan_length || h.data > n) return Result::Damaged;
    const uint64_t pixels = static_cast<uint64_t>(h.scan_count) * h.scan_length;
    // Plain pixels fill the rest of the file exactly; anything shorter is RLE.
    const bool flat = n - h.data >= pixels * 4 &&
        !(h.scan_length >= 8 && h.scan_length <= 0x7fff && n - h.data >= 4 && d[h.data] == 2 && d[h.data + 1] == 2);
    const float* scale = detail::ExponentTable();
    std::vector<uint8_t> scan(static_cast<size_t>(h.scan_length) * 4);
    std::vector<float> line(static_cast<size_t>(h.width) * 4, 1.0f);
    std::vector<uint8_t> grid;   // columns only: the whole picture as RGBE, row-major
    if (h.columns) {
        if (pixels * 4 > (1ull << 30)) return Result::TooLarge;
        grid.resize(static_cast<size_t>(pixels) * 4);
    }
    const auto emit = [&](uint32_t y, const uint8_t* rgbe, bool reverse) {
        for (uint32_t x = 0; x < h.width; ++x) {
            const uint8_t* px = rgbe + static_cast<size_t>(reverse ? h.width - 1 - x : x) * 4;
            const float f = scale[px[3]];
            float* out = &line[static_cast<size_t>(x) * 4];
            out[0] = px[3] ? (px[0] + 0.5f) * f : 0.0f;
            out[1] = px[3] ? (px[1] + 0.5f) * f : 0.0f;
            out[2] = px[3] ? (px[2] + 0.5f) * f : 0.0f;
            if (h.xyz) detail::XyzToRgb(out[0], out[1], out[2]);
        }
        row(y, line.data());
    };
    size_t p = h.data;
    for (uint32_t i = 0; i < h.scan_count; ++i) {
        if (!detail::ReadScanline(d, n, p, scan.data(), h.scan_length, flat)) return Result::Damaged;
        const uint32_t s = h.reverse_scans ? h.scan_count - 1 - i : i;
        if (!h.columns) {
            emit(s, scan.data(), h.reverse_pixels);
            continue;
        }
        for (uint32_t k = 0; k < h.scan_length; ++k) {   // scanline s is column s
            const uint32_t y = h.reverse_pixels ? h.scan_length - 1 - k : k;
            std::memcpy(&grid[(static_cast<size_t>(y) * h.width + s) * 4], &scan[static_cast<size_t>(k) * 4], 4);
        }
    }
    if (h.columns)
        for (uint32_t y = 0; y < h.height; ++y) emit(y, &grid[static_cast<size_t>(y) * h.width * 4], false);
    return Result::Ok;
}

} // namespace radiance
