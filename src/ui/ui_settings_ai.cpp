#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace pulse::ui {

// Settings > 增强包 > AI 与 MCP (kSettingsPageAi). Geometry: LayoutSettingsAi; texts:
// ai_text (settings_ai_layout.h); clicks: SettingsAi (AiHit).
void MainRenderer::DrawSettingsAi(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    const AiPageView* v = vm.settings_ai.get();
    if (!v) return;
    auto* dc = compositor_->Dc();
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const AiPageLayout& o = lay.ai;
    const float s = scale_;
    // The badges' success / warning inks (fluent_components.cpp), by theme.
    const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
    auto hex = [](uint32_t rgb) {
        return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
    };
    const D2D1_COLOR_F success = hex(dark ? 0x45D7A4 : 0x107C5A);
    const D2D1_COLOR_F warning = hex(dark ? 0xF7D154 : 0x8A5500);
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, bool is_small = false) {
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          is_small ? theme.text_secondary : theme.text);
    };
    auto card = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brStrokeCard_.get(), 1);
    };
    auto divider_below = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), r.left + 16*s, r.bottom, r.right - r.left - 32*s, 1);
    };
    auto hovered = [&](int target) { return IsHovered(vm, H::SettingsAi, target); };
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, int target,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.hovered = hovered(target);
        painter_.DrawButton({r, label, {}, kind, state});
    };
    // Title and caption of a row whose controls start at `controls_left`.
    auto row_text = [&](D2D1_RECT_F r, const wchar_t* icon, std::wstring_view title, std::wstring_view caption,
                        float controls_left) {
        DrawIconText(r.left + 16*s, r.top + 20*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        const float right = controls_left - 14*s;
        text(title, D2D1::RectF(r.left + 54*s, r.top + 12*s, right, r.top + 36*s));
        painter_.DrawWrappedCaption(caption, D2D1::Point2F(r.left + 54*s, r.top + 36*s),
                                    right - r.left - 54*s, theme.text_secondary);
    };
    auto empty_text = [&](D2D1_RECT_F r, std::wstring_view caption) {
        if (r.bottom <= r.top) return;
        painter_.DrawWrappedCaption(caption, D2D1::Point2F(r.left + 16*s, r.top + 14*s),
                                    r.right - r.left - 32*s, theme.text_secondary);
    };

    // 允许 AI 应用连接 / 连接 Claude.
    card(o.main_card);
    if (hovered(kAiToggle)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        const auto& r = o.enable_row;
        FillRoundedRect(dc, brFillHover_.get(), r.left + 4*s, r.top + 4*s, r.right - r.left - 8*s, r.bottom - r.top - 6*s, 4*s);
    }
    row_text(o.enable_row, L"\xE99A", ai_text::EnableTitle(), v->status, o.toggle.left);
    {
        fluent::ControlState state{};
        state.checked = v->enabled;
        state.hovered = hovered(kAiToggle);
        painter_.DrawSwitch(o.toggle, L"", state);
    }
    divider_below(o.enable_row);
    row_text(o.setup_row, L"\xE943", ai_text::SetupTitle(), ai_text::SetupDesc(), o.copy.left);
    button(o.copy, ai_text::CopyConfig(), kAiCopyConfig);
    if (o.broken.bottom > o.broken.top) {
        const auto& n = o.broken;
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), n.left, n.top, n.right - n.left, n.bottom - n.top, 6*s);
        MakeBrush(dc, warning, brFillSelected_);
        FillRoundedRect(dc, brFillSelected_.get(), n.left, n.top + 6*s, 3*s, n.bottom - n.top - 12*s, 1.5f*s);
        painter_.DrawWrappedCaption(ai_text::Broken(), D2D1::Point2F(n.left + 12*s, n.top + 6*s),
                                    n.right - n.left - 12*s, theme.text);
    }

    // 应用.
    text(ai_text::AppsSection(), o.apps_section);
    card(o.apps_card);
    empty_text(o.apps_empty, ai_text::AppsEmpty());
    for (size_t i = 0; i < o.clients.size() && i < v->clients.size(); ++i) {
        const AiClientRowView& c = v->clients[i];
        const AiClientRowLayout& r = o.clients[i];
        const int row = static_cast<int>(i);
        const float controls = r.allow.right > r.allow.left ? r.allow.left
                             : r.deny.right > r.deny.left ? r.deny.left : r.forget.left;
        // A green dot while it is connected.
        if (c.connected) {
            MakeBrush(dc, success, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), r.row.left + 24*s, r.row.top + 24*s, 8*s, 8*s, 4*s);
        } else {
            DrawIconText(r.row.left + 16*s, r.row.top + 16*s, 24*s, 24*s, L"\xE99A", L"", theme.text_secondary, 0.75f);
        }
        text(c.name, D2D1::RectF(r.row.left + 54*s, r.row.top + 8*s, controls - 12*s, r.row.top + 30*s));
        text(ai_text::ClientDetail(c), D2D1::RectF(r.row.left + 54*s, r.row.top + 30*s, controls - 12*s, r.row.top + 50*s), true);
        button(r.allow, ai_text::Allow(), kAiClientAllowBase + row);
        button(r.deny, ai_text::Deny(), kAiClientDenyBase + row);
        button(r.forget, ai_text::RemoveEntry(), kAiClientForgetBase + row);
        if (i + 1 < o.clients.size()) divider_below(r.row);
    }

    // 始终允许.
    text(ai_text::RulesSection(), o.rules_section);
    card(o.rules_card);
    empty_text(o.rules_empty, ai_text::RulesEmpty());
    for (size_t i = 0; i < o.rules.size() && i < v->rules.size(); ++i) {
        const AiRuleRowView& rule = v->rules[i];
        const auto& r = o.rules[i];
        const float controls = o.rule_remove[i].left - 12*s;
        DrawIconText(r.left + 16*s, r.top + 16*s, 24*s, 24*s, L"\xF012", L"", theme.text_secondary, 0.8f);
        text(rule.tool_label, D2D1::RectF(r.left + 54*s, r.top + 8*s, controls, r.top + 30*s));
        text(ai_text::RuleDetail(rule), D2D1::RectF(r.left + 54*s, r.top + 30*s, controls, r.top + 50*s), true);
        button(o.rule_remove[i], ai_text::RemoveEntry(), kAiRuleRemoveBase + static_cast<int>(i));
        if (i + 1 < o.rules.size()) divider_below(r);
    }

    // 本次运行.
    text(ai_text::RecentSection(), o.recent_section);
    button(o.open_log, ai_text::OpenLog(), kAiOpenLog);
    card(o.recent_card);
    empty_text(o.recent_empty, ai_text::RecentEmpty());
    for (size_t i = 0; i < o.calls.size() && i < v->calls.size(); ++i) {
        const AiCallRowView& call = v->calls[i];
        const auto& r = o.calls[i];
        const float outcome_w = (std::min)(200*s, (r.right - r.left) * 0.32f);
        text(call.time, D2D1::RectF(r.left + 16*s, r.top + 8*s, r.left + 96*s, r.bottom - 8*s), true);
        text(call.client + L"  \u00B7  " + call.tool_label,
             D2D1::RectF(r.left + 100*s, r.top + 8*s, r.right - 16*s - outcome_w - 8*s, r.bottom - 8*s));
        const D2D1_COLOR_F tone = call.tone == AiCallTone::Running ? theme.accent
                                : call.tone == AiCallTone::Failed ? theme.danger
                                : call.tone == AiCallTone::Declined ? warning : theme.text_secondary;
        painter_.DrawText(call.outcome, D2D1::RectF(r.right - 16*s - outcome_w, r.top + 8*s, r.right - 16*s, r.bottom - 8*s),
                          compositor_->SmallFormat(), tone, fluent::HorizontalAlignment::Right);
    }
    if (o.more_calls.bottom > o.more_calls.top)
        painter_.DrawWrappedCaption(ai_text::MoreCalls(v->more_calls), D2D1::Point2F(o.more_calls.left + 16*s, o.more_calls.top + 6*s),
                                    o.more_calls.right - o.more_calls.left - 32*s, theme.text_secondary);

    // AI 增强包.
    if (o.uninstall_row.bottom > o.uninstall_row.top) {
        text(ai_text::PackSection(), o.pack_section);
        card(o.uninstall_row);
        row_text(o.uninstall_row, L"\xE74D", ai_text::UninstallTitle(), ai_text::UninstallDesc(), o.uninstall.left);
        button(o.uninstall, ai_text::Uninstall(), kAiUninstall);
    }
}

} // namespace pulse::ui
