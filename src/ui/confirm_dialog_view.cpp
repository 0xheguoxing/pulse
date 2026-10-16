#include "../common/windows_compat.h"
#include "confirm_dialog_view.h"

#include "../common/display_path.h"
#include "../common/localization.h"
#include "typography.h"
#include "window_helpers.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace pulse::ui {
namespace {

constexpr float kMinWidth = 420.0f;
constexpr float kMaxWidth = 540.0f;
constexpr float kPad = 24.0f;
constexpr float kTitleBar = 36.0f;
constexpr float kCloseW = 46.0f;
constexpr float kIcon = 40.0f;
constexpr float kIconGap = 16.0f;
constexpr float kRowH = 36.0f;
constexpr float kButtonMinW = 96.0f;
constexpr float kButtonGap = 8.0f;
constexpr float kFooterPadY = 18.0f;
// Prefer a wider dialog over a message that wraps into a tall column.
constexpr float kComfortableLines = 4.0f;

float Dip(float scale, float value) { return value * scale; }

D2D1_COLOR_F ToneColor(ConfirmTone tone, const Theme& theme, bool dark) {
    switch (tone) {
    case ConfirmTone::Danger: return theme.danger;
    case ConfirmTone::Warning: return dark ? HexColor(0xFCE100) : HexColor(0x9D5D00);
    default: return theme.accent;
    }
}

const wchar_t* ToneGlyph(ConfirmTone tone) {
    switch (tone) {
    case ConfirmTone::Danger:
    case ConfirmTone::Warning: return L"\xE7BA";   // Warning
    case ConfirmTone::Info: return L"\xE946";      // Info
    default: return L"\xE9CE";                     // Unknown (question)
    }
}

float ButtonWidth(const fluent::Painter& painter, const std::wstring& text, float scale) {
    return std::max(painter.MeasureButtonWidth(text), Dip(scale, kButtonMinW));
}

// One group of the dialog rising in: drawn 6 DIP lower and faded until its
// spring reaches 1. A settled group (k == 1) costs nothing.
class RiseIn {
public:
    RiseIn(Compositor& compositor, float k, float rise) {
        if (k == 1.0f) return;
        dc_ = compositor.Dc();
        if (!dc_) return;
        dc_->GetTransform(&old_);
        dc_->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, (1.0f - k) * rise) * old_);
        const float opacity = std::clamp(k * 1.6f, 0.0f, 1.0f);
        if (opacity < 1.0f) {
            dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), opacity, nullptr,
                                                  D2D1_LAYER_OPTIONS1_NONE),
                           nullptr);
            layered_ = true;
        }
    }
    ~RiseIn() {
        if (!dc_) return;
        if (layered_) dc_->PopLayer();
        dc_->SetTransform(old_);
    }
    RiseIn(const RiseIn&) = delete;
    RiseIn& operator=(const RiseIn&) = delete;

private:
    ID2D1DeviceContext* dc_ = nullptr;
    D2D1_MATRIX_3X2_F old_{};
    bool layered_ = false;
};

// Scales (and turns) what is drawn while alive around `centre`.
class AroundCentre {
public:
    AroundCentre(Compositor& compositor, D2D1_POINT_2F centre, float scale, float degrees) {
        if (scale == 1.0f && degrees == 0.0f) return;
        dc_ = compositor.Dc();
        if (!dc_) return;
        dc_->GetTransform(&old_);
        dc_->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, centre) *
                          D2D1::Matrix3x2F::Scale(scale, scale, centre) * old_);
    }
    ~AroundCentre() { if (dc_) dc_->SetTransform(old_); }
    AroundCentre(const AroundCentre&) = delete;
    AroundCentre& operator=(const AroundCentre&) = delete;

private:
    ID2D1DeviceContext* dc_ = nullptr;
    D2D1_MATRIX_3X2_F old_{};
};

D2D1_POINT_2F Centre(const D2D1_RECT_F& r) {
    return D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
}

float Wrapped(IDWriteFactory2* factory, IDWriteTextFormat* format, std::wstring_view text,
              float width) {
    if (!factory || !format || text.empty() || width <= 0.0f) return 0.0f;
    return std::ceil(typography::MeasureWrapped(factory, format, text, width));
}

} // namespace

bool ConfirmDialogFormats::Create(IDWriteFactory2* factory, float scale) {
    title.reset();
    body.reset();
    note.reset();
    if (!factory) return false;
    typography::CreateTextFormat(factory,
        {typography::FontRole::Display, 18.0f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &title);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 14.0f * scale}, &body);
    typography::CreateTextFormat(factory, {typography::FontRole::Text, 12.5f * scale}, &note);
    for (IDWriteTextFormat* format : {title.get(), body.get(), note.get()}) {
        if (!format) return false;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return true;
}

ConfirmDialogSpec NormalizeConfirmSpec(ConfirmDialogSpec spec) {
    spec.message = path::FriendlyPathText(spec.message);
    spec.note = path::FriendlyPathText(spec.note);
    for (std::wstring& item : spec.items) item = path::FriendlyPathText(item);
    if (spec.confirm_text.empty())
        spec.confirm_text = l10n::Get(l10n::StringId::ConfirmDefault);
    if (spec.cancel_text.empty()) spec.cancel_text = l10n::Get(l10n::StringId::Cancel);
    // Trailing blank lines from older prompt strings would pad the layout.
    while (!spec.message.empty() && (spec.message.back() == L'\n' || spec.message.back() == L' '))
        spec.message.pop_back();
    return spec;
}

ConfirmTone ResolveConfirmTone(const ConfirmDialogSpec& spec) {
    if (spec.tone != ConfirmTone::Auto) return spec.tone;
    return spec.danger ? ConfirmTone::Danger : ConfirmTone::Question;
}

std::vector<std::wstring> ConfirmItemRows(const ConfirmDialogSpec& spec) {
    if (spec.items.size() <= kConfirmMaxItemRows) return spec.items;
    std::vector<std::wstring> rows(spec.items.begin(),
                                   spec.items.begin() + (kConfirmMaxItemRows - 1));
    wchar_t more[128]{};
    swprintf_s(more, l10n::Get(l10n::StringId::ConfirmMoreItemsFormat).c_str(),
               spec.items.size() - (kConfirmMaxItemRows - 1));
    rows.emplace_back(more);
    return rows;
}

std::wstring FitConfirmItemText(const std::wstring& text, float width,
                                const std::function<float(std::wstring_view)>& measure) {
    return FitPathMiddle(text, width, measure);
}

std::vector<int> ConfirmFocusOrder(const ConfirmDialogSpec& spec) {
    if (spec.secondary_text.empty()) return {kConfirmCancel, kConfirmPrimary};
    return {kConfirmCancel, kConfirmSecondary, kConfirmPrimary};
}

int ConfirmControlFor(ConfirmChoice choice, bool has_secondary) {
    switch (choice) {
    case ConfirmChoice::Confirm: return kConfirmPrimary;
    case ConfirmChoice::Secondary: return has_secondary ? kConfirmSecondary : kConfirmCancel;
    default: return kConfirmCancel;
    }
}

std::wstring ConfirmClipboardText(const ConfirmDialogSpec& spec) {
    std::wstring text = spec.title;
    if (!spec.message.empty()) text += L"\r\n\r\n" + spec.message;
    for (const std::wstring& item : spec.items) text += L"\r\n  " + item;
    if (!spec.note.empty()) text += L"\r\n\r\n" + spec.note;
    return text;
}

ConfirmDialogLayout LayoutConfirmDialog(const ConfirmDialogSpec& spec,
                                        const fluent::Painter& painter,
                                        IDWriteFactory2* factory,
                                        const ConfirmDialogFormats& formats, float scale) {
    ConfirmDialogLayout l;
    const float pad = Dip(scale, kPad);
    const float gap = Dip(scale, kButtonGap);
    const float btn_h = painter.MeasureButtonHeight();
    const float primary_w = ButtonWidth(painter, spec.confirm_text, scale);
    const float cancel_w = ButtonWidth(painter, spec.cancel_text, scale);
    const float secondary_w = spec.secondary_text.empty()
        ? 0.0f : ButtonWidth(painter, spec.secondary_text, scale);
    const float buttons_w = primary_w + cancel_w +
        (secondary_w > 0.0f ? secondary_w + gap : 0.0f) + gap;
    const float text_x = pad + Dip(scale, kIcon + kIconGap);

    // Widen until the message fits in a few lines or the cap is reached.
    float width = std::max(Dip(scale, kMinWidth), buttons_w + pad * 2.0f);
    if (formats.body.get() && factory) {
        const float line = Wrapped(factory, formats.body.get(), L"A", 1000.0f);
        while (width < Dip(scale, kMaxWidth)) {
            const float h = Wrapped(factory, formats.body.get(), spec.message,
                                    width - text_x - pad);
            if (h <= line * kComfortableLines + 0.5f) break;
            width = std::min(width + Dip(scale, 20.0f), Dip(scale, kMaxWidth));
        }
    }
    width = std::ceil(width);
    const float text_w = width - text_x - pad;

    l.title_bar = Dip(scale, kTitleBar);
    l.close = D2D1::RectF(width - Dip(scale, kCloseW), 0, width, l.title_bar);
    float y = Dip(scale, 28.0f);
    l.icon = D2D1::RectF(pad, y, pad + Dip(scale, kIcon), y + Dip(scale, kIcon));
    const float title_h = std::max(Wrapped(factory, formats.title.get(), spec.title, text_w),
                                   Dip(scale, 24.0f));
    // A one-line title sits centred on the icon; a long one grows downwards.
    const float title_top = title_h < Dip(scale, kIcon)
        ? y + (Dip(scale, kIcon) - title_h) * 0.5f : y;
    l.title = D2D1::RectF(text_x, title_top, width - pad, title_top + title_h);
    y = std::max(l.icon.bottom, l.title.bottom) + Dip(scale, 10.0f);

    const float msg_h = Wrapped(factory, formats.body.get(), spec.message, text_w);
    l.message = D2D1::RectF(text_x, y, width - pad, y + msg_h);
    if (msg_h > 0.0f) y = l.message.bottom;

    const std::vector<std::wstring> rows = ConfirmItemRows(spec);
    if (!rows.empty()) {
        y += Dip(scale, 14.0f);
        const float row_h = Dip(scale, kRowH);
        l.card = D2D1::RectF(text_x, y, width - pad,
                             y + row_h * static_cast<float>(rows.size()));
        for (size_t i = 0; i < rows.size(); ++i) {
            const float top = y + row_h * static_cast<float>(i);
            l.rows.push_back(D2D1::RectF(l.card.left, top, l.card.right, top + row_h));
        }
        y = l.card.bottom;
    }
    if (!spec.note.empty()) {
        y += Dip(scale, 12.0f);
        const float note_h = Wrapped(factory, formats.note.get(), spec.note, text_w);
        l.note = D2D1::RectF(text_x, y, width - pad, y + note_h);
        y = l.note.bottom;
    }

    y += Dip(scale, 24.0f);
    const float footer_h = btn_h + Dip(scale, kFooterPadY) * 2.0f;
    l.footer = D2D1::RectF(0, y, width, y + footer_h);
    const float by = y + Dip(scale, kFooterPadY);
    float right = width - pad;
    l.primary = D2D1::RectF(right - primary_w, by, right, by + btn_h);
    right = l.primary.left - gap;
    if (secondary_w > 0.0f) {
        l.secondary = D2D1::RectF(right - secondary_w, by, right, by + btn_h);
        right = l.secondary.left - gap;
    }
    l.cancel = D2D1::RectF(right - cancel_w, by, right, by + btn_h);
    l.width = width;
    l.height = std::ceil(l.footer.bottom);
    return l;
}

int HitTestConfirmDialog(const ConfirmDialogLayout& layout, bool has_secondary,
                         float x, float y) {
    if (ContainsRect(layout.close, x, y)) return kConfirmClose;
    if (ContainsRect(layout.primary, x, y)) return kConfirmPrimary;
    if (has_secondary && ContainsRect(layout.secondary, x, y)) return kConfirmSecondary;
    if (ContainsRect(layout.cancel, x, y)) return kConfirmCancel;
    return kConfirmNone;
}

void DrawDialogParagraph(Compositor& compositor, IDWriteTextFormat* format,
                         const D2D1_RECT_F& bounds, std::wstring_view text,
                         const D2D1_COLOR_F& color) {
    auto* factory = compositor.DwriteFactory();
    auto* dc = compositor.Dc();
    if (!factory || !dc || !format || text.empty()) return;
    const float width = bounds.right - bounds.left;
    const float height = std::max(bounds.bottom - bounds.top, 1.0f);
    if (width <= 0.0f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                         format, width, height, &layout)) || !layout.get())
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(color, &brush)) || !brush.get()) return;
    dc->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(), brush.get());
}

void DrawConfirmDialog(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                       const ConfirmDialogSpec& spec, const ConfirmDialogLayout& l,
                       const ConfirmDialogFormats& formats, const ConfirmDialogVisual& visual,
                       bool dark, bool high_contrast) {
    const float scale = painter.Scale();
    const ConfirmTone tone = ResolveConfirmTone(spec);
    const D2D1_COLOR_F tone_color = high_contrast ? theme.text : ToneColor(tone, theme, dark);

    fluent::ControlState close_state{};
    close_state.hovered = visual.hover == kConfirmClose;
    close_state.pressed = visual.pressed == kConfirmClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

    // Icon tile: the tone colour at low opacity behind its glyph.
    {
        AroundCentre pop(compositor, Centre(l.icon), visual.icon_scale, 0.0f);
        D2D1_COLOR_F tile = tone_color;
        tile.a = high_contrast ? 0.0f : (dark ? 0.18f : 0.12f);
        painter.FillRoundedRect(l.icon, Dip(scale, 10.0f), tile);
        if (high_contrast) painter.StrokeRoundedRect(l.icon, Dip(scale, 10.0f), theme.text);
        AroundCentre turn(compositor, Centre(l.icon), 1.0f, visual.icon_turn);
        painter.DrawGlyph(spec.glyph.empty() ? ToneGlyph(tone) : spec.glyph.c_str(), l.icon,
                          tone_color);
    }

    const float rise = Dip(scale, 6.0f);
    {
        RiseIn in(compositor, visual.body_in, rise);
        DrawDialogParagraph(compositor, formats.title.get(), l.title, spec.title, theme.text);
        DrawDialogParagraph(compositor, formats.body.get(), l.message, spec.message,
                            high_contrast ? theme.text : theme.text_secondary);
    }

    const std::vector<std::wstring> rows = ConfirmItemRows(spec);
    if (!rows.empty()) {
        const float radius = Dip(scale, theme.radius_control);
        {
            RiseIn in(compositor, visual.rows_in[0], rise);
            painter.FillRoundedRect(l.card, radius, theme.surface_card);
            painter.StrokeRoundedRect(l.card, radius, theme.stroke_card);
        }
        const std::wstring_view glyph = spec.item_glyph.empty()
            ? std::wstring_view(L"\xE8A5") : std::wstring_view(spec.item_glyph);
        const bool truncated = spec.items.size() > rows.size();
        for (size_t i = 0; i < rows.size() && i < l.rows.size(); ++i) {
            const D2D1_RECT_F& row = l.rows[i];
            RiseIn in(compositor, visual.rows_in[std::min(i, kConfirmMaxItemRows - 1)], rise);
            if (i > 0) {
                painter.FillRoundedRect(D2D1::RectF(row.left + Dip(scale, 12.0f), row.top,
                                                    row.right - Dip(scale, 12.0f),
                                                    row.top + 1.0f),
                                        0, theme.stroke_divider);
            }
            const bool more_row = truncated && i + 1 == rows.size();
            if (!more_row) {
                painter.DrawGlyph(glyph, D2D1::RectF(row.left + Dip(scale, 10.0f), row.top,
                                                     row.left + Dip(scale, 30.0f), row.bottom),
                                  theme.text_secondary);
            }
            const D2D1_RECT_F text_rect = D2D1::RectF(row.left + Dip(scale, 40.0f), row.top,
                                                      row.right - Dip(scale, 12.0f), row.bottom);
            const std::wstring text = more_row ? rows[i] : FitConfirmItemText(
                rows[i], text_rect.right - text_rect.left,
                [&](std::wstring_view s) {
                    return typography::MeasureLine(&compositor, compositor.TextFormat(), s);
                });
            painter.DrawText(text, text_rect,
                             compositor.TextFormat(),
                             more_row ? theme.text_secondary : theme.text,
                             fluent::HorizontalAlignment::Left, theme.surface_card);
        }
    }
    {
        RiseIn in(compositor, visual.body_in, rise);
        DrawDialogParagraph(compositor, formats.note.get(), l.note, spec.note,
                            high_contrast ? theme.text : theme.text_secondary);
    }

    // Footer band: one layer up from the body, like the main window's sheet.
    painter.FillRoundedRect(l.footer, 0, theme.surface_sheet);
    painter.FillRoundedRect(D2D1::RectF(l.footer.left, l.footer.top, l.footer.right,
                                        l.footer.top + 1.0f),
                            0, theme.stroke_divider);

    auto state_for = [&](int id) {
        fluent::ControlState state{};
        state.hovered = visual.hover == id;
        state.pressed = visual.pressed == id;
        state.keyboard_focus = visual.show_focus && visual.focus == id && !visual.focus_glide;
        return state;
    };
    // A button held down (pointer still on it) sinks a little.
    auto pressed_scale = [&](int id) {
        return visual.pressed == id && visual.hover == id && !high_contrast ? 0.97f : 1.0f;
    };
    {
        RiseIn in(compositor, visual.footer_in, rise);
        {
            AroundCentre press(compositor, Centre(l.cancel), pressed_scale(kConfirmCancel), 0.0f);
            painter.DrawButton({l.cancel, spec.cancel_text, {}, fluent::ButtonKind::Standard,
                                state_for(kConfirmCancel)});
        }
        if (!spec.secondary_text.empty()) {
            AroundCentre press(compositor, Centre(l.secondary), pressed_scale(kConfirmSecondary),
                               0.0f);
            painter.DrawButton({l.secondary, spec.secondary_text, {},
                                fluent::ButtonKind::Standard, state_for(kConfirmSecondary)});
        }
        {
            AroundCentre press(compositor, Centre(l.primary), pressed_scale(kConfirmPrimary),
                               0.0f);
            painter.DrawButton({l.primary, spec.confirm_text, {},
                                spec.danger ? fluent::ButtonKind::Danger
                                            : fluent::ButtonKind::Primary,
                                state_for(kConfirmPrimary)});
        }
        // The focus ring travelling between buttons (the window springs focus_rect).
        const bool ring = visual.show_focus && visual.focus_glide;
        if (ring) painter.DrawFocusRing(visual.focus_rect, Dip(scale, theme.radius_control));
        // The shared accent ring vanishes against a filled button; add the Fluent
        // outer ring in the text colour.
        if (visual.show_focus && visual.focus == kConfirmPrimary) {
            const D2D1_RECT_F& r = ring ? visual.focus_rect : l.primary;
            const float gap = Dip(scale, 3.0f);
            painter.StrokeRoundedRect(D2D1::RectF(r.left - gap, r.top - gap,
                                                  r.right + gap, r.bottom + gap),
                                      Dip(scale, theme.radius_control) + gap, theme.text,
                                      Dip(scale, 1.5f));
        }
    }

    // The footer band covers the surface outline; draw it again on top.
    const float inset = 0.5f;
    painter.StrokeRoundedRect(D2D1::RectF(inset, inset, l.width - inset, l.height - inset),
                              Dip(scale, 12.0f), theme.stroke_card);
}

} // namespace pulse::ui
