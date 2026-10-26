// ai_panel_draw.h — drawing helpers shared by the Pulse 助手 panel's files
// (ai_panel_view.cpp, ai_panel_conversation.cpp). Internal to src/ui.
#pragma once
#include "ai_panel_view.h"
#include "ui_compositor.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {

struct AiPanelView::Palette {
    D2D1_COLOR_F accent, accent_soft, accent_tint, on_accent;
    D2D1_COLOR_F text, text2, text3;
    D2D1_COLOR_F line, line2, hover;
    D2D1_COLOR_F card, pane, bg;
    D2D1_COLOR_F ok;
    D2D1_COLOR_F amber, amber_bg, amber_line;
    D2D1_COLOR_F bad, bad_bg, bad_line;
    D2D1_COLOR_F sys, red, red_bg, red_line;
    D2D1_COLOR_F shadow;
    D2D1_COLOR_F folder_icon;
    std::array<D2D1_COLOR_F, 6> tint_fg, tint_bg;
    // Docked over a wallpaper or Mica: the column is a translucent card like
    // the pane, and the welcome cards and the resting composer let it show
    // through. Menus, cards in the conversation and the focused composer
    // keep the solid colors above.
    bool glass = false;
    D2D1_COLOR_F glass_panel{}, glass_panel_line{};
    D2D1_COLOR_F glass_card{}, glass_card_line{};
    D2D1_COLOR_F glass_input{}, glass_input_line{};
    // Hover and hairline for what sits right on the glass (the header, the
    // resting composer's buttons); `hover` and `line2` stay opaque because
    // cards mix them with other colors.
    D2D1_COLOR_F glass_hover{}, glass_line{};
};

// The panel's colors for `theme` (ai_panel_view.cpp).
AiPanelView::Palette AiPanelPalette(const Theme& theme, bool dark);

namespace ai_draw {

inline D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

inline D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

// `top` composited over the opaque `base`.
inline D2D1_COLOR_F Over(D2D1_COLOR_F top, D2D1_COLOR_F base) {
    const float a = top.a;
    return D2D1::ColorF(top.r * a + base.r * (1 - a), top.g * a + base.g * (1 - a), top.b * a + base.b * (1 - a), 1.0f);
}

inline D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

inline D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return D2D1::RectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }

inline bool In(const D2D1_RECT_F& r, float x, float y) {
    return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// CSS cubic-bezier(.3,.7,.2,1), the design's panel and menu curve.
inline float EaseDesign(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    // Solve x(u) = t for u by Newton, then y(u).
    const float x1 = 0.3f, y1 = 0.7f, x2 = 0.2f, y2 = 1.0f;
    float u = t;
    for (int i = 0; i < 6; ++i) {
        const float x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u - t;
        const float dx = 3 * (1 - u) * (1 - u) * x1 + 6 * (1 - u) * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u = std::clamp(u - x / dx, 0.0f, 1.0f);
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}

struct TextOptions {
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    bool center_v = true;      // single line centered in the box
    bool wrap = false;
    float line_height = 0.0f;  // px; 0 = natural
    bool ellipsis = true;
};

struct Ctx {
    Compositor* compositor;
    ID2D1DeviceContext* dc;
    ID2D1SolidColorBrush* brush;
    float s;

    ID2D1SolidColorBrush* B(D2D1_COLOR_F c) const { brush->SetColor(c); return brush; }

    void Fill(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) const {
        if (c.a <= 0.0f) return;
        if (radius <= 0.0f) dc->FillRectangle(r, B(c));
        else dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), B(c));
    }
    // A 1 px border inside `r`.
    void Stroke(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width = 1.0f) const {
        if (c.a <= 0.0f) return;
        const float h = width * 0.5f;
        const D2D1_RECT_F in = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(in, (std::max)(0.0f, radius - h), (std::max)(0.0f, radius - h)),
                                 B(c), width);
    }
    // Soft drop shadow: stacked rounded rects fading out (cheap blur look).
    void Shadow(const D2D1_RECT_F& r, float radius, float blur, float dy, D2D1_COLOR_F c) const {
        const int steps = 8;
        for (int i = steps; i >= 1; --i) {
            const float t = static_cast<float>(i) / steps;
            const float grow = blur * t;
            D2D1_RECT_F g = Inflate(r, grow * 0.6f);
            g.top += dy * (0.4f + 0.6f * t);
            g.bottom += dy * (0.4f + 0.6f * t);
            Fill(g, radius + grow * 0.6f, Alpha(c, (1.0f - t) * 0.9f / steps * 2.2f));
        }
    }
    void Icon(AiIcon icon, float x, float y, float size, D2D1_COLOR_F c, const AiIconPose& pose = {}) const {
        DrawAiIcon(dc, icon, D2D1::RectF(x, y, x + size, y + size), c, pose);
    }

    IDWriteTextLayout* Layout(const std::wstring& text, IDWriteTextFormat* fmt, float width, float height,
                              const TextOptions& o) const {
        if (!fmt || text.empty()) return nullptr;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(compositor->DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                                                 (std::max)(1.0f, width), (std::max)(1.0f, height),
                                                                 &layout)))
            return nullptr;
        layout->SetTextAlignment(o.align);
        layout->SetWordWrapping(o.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (o.line_height > 0.0f)
            layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, o.line_height, o.line_height * 0.78f);
        if (!o.wrap && o.ellipsis) {
            IDWriteInlineObject* sign = nullptr;
            if (SUCCEEDED(compositor->DwriteFactory()->CreateEllipsisTrimmingSign(fmt, &sign))) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trim, sign);
                sign->Release();
            }
        }
        return layout;
    }
    // Draws text in `r`; returns the laid out height.
    float Text(const std::wstring& text, IDWriteTextFormat* fmt, const D2D1_RECT_F& r, D2D1_COLOR_F c,
               const TextOptions& o = {}) const {
        IDWriteTextLayout* layout = Layout(text, fmt, r.right - r.left, o.wrap ? 10000.0f : r.bottom - r.top, o);
        if (!layout) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        float y = r.top;
        if (!o.wrap && o.center_v) y = std::round(r.top + ((r.bottom - r.top) - m.height) * 0.5f);
        compositor->DrawTextLayout(D2D1::Point2F(r.left, y), layout, B(c));
        layout->Release();
        return m.height;
    }
    float Measure(const std::wstring& text, IDWriteTextFormat* fmt) const {
        TextOptions o;
        o.ellipsis = false;
        IDWriteTextLayout* layout = Layout(text, fmt, 10000.0f, 1000.0f, o);
        if (!layout) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        return std::ceil(m.widthIncludingTrailingWhitespace);
    }
    float MeasureWrapped(const std::wstring& text, IDWriteTextFormat* fmt, float width, float line_height) const {
        TextOptions o;
        o.wrap = true;
        o.line_height = line_height;
        IDWriteTextLayout* layout = Layout(text, fmt, width, 10000.0f, o);
        if (!layout) return 0.0f;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        return std::ceil(m.height);
    }
};

} // namespace ai_draw
} // namespace pulse::ui
