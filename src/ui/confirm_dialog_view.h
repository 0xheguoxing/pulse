#pragma once

// Layout and drawing of the Pulse confirm dialog, separate from its window so
// the dialog gallery can render it offscreen.

#include "FluentTokens.h"
#include "confirm_dialog.h"
#include "fluent_components.h"
#include "ui_compositor.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

// Button ids shared by layout, hit testing and drawing.
enum ConfirmControl : int {
    kConfirmNone = 0,
    kConfirmPrimary = 1,
    kConfirmCancel = 2,
    kConfirmClose = 3,
    kConfirmSecondary = 4,
};

// Item rows beyond this collapse into one "N more" row.
inline constexpr size_t kConfirmMaxItemRows = 5;

struct ConfirmDialogFormats {
    ComPtr<IDWriteTextFormat> title;
    ComPtr<IDWriteTextFormat> body;
    ComPtr<IDWriteTextFormat> note;
    bool Create(IDWriteFactory2* factory, float scale);
};

struct ConfirmDialogLayout {
    float width = 0.0f;
    float height = 0.0f;
    float title_bar = 0.0f;  // drag strip height
    D2D1_RECT_F close{};
    D2D1_RECT_F icon{};
    D2D1_RECT_F title{};
    D2D1_RECT_F message{};
    D2D1_RECT_F card{};
    std::vector<D2D1_RECT_F> rows;
    D2D1_RECT_F note{};
    D2D1_RECT_F footer{};
    D2D1_RECT_F primary{};
    D2D1_RECT_F secondary{};
    D2D1_RECT_F cancel{};
};

struct ConfirmDialogVisual {
    int hover = kConfirmNone;
    int pressed = kConfirmNone;
    int focus = kConfirmPrimary;
    bool show_focus = false;
    // Motion (animation phase 9b), filled by the window per frame; the
    // defaults draw the settled dialog (the gallery renders it that way).
    float icon_scale = 1.0f;   // the icon tile pops in
    float icon_turn = 0.0f;    // degrees; a warning glyph wobbles once
    float body_in = 1.0f;      // 0..1 (springs may overshoot): title, message, note rise in
    float rows_in[kConfirmMaxItemRows] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    float footer_in = 1.0f;    // the buttons
    bool focus_glide = false;  // draw the focus ring at focus_rect, gliding between buttons
    D2D1_RECT_F focus_rect{};
};

// Fills default button texts and turns raw paths into friendly text.
ConfirmDialogSpec NormalizeConfirmSpec(ConfirmDialogSpec spec);
ConfirmTone ResolveConfirmTone(const ConfirmDialogSpec& spec);
// Rows actually drawn: the items, or the first ones plus an "N more" row.
std::vector<std::wstring> ConfirmItemRows(const ConfirmDialogSpec& spec);
// Every item the question is about: items.size(), or items_total if larger.
size_t ConfirmItemCount(const ConfirmDialogSpec& spec);
// One item row that fits `width`: paths keep the drive and the last segments
// ("C:\\…\\fx\\notes.txt"), other text gets a trailing ellipsis.
std::wstring FitConfirmItemText(const std::wstring& text, float width,
                                const std::function<float(std::wstring_view)>& measure);
// Keyboard order of the footer buttons, left to right.
std::vector<int> ConfirmFocusOrder(const ConfirmDialogSpec& spec);
// The footer button for a choice; Secondary falls back to Cancel without one.
int ConfirmControlFor(ConfirmChoice choice, bool has_secondary);
// Plain text for Ctrl+C, like a system message box.
std::wstring ConfirmClipboardText(const ConfirmDialogSpec& spec);

ConfirmDialogLayout LayoutConfirmDialog(const ConfirmDialogSpec& spec,
                                        const fluent::Painter& painter,
                                        IDWriteFactory2* factory,
                                        const ConfirmDialogFormats& formats, float scale);
int HitTestConfirmDialog(const ConfirmDialogLayout& layout, bool has_secondary,
                         float x, float y);
void DrawConfirmDialog(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                       const ConfirmDialogSpec& spec, const ConfirmDialogLayout& layout,
                       const ConfirmDialogFormats& formats, const ConfirmDialogVisual& visual,
                       bool dark, bool high_contrast);

// Shared by Pulse dialogs: a top-aligned word-wrapped paragraph.
void DrawDialogParagraph(Compositor& compositor, IDWriteTextFormat* format,
                         const D2D1_RECT_F& bounds, std::wstring_view text,
                         const D2D1_COLOR_F& color);

} // namespace pulse::ui