#pragma once
// snack_style.h - Multi-window M7i: the colors the floating capsule
// notifications and the dragged-tab hint share (toast redesign, scheme A): a
// plate in the opposite tone of the window, its text, and an accent that
// stays readable on it.
#include "FluentTokens.h"

namespace pulse::ui {
struct SnackPalette {
    D2D1_COLOR_F plate;
    D2D1_COLOR_F text;
    D2D1_COLOR_F text_secondary;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F divider;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F shadow;
};

inline float SnackLuma(const D2D1_COLOR_F& c) noexcept { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

inline D2D1_COLOR_F SnackMix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) noexcept {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f);
}

// A light window gets a dark plate and the other way round; high contrast
// keeps the system's flyout colors.
inline SnackPalette MakeSnackPalette(const Theme& theme, bool high_contrast) noexcept {
    if (high_contrast)
        return {theme.surface_flyout, theme.text,      theme.text_secondary, theme.accent,
                theme.stroke_card,    theme.fill_hover, D2D1::ColorF(0, 0.0f)};
    const bool dark_window = SnackLuma(theme.bg) < 0.5f;
    const D2D1_COLOR_F toward = dark_window ? D2D1::ColorF(0x000000) : D2D1::ColorF(0xFFFFFF);
    D2D1_COLOR_F accent = theme.accent;
    accent.a = 1.0f;
    // Mix the accent toward the plate's opposite until it reads on the plate.
    for (int step = 0; step < 8; ++step) {
        const float luma = SnackLuma(accent);
        if (dark_window ? luma <= 0.36f : luma >= 0.58f) break;
        accent = SnackMix(accent, toward, 0.18f);
    }
    if (!dark_window)
        return {D2D1::ColorF(0x2B2D31, 0.97f), D2D1::ColorF(0xF3F4F6), D2D1::ColorF(0xB7BCC4), accent,
                D2D1::ColorF(0xFFFFFF, 0.18f), D2D1::ColorF(0xFFFFFF, 0.12f), D2D1::ColorF(0x000000, 0.26f)};
    return {D2D1::ColorF(0xF1F2F4, 0.98f), D2D1::ColorF(0x17191C), D2D1::ColorF(0x5D6470), accent,
            D2D1::ColorF(0x000000, 0.14f), D2D1::ColorF(0x000000, 0.07f), D2D1::ColorF(0x000000, 0.45f)};
}
}  // namespace pulse::ui
