#pragma once
#include "FluentTokens.h"
#include <d2d1_1.h>
#include <wrl/client.h>
#include <algorithm>

namespace pulse::ui {
// Shared by the main file pane and file picker. Brushes live for one draw pass.
class FileItemSelectionPainter {
public:
    FileItemSelectionPainter(ID2D1DeviceContext* dc, const Theme& theme, float scale,
                             bool active, bool high_contrast, bool outline)
        : dc_(dc), theme_(theme), scale_(scale), active_(active), outline_(outline) {
        dc_->CreateSolidColorBrush(active ? theme.fill_selected : theme.fill_selected_inactive, &fill_);
        dc_->CreateSolidColorBrush(theme.accent, &accent_);
        dc_->CreateSolidColorBrush(active ? theme.list_selected_outline : WithAlpha(theme.text, 0.30f), &outline_brush_);
        dc_->CreateSolidColorBrush(theme.fill_hover, &hover_);
        if (active && !high_contrast) {
            const D2D1_GRADIENT_STOP stops[] = {{0, theme.list_selected_start}, {1, theme.list_selected_end}};
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 2, &collection)))
                dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 0}), collection.Get(), &gradient_);
        }
    }
    static D2D1_RECT_F Bounds(D2D1_RECT_F cell, float scale) {
        return {cell.left + 4 * scale, cell.top + scale,
                (std::max)(cell.left + 4 * scale, cell.right - 4 * scale),
                (std::max)(cell.top + scale, cell.bottom - scale)};
    }
    void Hover(D2D1_RECT_F cell) const {
        if (hover_) dc_->FillRoundedRectangle(D2D1::RoundedRect(Bounds(cell, scale_),
            theme_.radius_control * scale_, theme_.radius_control * scale_), hover_.Get());
    }
    // bar: the accent bar at the left edge (off where an item check box marks the row).
    void Selected(D2D1_RECT_F cell, bool bar = true) const {
        const auto sel = Bounds(cell, scale_);
        const float radius = theme_.radius_control * scale_;
        const auto rr = D2D1::RoundedRect(sel, radius, radius);
        if (gradient_) {
            gradient_->SetStartPoint({sel.left, 0}); gradient_->SetEndPoint({sel.right, 0});
            dc_->FillRoundedRectangle(rr, gradient_.Get());
        } else if (fill_) dc_->FillRoundedRectangle(rr, fill_.Get());
        if (outline_ && outline_brush_ && sel.right - sel.left > 2 * scale_) {
            const float half = 0.5f * scale_;
            dc_->DrawRoundedRectangle(D2D1::RoundedRect({sel.left + half, sel.top + half,
                sel.right - half, sel.bottom - half}, radius, radius), outline_brush_.Get(), scale_);
        }
        if (bar && active_ && accent_) {
            const float height = (std::min)(18 * scale_, (std::max)(0.0f, sel.bottom - sel.top - 8 * scale_));
            const float y = (sel.top + sel.bottom - height) * 0.5f;
            dc_->FillRoundedRectangle(D2D1::RoundedRect({sel.left + scale_, y,
                sel.left + 3 * scale_, y + height}, scale_, scale_), accent_.Get());
        }
    }
private:
    ID2D1DeviceContext* dc_;
    const Theme& theme_;
    float scale_;
    bool active_, outline_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_, accent_, outline_brush_, hover_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradient_;
};
}
