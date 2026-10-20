// favorites_bar_layout.h — the 收藏栏 row (toolbar redesign §8): the starred
// list laid out left to right under the command row. Pure geometry, shared by
// drawing, hit testing and the » menu, so they always agree.
#pragma once
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse::ui {

inline constexpr float kFavoritesBarHeightDip = 34.0f;
// Narrower than this (the toolbar's width, DIP): icons only.
inline constexpr float kFavoritesBarIconOnlyBelowDip = 820.0f;

struct FavoritesBarLayout {
    std::vector<D2D1_RECT_F> items;  // one per favorite; empty rect: folded into »
    D2D1_RECT_F more{};              // » (empty when everything fits)
    int first_hidden = -1;           // first favorite in », -1 none
    bool icon_only = false;
};

// Prototype .fav: 28 high, padding 0 8 0 7, a 16 icon, gap 6, at most 168
// wide (30 when icons only); the row has padding 0 8 4 10.
inline float FavoritesBarItemWidth(float text_width, float scale, bool icon_only) {
    if (icon_only) return 30.0f * scale;
    return std::min(168.0f * scale, (7.0f + 16.0f + 6.0f + 8.0f) * scale + std::ceil(text_width));
}

// `band`: the row, full width of the toolbar. `text_widths`: each label's
// width in pixels (ignored when icon_only).
inline FavoritesBarLayout MakeFavoritesBarLayout(const D2D1_RECT_F& band, float scale,
                                                 const std::vector<float>& text_widths, bool icon_only) {
    FavoritesBarLayout out;
    out.icon_only = icon_only;
    out.items.assign(text_widths.size(), D2D1_RECT_F{});
    if (text_widths.empty()) return out;
    const float h = 28.0f * scale, gap = 2.0f * scale, more_w = 28.0f * scale;
    const float top = std::round(band.top + std::max(0.0f, (band.bottom - band.top - 4.0f * scale - h) * 0.5f));
    const float left = band.left + 10.0f * scale, right = band.right - 8.0f * scale;
    float total = 0.0f;
    for (size_t i = 0; i < text_widths.size(); ++i)
        total += FavoritesBarItemWidth(text_widths[i], scale, icon_only) + (i ? gap : 0.0f);
    // Folding: the last ones go into », which needs its own room.
    const float limit = left + total <= right + 0.5f ? right : right - more_w - gap;
    float x = left;
    for (size_t i = 0; i < text_widths.size(); ++i) {
        const float w = FavoritesBarItemWidth(text_widths[i], scale, icon_only);
        if (x + w > limit + 0.5f) { out.first_hidden = static_cast<int>(i); break; }
        out.items[i] = D2D1::RectF(std::round(x), top, std::round(x + w), top + h);
        x += w + gap;
    }
    if (out.first_hidden >= 0)
        out.more = D2D1::RectF(std::round(x), top, std::round(x + more_w), top + h);
    return out;
}

} // namespace pulse::ui
