// settings_ai_layout.h - Settings > 增强包 > AI 与 MCP (kSettingsPageAi): texts,
// geometry and hit testing. Included by ui_renderer_internal.h inside its
// anonymous namespace after settings_shortcuts_layout.h, so no includes here.
// Drawing: ui_settings_ai.cpp; clicks: HitTestResult::SettingsAi (AiHit),
// handled in app/ai_settings_ui.cpp; data: AiPageView.
//
// A sub-page of 增强包 (the nav keeps 增强包 selected): the switch and the
// client setup, then the apps that connected, the always-allow rules, this
// run's calls, and the pack's uninstall.

namespace ai_text {
inline const wchar_t* T(l10n::StringId id) { return l10n::Get(id).c_str(); }
inline const wchar_t* Title() { return T(l10n::StringId::AiPackTitle); }
inline const wchar_t* Intro() { return T(l10n::StringId::AiPageIntro); }
inline const wchar_t* EnableTitle() { return T(l10n::StringId::AiEnableTitle); }
inline const wchar_t* SetupTitle() { return T(l10n::StringId::AiSetupTitle); }
inline const wchar_t* SetupDesc() { return T(l10n::StringId::AiSetupDesc); }
inline const wchar_t* CopyConfig() { return T(l10n::StringId::AiCopyConfig); }
inline const wchar_t* Broken() { return T(l10n::StringId::AiSettingsBroken); }
inline const wchar_t* AppsSection() { return T(l10n::StringId::AiAppsSection); }
inline const wchar_t* AppsEmpty() { return T(l10n::StringId::AiAppsEmpty); }
inline const wchar_t* Allow() { return T(l10n::StringId::AiConnectAllow); }
inline const wchar_t* Deny() { return T(l10n::StringId::AiDeny); }
inline const wchar_t* RemoveEntry() { return T(l10n::StringId::AiRemoveEntry); }
inline const wchar_t* RulesSection() { return T(l10n::StringId::AiRulesSection); }
inline const wchar_t* RulesEmpty() { return T(l10n::StringId::AiRulesEmpty); }
inline const wchar_t* RecentSection() { return T(l10n::StringId::AiRecentSection); }
inline const wchar_t* RecentEmpty() { return T(l10n::StringId::AiRecentEmpty); }
inline const wchar_t* OpenLog() { return T(l10n::StringId::AiOpenLog); }
inline const wchar_t* PackSection() { return T(l10n::StringId::AiPackToastTitle); }
inline const wchar_t* UninstallTitle() { return T(l10n::StringId::AiUninstallTitle); }
inline const wchar_t* UninstallDesc() { return T(l10n::StringId::AiUninstallDesc); }
inline const wchar_t* Uninstall() { return T(l10n::StringId::Remove_2); }
inline const wchar_t* Grant(AiGrantView grant) {
    switch (grant) {
        case AiGrantView::Allowed: return T(l10n::StringId::AiGrantAllowed);
        case AiGrantView::Session: return T(l10n::StringId::AiGrantSession);
        case AiGrantView::Denied: return T(l10n::StringId::AiGrantDenied);
        default: return T(l10n::StringId::AiGrantAsk);
    }
}
inline std::wstring ClientDetail(const AiClientRowView& c) {
    std::wstring text = Grant(c.grant);
    if (c.connected) text += std::wstring(L"  \u00B7  ") + T(l10n::StringId::AiConnectedNow);
    return text;
}
inline std::wstring RuleDetail(const AiRuleRowView& r) { return r.client + L"  \u00B7  " + r.folder; }
inline std::wstring MoreCalls(size_t n) {
    std::wstring text = T(l10n::StringId::AiMoreCalls);
    if (const size_t at = text.find(L"{n}"); at != std::wstring::npos) text.replace(at, 3, std::to_wstring(n));
    return text;
}
} // namespace ai_text

inline float LayoutSettingsAi(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                              const fluent::Painter* painter) {
    AiPageLayout& o = l.ai;
    o = {};
    const AiPageView* v = vm.settings_ai.get();
    const float left = l.content.left + 20.0f * s, right = l.content.right - 20.0f * s;
    const float text_left = left + 54.0f * s, inner_right = right - 16.0f * s;
    auto caption_h = [&](std::wstring_view text, float width) {
        if (text.empty()) return 0.0f;
        const float h = painter ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40.0f * s, width)) : 0.0f;
        return h > 0.0f ? h : 20.0f * s;
    };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 88.0f * s; };
    auto centered = [&](const D2D1_RECT_F& row, float right_edge, float width) {
        const float cy = (row.top + row.bottom) * 0.5f;
        return D2D1::RectF(right_edge - width, cy - 16.0f * s, right_edge, cy + 16.0f * s);
    };
    auto section = [&]() {
        y += 24.0f * s;
        const auto r = D2D1::RectF(left, y, right, y + 28.0f * s);
        y = r.bottom;
        return r;
    };
    // A row with a title and a wrapped caption left of `controls_left`.
    auto text_row = [&](std::wstring_view caption, float controls_left) {
        const float h = (std::max)(64.0f * s, 36.0f * s + caption_h(caption, controls_left - 14.0f * s - text_left) + 14.0f * s);
        const auto r = D2D1::RectF(left, y, right, y + h);
        y = r.bottom;
        return r;
    };

    y += caption_h(ai_text::Intro(), right - left) + 18.0f * s;
    if (!v) return y;

    // 允许 AI 应用连接 / 连接 Claude.
    const float card_top = y;
    o.enable_row = text_row(v->status, inner_right - 42.0f * s);
    o.toggle = centered(o.enable_row, inner_right, 42.0f * s);
    const float copy_w = button_w(ai_text::CopyConfig());
    o.setup_row = text_row(ai_text::SetupDesc(), inner_right - copy_w);
    o.copy = centered(o.setup_row, inner_right, copy_w);
    if (v->broken) {
        const float h = caption_h(ai_text::Broken(), inner_right - text_left - 12.0f * s);
        o.broken = D2D1::RectF(text_left - 8.0f * s, y, inner_right, y + h + 12.0f * s);
        y = o.broken.bottom + 14.0f * s;
    }
    o.main_card = D2D1::RectF(left, card_top, right, y);

    // 应用: one row per client that has connected.
    o.apps_section = section();
    const float apps_top = y;
    if (v->clients.empty()) {
        o.apps_empty = D2D1::RectF(left, y, right, y + caption_h(ai_text::AppsEmpty(), right - left - 32.0f * s) + 28.0f * s);
        y = o.apps_empty.bottom;
    }
    const float forget_w = button_w(ai_text::RemoveEntry());
    const float allow_w = button_w(ai_text::Allow()), deny_w = button_w(ai_text::Deny());
    for (size_t i = 0; i < v->clients.size() && i < static_cast<size_t>(kAiRowLimit); ++i) {
        const AiClientRowView& c = v->clients[i];
        AiClientRowLayout row;
        row.row = D2D1::RectF(left, y, right, y + 56.0f * s);
        y = row.row.bottom;
        row.forget = centered(row.row, inner_right, forget_w);
        float x = row.forget.left - 8.0f * s;
        if (c.grant != AiGrantView::Denied) {
            row.deny = centered(row.row, x, deny_w);
            x = row.deny.left - 8.0f * s;
        }
        if (c.grant != AiGrantView::Allowed) row.allow = centered(row.row, x, allow_w);
        o.clients.push_back(row);
    }
    o.apps_card = D2D1::RectF(left, apps_top, right, y);

    // 始终允许: the rules "新建确认" added.
    o.rules_section = section();
    const float rules_top = y;
    if (v->rules.empty()) {
        o.rules_empty = D2D1::RectF(left, y, right, y + caption_h(ai_text::RulesEmpty(), right - left - 32.0f * s) + 28.0f * s);
        y = o.rules_empty.bottom;
    }
    for (size_t i = 0; i < v->rules.size() && i < static_cast<size_t>(kAiRowLimit); ++i) {
        const auto r = D2D1::RectF(left, y, right, y + 56.0f * s);
        y = r.bottom;
        o.rules.push_back(r);
        o.rule_remove.push_back(centered(r, inner_right, forget_w));
    }
    o.rules_card = D2D1::RectF(left, rules_top, right, y);

    // 本次运行: the newest calls, 打开活动记录 on the section row.
    o.recent_section = section();
    {
        const float w = button_w(ai_text::OpenLog());
        o.open_log = D2D1::RectF(right - w, o.recent_section.top - 6.0f * s, right, o.recent_section.top + 26.0f * s);
    }
    y += 8.0f * s;
    const float recent_top = y;
    if (v->calls.empty()) {
        o.recent_empty = D2D1::RectF(left, y, right, y + caption_h(ai_text::RecentEmpty(), right - left - 32.0f * s) + 28.0f * s);
        y = o.recent_empty.bottom;
    } else {
        y += 6.0f * s;
        for (size_t i = 0; i < v->calls.size(); ++i) {
            const auto r = D2D1::RectF(left, y, right, y + 36.0f * s);
            y = r.bottom;
            o.calls.push_back(r);
        }
        if (v->more_calls) {
            o.more_calls = D2D1::RectF(left, y, right, y + caption_h(ai_text::MoreCalls(v->more_calls), right - left - 32.0f * s) + 16.0f * s);
            y = o.more_calls.bottom;
        }
        y += 6.0f * s;
    }
    o.recent_card = D2D1::RectF(left, recent_top, right, y);

    // AI 增强包: uninstall (not for a bundled development host).
    if (v->can_uninstall) {
        o.pack_section = section();
        const float w = button_w(ai_text::Uninstall());
        o.uninstall_row = text_row(ai_text::UninstallDesc(), inner_right - w);
        o.uninstall = centered(o.uninstall_row, inner_right, w);
    }
    return y + 24.0f * s;
}

inline int HitTestSettingsAi(const SettingsLayout& l, const WindowViewModel& vm, float x, float y) {
    const AiPageLayout& o = l.ai;
    const AiPageView* v = vm.settings_ai.get();
    if (!v) return -1;
    auto in = [&](const D2D1_RECT_F& r) { return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(o.copy)) return kAiCopyConfig;
    if (in(o.enable_row)) return kAiToggle;   // the whole row, like the other switch rows
    if (in(o.open_log)) return kAiOpenLog;
    if (in(o.uninstall)) return kAiUninstall;
    for (size_t i = 0; i < o.clients.size(); ++i) {
        const int row = static_cast<int>(i);
        if (in(o.clients[i].allow)) return kAiClientAllowBase + row;
        if (in(o.clients[i].deny)) return kAiClientDenyBase + row;
        if (in(o.clients[i].forget)) return kAiClientForgetBase + row;
    }
    for (size_t i = 0; i < o.rule_remove.size(); ++i)
        if (in(o.rule_remove[i])) return kAiRuleRemoveBase + static_cast<int>(i);
    return -1;
}
