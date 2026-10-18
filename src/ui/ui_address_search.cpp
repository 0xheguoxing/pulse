#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "address_search_layout.h"

namespace pulse::ui {

D2D1_RECT_F MainRenderer::AddressSearchButtonRect(float w) const {
    const auto field = SearchBarRect(w);
    const float width = (field.right - field.left) / scale_ - 8.0f;
    return D2D1::RectF(field.right - (width + 4.0f) * scale_, field.top + 3.0f * scale_,
                      field.right - 4.0f * scale_, field.bottom - 3.0f * scale_);
}

void MainRenderer::DrawAddressSearchChrome(const WindowViewModel& vm, float w, const Theme& theme) {
    if (search_hidden_) return;
    const auto field = SearchBarRect(w);
    if (!vm.address_searching && w-EffectiveSidebarWidth(w)<480*scale_) {
        const auto button=D2D1::RectF(field.right-32*scale_,field.top,field.right,field.bottom);
        DrawButton(button,theme,IsHovered(vm,HitTestResult::AddressSearch) ? theme.fill_hover : kTransparent,
            kIconSearch,L"",theme.text_secondary,true,true);
        return;
    }
    fluent::ControlState state{};
    state.focused = vm.address_searching;
    state.hovered = IsHovered(vm, HitTestResult::AddressSearch);
    if (vm.address_searching) painter_.DrawTextFieldFrame(field, state);
    auto button = [&](D2D1_RECT_F bounds, const std::wstring& text, const wchar_t* glyph,
                      HitTestResult::Region region, bool dropdown = false) {
        fluent::ButtonSpec spec;
        spec.bounds = bounds;
        spec.text = text;
        spec.glyph = glyph;
        spec.icon_only = text.empty();
        spec.skip_glyph = true;
        spec.kind = fluent::ButtonKind::Transparent;
        spec.bordered = false;
        spec.drop_down = dropdown;
        spec.state.hovered = IsHovered(vm, region);
        painter_.DrawButton(spec);
        {
            // Compact buttons cannot use the text button's 8-DIP side padding.
            const float edge = std::min({20.0f * scale_, bounds.right - bounds.left - 4.0f * scale_,
                                         bounds.bottom - bounds.top - 4.0f * scale_});
            const float cx = spec.icon_only ? (bounds.left + bounds.right) * 0.5f
                                            : bounds.left + 18.0f * scale_;
            const float cy = (bounds.top + bounds.bottom) * 0.5f;
            DrawIconText(cx - edge * 0.5f, cy - edge * 0.5f, edge, edge,
                         glyph, L"", theme.text, 0.8f);
        }
    };
    if (vm.address_searching) {
        const auto layout = LayoutAddressSearch(field, scale_);
        if (!vm.address_editing) {
            const auto text = vm.address_search_text.empty()
                ? l10n::Get(vm.address_search_content ? l10n::StringId::SearchContentHint : l10n::StringId::SearchNameHint) : vm.address_search_text;
            ComPtr<ID2D1SolidColorBrush> brush;
            compositor_->Dc()->CreateSolidColorBrush(vm.address_search_text.empty()
                ? theme.text_secondary : theme.text, &brush);
            if (brush.get()) DrawTextRect(compositor_->Dc(), compositor_->AddressFormat(), brush.get(),
                text, layout.input.left, layout.input.top, layout.input.right - layout.input.left,
                layout.input.bottom - layout.input.top);
        }
        if (vm.address_scope_animation > 0.0f) {
            auto color = theme.accent;
            color.a *= vm.address_scope_animation * 0.18f;
            ComPtr<ID2D1SolidColorBrush> brush;
            compositor_->Dc()->CreateSolidColorBrush(color, &brush);
            if (brush.get()) compositor_->Dc()->FillRoundedRectangle(
                D2D1::RoundedRect(layout.scope, 4.0f * scale_, 4.0f * scale_), brush.get());
        }
        auto* dc = compositor_->Dc();
        auto chip = [&](D2D1_RECT_F bounds, bool accent, HitTestResult::Region region) {
            auto fill = accent ? theme.accent : theme.text;
            fill.a *= accent ? 0.18f : 0.07f;
            if (IsHovered(vm, region)) fill.a = std::min(1.0f, fill.a + 0.06f);
            ComPtr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(fill, &brush);
            if (brush.get()) dc->FillRoundedRectangle(D2D1::RoundedRect(bounds, 4.0f * scale_, 4.0f * scale_), brush.get());
        };
        // Scope chip: where the search runs, labelled with the folder name.
        if (layout.scope.right > layout.scope.left) {
            const auto& scope = layout.scope;
            const float h = scope.bottom - scope.top;
            const wchar_t* glyph = vm.address_search_current ? L"\xE8B7" : L"\xE774";
            chip(scope, vm.address_search_current, HitTestResult::AddressSearchScope);
            if (layout.scope_label) {
                DrawIconText(scope.left + 3.0f * scale_, scope.top, 18.0f * scale_, h, glyph, L"",
                             vm.address_search_current ? theme.accent : theme.text_secondary, 0.7f);
                DrawIconText(scope.right - 16.0f * scale_, scope.top, 14.0f * scale_, h, L"\xE70D", L"v",
                             theme.text_secondary, 0.45f);
                const auto label = vm.address_search_scope_label.empty()
                    ? l10n::Get(vm.address_search_current ? l10n::StringId::SearchScopeHere : l10n::StringId::SearchScopeAll)
                    : vm.address_search_scope_label;
                MakeBrush(dc, theme.text, brText_);
                const float text_left = scope.left + 23.0f * scale_;
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(), brText_.get(),
                    label, text_left, scope.top, std::max(0.0f, scope.right - 18.0f * scale_ - text_left), h);
            } else {
                DrawIconText(scope.left, scope.top, scope.right - scope.left, h, glyph, L"",
                             vm.address_search_current ? theme.accent : theme.text_secondary, 0.7f);
            }
        }
        // Mode chip: one click toggles between name and content search.
        if (layout.mode.right > layout.mode.left) {
            const auto& mode = layout.mode;
            const float h = mode.bottom - mode.top;
            // Content mode is lit so it never looks like the default name search.
            chip(mode, vm.address_search_content, HitTestResult::AddressSearchMode);
            if (layout.mode_label) {
                const auto& text = l10n::Get(vm.address_search_content
                    ? l10n::StringId::SearchModeContent : l10n::StringId::SearchModeName);
                const float text_w = MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), text);
                const float icon_w = 14.0f * scale_;
                const float gap = 3.0f * scale_;
                const float x0 = std::max(mode.left + 4.0f * scale_,
                                          (mode.left + mode.right - text_w - icon_w - gap) * 0.5f);
                DrawIconText(x0, mode.top, icon_w, h, vm.address_search_content ? L"\xE8A5" : L"\xE8D2", L"",
                             vm.address_search_content ? theme.accent : theme.text_secondary, 0.6f);
                MakeBrush(dc, theme.text, brText_);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(), brText_.get(),
                    text, x0 + icon_w + gap, mode.top, std::max(0.0f, mode.right - (x0 + icon_w + gap)), h);
            } else {
                DrawIconText(mode.left, mode.top, mode.right - mode.left, h,
                             vm.address_search_content ? L"\xE8A5" : L"\xE8D2", L"",
                             vm.address_search_content ? theme.accent : theme.text, 0.8f);
            }
        }
        if (layout.options.right > layout.options.left)
            button(layout.options, L"", L"\xE9E9", HitTestResult::AddressSearchOptions);
        // A single trailing button: clears the draft while editing, otherwise exits search.
        if (layout.close.right > layout.close.left) {
            const bool clears = vm.address_editing && vm.address_search_has_text;
            button(layout.close, L"", L"\xE711",
                   clears ? HitTestResult::AddressSearchClear : HitTestResult::AddressSearchClose);
        }

    } else {
        fluent::TextFieldSpec search;
        search.bounds = field;
        search.state = state;
        search.placeholder = vm.address_search_placeholder.empty()
            ? l10n::Get(l10n::StringId::Search) : vm.address_search_placeholder;
        search.leading_glyph = L"\xE721";
        search.trailing_keycap = L"Ctrl+K";
        search.suppress_text = true;
        painter_.DrawTextField(search);
        MakeBrush(compositor_->Dc(),theme.text_secondary,brTextSecondary_);
        const float text_left=field.left+36*scale_;
        const float text_right=field.right-14*scale_-painter_.MeasureBadgeWidth(search.trailing_keycap);
        DrawTextEndEllipsis(compositor_->Dc(),compositor_->DwriteFactory(),compositor_->TextFormat(),
            brTextSecondary_.get(),std::wstring(search.placeholder),text_left,field.top,
            std::max(0.0f,text_right-text_left),field.bottom-field.top);
    }
    if (vm.address_search_animation > 0.0f) {
        auto color = theme.accent;
        color.a *= vm.address_search_animation;
        ComPtr<ID2D1SolidColorBrush> brush;
        compositor_->Dc()->CreateSolidColorBrush(color, &brush);
        const float right = field.right - 6.0f * scale_;
        const float width = (field.right - field.left - 12.0f * scale_) * vm.address_search_animation;
        if (brush.get()) compositor_->Dc()->DrawLine(D2D1::Point2F(right - width, field.bottom - scale_),
            D2D1::Point2F(right, field.bottom - scale_), brush.get(), 2.0f * scale_);
    }
}

} // namespace pulse::ui
