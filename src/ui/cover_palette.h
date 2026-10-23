#pragma once
// Colours of a cover picture for the Quick Look audio waveform: up to three
// distinct, reasonably saturated hues, ordered by where they sit on the
// cover from left to right, so the waveform's gradient echoes the artwork.
// Header-only: every target that compiles thumbnail_cache.cpp gets it free.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulse::ui {

struct CoverPalette {
    bool valid = false;        // false for grey / black-and-white / empty covers
    uint32_t colors[3]{};      // 0xRRGGBB, gradient stops left to right
    bool operator==(const CoverPalette&) const = default;
};

namespace cover_palette_detail {
struct Hsl { float h, s, l; };   // h in [0,360)

inline Hsl ToHsl(float r, float g, float b) {
    const float mx = (std::max)({r, g, b}), mn = (std::min)({r, g, b});
    Hsl out{0.0f, 0.0f, (mx + mn) * 0.5f};
    const float d = mx - mn;
    if (d < 1e-5f) return out;
    out.s = out.l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == r) out.h = std::fmod((g - b) / d + 6.0f, 6.0f);
    else if (mx == g) out.h = (b - r) / d + 2.0f;
    else out.h = (r - g) / d + 4.0f;
    out.h *= 60.0f;
    return out;
}

inline uint32_t FromHsl(Hsl c) {
    const auto channel = [&](float n) {
        const float k = std::fmod(n + c.h / 30.0f, 12.0f);
        const float a = c.s * (std::min)(c.l, 1.0f - c.l);
        const float v = c.l - a * (std::max)(-1.0f, (std::min)({k - 3.0f, 9.0f - k, 1.0f}));
        return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return (channel(0) << 16) | (channel(8) << 8) | channel(4);
}

inline float HueDistance(float a, float b) {
    const float d = std::fabs(a - b);
    return (std::min)(d, 360.0f - d);
}

inline constexpr int kBins = 24;            // 15 degrees each
struct Bin { double weight = 0, r = 0, g = 0, b = 0, x = 0; };
} // namespace cover_palette_detail

// bgra: premultiplied BGRA rows (as the preview host sends them). Samples at
// most 48 x 48 points, so it is cheap enough for every decoded cover.
inline CoverPalette ComputeCoverPalette(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride) {
    using namespace cover_palette_detail;
    CoverPalette out;
    if (!bgra || width < 2 || height < 2 || stride < width * 4) return out;
    const uint32_t nx = (std::min)(width, 48u), ny = (std::min)(height, 48u);
    Bin bins[kBins];
    double total = 0;
    for (uint32_t j = 0; j < ny; ++j) {
        const uint32_t y = static_cast<uint32_t>((j + 0.5) * height / ny);
        const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
        for (uint32_t i = 0; i < nx; ++i) {
            const uint32_t x = static_cast<uint32_t>((i + 0.5) * width / nx);
            const uint8_t* p = row + x * 4;
            if (p[3] < 128) continue;
            const float a = p[3] / 255.0f;   // un-premultiply
            const float r = (std::min)(1.0f, p[2] / 255.0f / a), g = (std::min)(1.0f, p[1] / 255.0f / a),
                        b = (std::min)(1.0f, p[0] / 255.0f / a);
            const Hsl c = ToHsl(r, g, b);
            // Greys, near-black and near-white carry no colour worth echoing.
            if (c.s < 0.2f || c.l < 0.12f || c.l > 0.92f) continue;
            const double w = c.s * (1.0 - std::fabs(c.l - 0.5) * 1.4);
            if (w <= 0) continue;
            Bin& bin = bins[static_cast<int>(c.h / (360.0f / kBins)) % kBins];
            bin.weight += w; bin.r += r * w; bin.g += g * w; bin.b += b * w;
            bin.x += (static_cast<double>(x) / width) * w;
            total += w;
        }
    }
    // Colour must cover a meaningful part of the picture.
    if (total < nx * ny * 0.08) return out;
    // Merge each bin with its neighbours so a hue split across two bins wins.
    struct Pick { double weight; float h; uint32_t rgb; double x; };
    Pick picks[3];
    int count = 0;
    bool used[kBins]{};
    for (int round = 0; round < 3; ++round) {
        int best = -1;
        double best_w = 0;
        for (int k = 0; k < kBins; ++k) {
            if (used[k]) continue;
            const double w = bins[k].weight + 0.5 * (bins[(k + kBins - 1) % kBins].weight + bins[(k + 1) % kBins].weight);
            if (w > best_w) { best_w = w; best = k; }
        }
        if (best < 0) break;
        Bin merged;
        for (int d = -1; d <= 1; ++d) {
            const Bin& b = bins[(best + d + kBins) % kBins];
            merged.weight += b.weight; merged.r += b.r; merged.g += b.g; merged.b += b.b; merged.x += b.x;
        }
        for (int d = -1; d <= 1; ++d) used[(best + d + kBins) % kBins] = true;
        if (merged.weight <= 0) break;
        const float r = static_cast<float>(merged.r / merged.weight), g = static_cast<float>(merged.g / merged.weight),
                    b = static_cast<float>(merged.b / merged.weight);
        const Hsl c = ToHsl(r, g, b);
        bool distinct = true;
        for (int i = 0; i < count; ++i) distinct = distinct && HueDistance(picks[i].h, c.h) >= 30.0f;
        // Secondary colours must be visible, not a few stray pixels.
        if (count && merged.weight < picks[0].weight * 0.12) break;
        if (!distinct) continue;
        picks[count++] = Pick{merged.weight, c.h, FromHsl(c), merged.x / merged.weight};
    }
    if (!count) return out;
    if (count == 1) {
        // One hue: a gentle shift either side still reads as a gradient.
        Hsl c = ToHsl(((picks[0].rgb >> 16) & 255) / 255.0f, ((picks[0].rgb >> 8) & 255) / 255.0f,
                      (picks[0].rgb & 255) / 255.0f);
        Hsl left = c, right = c;
        left.h = std::fmod(c.h + 348.0f, 360.0f); left.l = (std::min)(0.85f, c.l + 0.08f);
        right.h = std::fmod(c.h + 14.0f, 360.0f); right.l = (std::max)(0.15f, c.l - 0.08f);
        out.colors[0] = FromHsl(left); out.colors[1] = picks[0].rgb; out.colors[2] = FromHsl(right);
        out.valid = true;
        return out;
    }
    std::sort(picks, picks + count, [](const Pick& a, const Pick& b) { return a.x < b.x; });
    if (count == 2) {
        out.colors[0] = picks[0].rgb; out.colors[2] = picks[1].rgb;
        // Middle stop: the halfway hue, so the blend does not pass through grey.
        const auto hsl = [](uint32_t v) { return ToHsl(((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f); };
        const Hsl a = hsl(picks[0].rgb), b = hsl(picks[1].rgb);
        float dh = b.h - a.h;
        if (dh > 180.0f) dh -= 360.0f;
        if (dh < -180.0f) dh += 360.0f;
        out.colors[1] = FromHsl(Hsl{std::fmod(a.h + dh * 0.5f + 360.0f, 360.0f), (a.s + b.s) * 0.5f, (a.l + b.l) * 0.5f});
    } else {
        for (int i = 0; i < 3; ++i) out.colors[i] = picks[i].rgb;
    }
    out.valid = true;
    return out;
}

// A stop adjusted for the theme: lighter on dark, deeper on light, never grey.
inline uint32_t CoverPaletteThemed(uint32_t rgb, bool dark) {
    using namespace cover_palette_detail;
    Hsl c = ToHsl(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f);
    c.s = (std::max)(c.s, 0.45f);
    c.l = dark ? std::clamp(c.l, 0.5f, 0.8f) : std::clamp(c.l, 0.3f, 0.6f);
    // HSL lightness is not what the eye sees (blue at L 0.5 is far darker than
    // yellow), so walk the lightness until the perceived luma fits the theme.
    const auto luma = [](uint32_t v) {
        return (((v >> 16) & 255) * 0.299f + ((v >> 8) & 255) * 0.587f + (v & 255) * 0.114f) / 255.0f;
    };
    for (int i = 0; i < 24; ++i) {
        const float y = luma(FromHsl(c));
        if (dark ? y >= 0.45f || c.l >= 0.84f : y <= 0.62f || c.l <= 0.26f) break;
        c.l += dark ? 0.02f : -0.02f;
    }
    return FromHsl(c);
}

// Quick Look's image info drawer: an RGB histogram (64 bins per channel,
// scaled so the fullest bin is 1000) and whether any pixel is translucent.
// Measured on at most 256 x 256 samples, off the UI thread.
struct ImageStats {
    bool valid = false;
    bool has_alpha = false;
    uint16_t r[64]{}, g[64]{}, b[64]{};
};

inline ImageStats ComputeImageStats(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride) {
    ImageStats out;
    if (!bgra || !width || !height || stride < width * 4) return out;
    const uint32_t step_x = (std::max)(1u, width / 256), step_y = (std::max)(1u, height / 256);
    uint32_t counts[3][64]{};
    uint32_t samples = 0;
    for (uint32_t y = 0; y < height; y += step_y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width; x += step_x) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            const uint32_t a = p[3];
            if (a < 250) out.has_alpha = true;
            if (a == 0) continue;
            // Premultiplied: undo it so a faded edge counts as its real colour.
            auto channel = [a](uint8_t v) { return a == 255 ? v : (std::min)(255u, v * 255u / a); };
            ++counts[0][channel(p[2]) >> 2];
            ++counts[1][channel(p[1]) >> 2];
            ++counts[2][channel(p[0]) >> 2];
            ++samples;
        }
    }
    if (!samples) return out;
    uint32_t peak = 1;
    for (const auto& channel : counts)
        for (uint32_t v : channel) peak = (std::max)(peak, v);
    for (int i = 0; i < 64; ++i) {
        out.r[i] = static_cast<uint16_t>(counts[0][i] * 1000ull / peak);
        out.g[i] = static_cast<uint16_t>(counts[1][i] * 1000ull / peak);
        out.b[i] = static_cast<uint16_t>(counts[2][i] * 1000ull / peak);
    }
    out.valid = true;
    return out;
}

} // namespace pulse::ui
