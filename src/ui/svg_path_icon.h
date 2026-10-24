// svg_path_icon.h — draw SVG path data (the same mini parser as the
// assistant's icons) for dialogs that carry their own glyphs.
#pragma once
#include <d2d1_1.h>
#include <cstddef>

namespace pulse::ui {

struct SvgPart {
    const char* d;       // must be a string literal: geometry is cached per pointer
    bool fill;           // false = stroke with round caps/joins
    float stroke;        // view-box units
    float opacity;
};

// Fits a `view`×`view` view box into the square at box.left/top.
void DrawSvgParts(ID2D1DeviceContext* dc, const SvgPart* parts, size_t count, float view,
                  const D2D1_RECT_F& box, D2D1_COLOR_F color);

} // namespace pulse::ui
