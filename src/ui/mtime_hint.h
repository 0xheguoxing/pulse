#pragma once
// 修改时间提示: how recently a row changed, without touching the order.
// The list marks rows changed within a range (dots, bars or bold text only);
// buckets follow change tracking's wording (刚刚 / 今天 / 昨天 / N 天前) and
// each one has its own shape, so color is never the only cue.
#include <d2d1_1.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace pulse::ui {

struct MtimeHint {
    int style = 1;            // 0 off, 1 dots, 2 bars, 3 text only
    int range_days = 7;       // 1 (today), 3, 7, 30
    int palette = 0;          // kMtimePaletteCount entries; 0 follows the accent
    bool fade = true;         // dates outside the range step back
    bool tree = true;         // folders take the newest change inside (change tracking)
    bool other_views = true;  // icon / list views mark the icon's top-left corner
    bool bold = true;         // names and dates changed today are semibold
    bool flash = true;        // a row whose file just changed glows once
    bool operator==(const MtimeHint&) const = default;
};

constexpr int kMtimePaletteCount = 6;
constexpr int kMtimeBucketNone = 6;
constexpr uint64_t kMtimeSecond = 10'000'000ull;   // FILETIME ticks
constexpr uint64_t kMtimeDay = 86400ull * kMtimeSecond;
// Dots sit in the date cell's lead; the text starts this much further right.
constexpr float kMtimeDotLeadDip = 12.0f;

inline int ClampMtimeRange(int days) { return days <= 1 ? 1 : days <= 3 ? 3 : days <= 7 ? 7 : 30; }
// The oldest bucket a range still marks.
inline int MtimeRangeLastBucket(int days) {
    switch (ClampMtimeRange(days)) {
    case 1: return 1;
    case 3: return 3;
    case 7: return 4;
    default: return 5;
    }
}

// FILETIME of the local midnight that starts today (UTC ticks).
inline uint64_t MtimeLocalMidnight(uint64_t now) {
    FILETIME ft{static_cast<DWORD>(now), static_cast<DWORD>(now >> 32)};
    SYSTEMTIME utc{}, local{}, midnight{};
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return now;
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &midnight) || !SystemTimeToFileTime(&midnight, &ft))
        return now;
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

struct MtimeClock {
    uint64_t now = 0, midnight = 0;
    static MtimeClock Now() {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        MtimeClock c;
        c.now = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        c.midnight = MtimeLocalMidnight(c.now);
        return c;
    }
};

// 0 刚刚 (5 min), 1 今天, 2 昨天, 3 three days, 4 seven days, 5 thirty days,
// 6 older. `next` (when given) is lowered to the time this row's bucket
// changes, so the list repaints exactly then.
inline int MtimeBucket(uint64_t time, const MtimeClock& clock, uint64_t* next = nullptr) {
    if (!time) return kMtimeBucketNone;
    const auto until = [&](uint64_t at) { if (next && at > clock.now) *next = (std::min)(*next, at); };
    const uint64_t tomorrow = clock.midnight + kMtimeDay;
    if (time >= clock.now) { until(time + 300 * kMtimeSecond); return 0; }   // clock skew: still new
    const uint64_t age = clock.now - time;
    if (age < 300 * kMtimeSecond) { until(time + 300 * kMtimeSecond); return 0; }
    if (time >= clock.midnight) { until(tomorrow); return 1; }
    if (clock.midnight >= kMtimeDay && time >= clock.midnight - kMtimeDay) { until(tomorrow); return 2; }
    if (age < 3 * kMtimeDay) { until(time + 3 * kMtimeDay); return 3; }
    if (age < 7 * kMtimeDay) { until(time + 7 * kMtimeDay); return 4; }
    if (age < 30 * kMtimeDay) { until(time + 30 * kMtimeDay); return 5; }
    return kMtimeBucketNone;
}

// Single-hue palettes step down in strength with age; the multi-hue ones
// (冷暖, 极光) carry age in the hue and stay nearly opaque.
struct MtimePalette {
    uint32_t light[6];
    uint32_t dark[6];
    bool ramp;
};
inline const MtimePalette& MtimePaletteAt(int index) {
    static const MtimePalette kPalettes[kMtimePaletteCount] = {
        // 主题色: colors come from the theme accent.
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, true},
        // 冷暖: hot red for just now through orange and amber to green and calm blue.
        {{0xE5484D, 0xF2711C, 0xE59E0C, 0x6FA82F, 0x2E9E6A, 0x3A7FCC},
         {0xFF6B6F, 0xFF8F4D, 0xFFC24D, 0xA5D46A, 0x4CC38A, 0x62A8F0}, false},
        // 青碧
        {{0x0E9384, 0x0E9384, 0x0E9384, 0x0E9384, 0x0E9384, 0x0E9384},
         {0x2CC9B4, 0x2CC9B4, 0x2CC9B4, 0x2CC9B4, 0x2CC9B4, 0x2CC9B4}, true},
        // 琥珀
        {{0xD97706, 0xD97706, 0xD97706, 0xD97706, 0xD97706, 0xD97706},
         {0xF5A742, 0xF5A742, 0xF5A742, 0xF5A742, 0xF5A742, 0xF5A742}, true},
        // 极光: magenta, violet, indigo, cyan, teal, green.
        {{0xD6409F, 0x8E4EC6, 0x5B5BD6, 0x0088B8, 0x12A594, 0x3E9B4F},
         {0xF07CC2, 0xB78CF0, 0x8D8DF5, 0x4CC2E8, 0x3DD0BC, 0x7BD282}, false},
        // 石墨: no hue at all, for a quiet list.
        {{0x3F4652, 0x3F4652, 0x3F4652, 0x3F4652, 0x3F4652, 0x3F4652},
         {0xD0D4DB, 0xD0D4DB, 0xD0D4DB, 0xD0D4DB, 0xD0D4DB, 0xD0D4DB}, true},
    };
    return kPalettes[std::clamp(index, 0, kMtimePaletteCount - 1)];
}

// High contrast: the system highlight (the theme accent there) at full
// strength; the shapes carry the age.
inline D2D1_COLOR_F MtimeColor(int palette, int bucket, bool dark, const D2D1_COLOR_F& accent, bool high_contrast) {
    bucket = std::clamp(bucket, 0, 5);
    if (high_contrast) return D2D1::ColorF(accent.r, accent.g, accent.b, 1.0f);
    static constexpr float kRamp[6] = {1.0f, 0.9f, 0.72f, 0.58f, 0.46f, 0.36f};
    static constexpr float kFlat[6] = {1.0f, 1.0f, 0.95f, 0.9f, 0.85f, 0.8f};
    const MtimePalette& p = MtimePaletteAt(palette);
    const float alpha = p.ramp ? kRamp[bucket] : kFlat[bucket];
    if (std::clamp(palette, 0, kMtimePaletteCount - 1) == 0)
        return D2D1::ColorF(accent.r, accent.g, accent.b, alpha);
    return D2D1::ColorF((dark ? p.dark : p.light)[bucket], alpha);
}

// The bucket's shape in a box `size` wide centered on (cx, cy):
// 0 dot + ring, 1 dot, 2-3 ring with a center dot, 4 ring, 5 small dot.
inline void DrawMtimeMark(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, float cx, float cy, float size,
                          int bucket, const D2D1_COLOR_F& color) {
    if (!dc || !brush || bucket < 0 || bucket > 5) return;
    const auto at = D2D1::Point2F(cx, cy);
    const auto fill = [&](float r, float a) {
        brush->SetColor(D2D1::ColorF(color.r, color.g, color.b, color.a * a));
        dc->FillEllipse(D2D1::Ellipse(at, r, r), brush);
    };
    const auto ring = [&](float r, float stroke, float a) {
        brush->SetColor(D2D1::ColorF(color.r, color.g, color.b, color.a * a));
        dc->DrawEllipse(D2D1::Ellipse(at, r, r), brush, stroke);
    };
    switch (bucket) {
    case 0: fill(size * 0.30f, 1.0f); ring(size * 0.46f, (std::max)(1.0f, size * 0.08f), 0.55f); break;
    case 1: fill(size * 0.32f, 1.0f); break;
    case 2:
    case 3: ring(size * 0.30f, size * 0.13f, 1.0f); fill(size * 0.11f, 1.0f); break;
    case 4: ring(size * 0.30f, size * 0.12f, 1.0f); break;
    default: fill(size * 0.15f, 1.0f); break;
    }
}

}  // namespace pulse::ui
