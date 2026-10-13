#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace pulse::ui {

// Settings > 快速预览 (page 5). Geometry: LayoutSettingsPacks; texts:
// pack_text (settings_layout_sections.h); clicks: SettingsPackAction.
void MainRenderer::DrawSettingsPacks(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    auto* dc = compositor_->Dc();
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const float s = scale_;
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
    auto hovered = [&](PackAction action) {
        return IsHovered(vm, H::SettingsPackAction, static_cast<int>(action));
    };
    auto hover_row = [&](D2D1_RECT_F r, PackAction action) {
        if (r.bottom <= r.top || !hovered(action)) return;
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), r.left + 4*s, r.top + 2*s,
                        r.right - r.left - 8*s, r.bottom - r.top - 4*s, 4*s);
    };
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, PackAction action,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard, bool enabled = true) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hovered(action);
        painter_.DrawButton({r, label, {}, kind, state});
    };
    auto toggle = [&](D2D1_RECT_F bounds, bool on, PackAction action) {
        fluent::ControlState state{};
        state.checked = on;
        state.hovered = hovered(action);
        painter_.DrawSwitch(bounds, L"", state);
    };
    auto row_switch = [&](D2D1_RECT_F row) {
        const float cy = (row.top + row.bottom) * 0.5f;
        return D2D1::RectF(row.right - 16*s - 42*s, cy - 16*s, row.right - 16*s, cy + 16*s);
    };
    auto setting = [&](D2D1_RECT_F r, const wchar_t* icon, std::wstring_view title, std::wstring_view desc) {
        const float right = r.right - 72*s;
        DrawIconText(r.left + 16*s, r.top + 20*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(r.left + 54*s, r.top + 10*s, right, r.top + 34*s));
        painter_.DrawWrappedCaption(desc, D2D1::Point2F(r.left + 54*s, r.top + 35*s),
                                    right - r.left - 54*s, theme.text_secondary);
    };

    // Format availability and pack controls share the same current settings model.
    const auto source_color = [&](PreviewFormatSource source) {
        if (source == PreviewFormatSource::Pack) {
            auto color = theme.accent;
            if (vm.dark) {
                color.r += (1.0f - color.r) * 0.5f;
                color.g += (1.0f - color.g) * 0.5f;
                color.b += (1.0f - color.b) * 0.5f;
            }
            return color;
        }
        if (source == PreviewFormatSource::PackOrSystem)
            return vm.dark ? HexColor(0xB5C6E5) : HexColor(0x4F5D77);
        return theme.text;
    };
    const auto chip = [&](D2D1_RECT_F bounds, std::wstring_view label, PreviewFormatState support) {
        const auto color = source_color(support.source);
        const auto rounded = D2D1::RoundedRect(bounds, (bounds.bottom - bounds.top)/2, (bounds.bottom - bounds.top)/2);
        MakeBrush(dc, WithAlpha(color, vm.dark ? 0.13f : 0.08f), brFillHover_);
        if (support.available) dc->FillRoundedRectangle(rounded, brFillHover_.get());
        else {
            if (!dashStroke_.get()) {
                ComPtr<ID2D1Factory> factory;
                dc->GetFactory(&factory);
                auto properties = D2D1::StrokeStyleProperties();
                properties.dashStyle = D2D1_DASH_STYLE_DASH;
                factory->CreateStrokeStyle(properties, nullptr, 0, &dashStroke_);
            }
            MakeBrush(dc, WithAlpha(color, 0.65f), brStrokeCard_);
            dc->DrawRoundedRectangle(rounded, brStrokeCard_.get(), s, dashStroke_.get());
        }
        painter_.DrawText(label, bounds, compositor_->SmallFormat(), color, fluent::HorizontalAlignment::Center);
    };
    // 预览方式: hover card or Space only, then the hover delay.
    auto choice_row = [&](D2D1_RECT_F r, const D2D1_RECT_F* choices, int count, int checked,
                          PackAction first, const wchar_t* icon, std::wstring_view title,
                          std::wstring_view desc, const wchar_t* (*label)(int)) {
        if (r.bottom <= r.top) return;
        card(r);
        const bool stacked = choices[0].left < r.left + 100*s;
        const float right = stacked ? r.right - 16*s : choices[0].left - 12*s;
        DrawIconText(r.left + 16*s, r.top + 20*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(r.left + 54*s, r.top + 10*s, right, r.top + 34*s));
        painter_.DrawWrappedCaption(desc, D2D1::Point2F(r.left + 54*s, r.top + 35*s),
                                    right - r.left - 54*s, theme.text_secondary);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left, choices[0].top,
                                                choices[count - 1].right, choices[count - 1].bottom));
        for (int i = 0; i < count; ++i) {
            fluent::SegmentedItemSpec item{};
            item.bounds = choices[i];
            item.text = label(i);
            item.state.checked = i == checked;
            item.state.hovered = hovered(static_cast<PackAction>(static_cast<int>(first) + i));
            item.shared_track = true;
            item.position = i == 0 ? fluent::SegmentPosition::First
                : i == count - 1 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    };
    text(pack_text::TriggerSection(), lay.trigger_section);
    const int trigger = vm.settings_preview_trigger == 1 ? 1 : 0;
    choice_row(lay.trigger_row, lay.trigger_choice, 2, trigger, PackAction::TriggerHover, L"\xE890",
               pack_text::TriggerTitle(), pack_text::TriggerDesc(trigger), &pack_text::TriggerChoice);
    const int delay = vm.settings_hover_delay_ms == 300 ? 0 : vm.settings_hover_delay_ms == 800 ? 2 : 1;
    choice_row(lay.hover_delay_row, lay.hover_delay_choice, 3, delay, PackAction::HoverDelayShort, L"\xE916",
               pack_text::DelayTitle(), pack_text::DelayDesc(), &pack_text::DelayChoice);

    text(pack_text::FormatsSection(), lay.preview_section);
    card(lay.preview_group);
    const auto header = lay.disclosure[2];
    if (IsHovered(vm, H::SettingsDisclosure, 2)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), header.left + 2*s, header.top + 2*s,
                        header.right - header.left - 4*s, header.bottom - header.top - 4*s, 6*s);
    }
    DrawIconText(header.left + 16*s, header.top + 20*s, 24*s, 24*s, L"\xE8A5", L"", theme.text_secondary, 0.85f);
    text(pack_text::FormatsTitle(), lay.preview_header_title);
    painter_.DrawWrappedCaption(pack_text::FormatsSummary(vm),
        D2D1::Point2F(lay.preview_header_summary.left, lay.preview_header_summary.top),
        lay.preview_header_summary.right - lay.preview_header_summary.left, theme.text_secondary);
    const bool formats_open = (vm.settings_expanded & 4u) != 0;
    DrawIconText(header.right - 38*s, (header.top + header.bottom)/2 - 9*s, 18*s, 18*s,
                 formats_open ? L"\xE70D" : L"\xE76C", L"", theme.text_secondary, 0.75f);
    if (formats_open) {
        for (int i = 0; i < 3; ++i)
            chip(lay.preview_legend_chip[i], pack_text::Legend(i), {static_cast<PreviewFormatSource>(i), i == 0});
        painter_.DrawWrappedCaption(pack_text::LegendNote(),
            D2D1::Point2F(lay.preview_legend_note.left, lay.preview_legend_note.top),
            lay.preview_legend_note.right - lay.preview_legend_note.left, theme.text_secondary);
        const auto format_divider = [&](float y) {
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            FillRect(dc, brStrokeDivider_.get(), lay.preview_group.left, y,
                     lay.preview_group.right - lay.preview_group.left, 1);
        };
        format_divider(lay.preview_legend.bottom);
        std::vector<PreviewFormatChip> chips;
        std::vector<PreviewFormatRow> rows;
        LayoutPreviewFormats(lay.preview_formats, s, &chips, &rows,
            [&](std::wstring_view value, float width) { return painter_.MeasureWrappedCaptionHeight(value, width); });
        const auto& groups = PreviewFormatGroups();
        for (size_t g = 0; g < rows.size() && g < groups.size(); ++g) {
            text(groups[g].name, rows[g].name);
            const auto& note = rows[g].note;
            if (note.bottom > note.top && !groups[g].note.empty()) {
                painter_.DrawWrappedCaption(groups[g].note, D2D1::Point2F(note.left, note.top),
                                            note.right - note.left, theme.text_secondary);
            }
            if (g + 1 < rows.size()) format_divider(rows[g].bounds.bottom);
        }
        for (const auto& item : chips)
            chip(item.rect, groups[item.group].extensions[item.index], PreviewFormatSupport(vm, item.group, item.index));
    }

    text(pack_text::SystemTitle(), lay.preview_codec_section);
    card(lay.preview_codec_group);
    const bool detected = (vm.settings_preview_codecs & kPreviewCodecsDetected) != 0;
    for (int i = 0; i < kPreviewCodecCount; ++i) {
        const auto info = PreviewCodec(i);
        const auto& r = lay.preview_codec_row[i];
        const auto& bounds = lay.preview_codec_text[i];
        const float title_h = 26*s*static_cast<float>(vm.settings_ui_font_scale)/100;
        DrawIconText(r.left + 16*s, r.top + 17*s, 24*s, 24*s,
            i == 1 || i == 2 ? L"\xE714" : L"\xE91B", L"", theme.text_secondary, 0.85f);
        text(info.name, D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + title_h));
        painter_.DrawWrappedCaption(info.description, D2D1::Point2F(bounds.left, bounds.top + title_h),
            bounds.right - bounds.left, theme.text_secondary);
        const bool has = (vm.settings_preview_codecs & (1u << i)) != 0;
        const auto status = !detected ? l10n::Pick(L"检测中", L"Checking") : has
            ? l10n::Pick(L"已安装", L"Installed") : l10n::Pick(L"未安装", L"Not installed");
        fluent::BadgeSpec badge;
        badge.bounds = lay.preview_codec_badge[i]; badge.text = status;
        badge.kind = !detected ? fluent::BadgeKind::Neutral : has ? fluent::BadgeKind::Success : fluent::BadgeKind::Warning;
        painter_.DrawBadge(badge);
        const auto& get = lay.preview_codec_button[i];
        if (get.right > get.left) {
            fluent::ControlState state{};
            state.hovered = IsHovered(vm, H::SettingsPreviewStore, i);
            painter_.DrawButton({get, l10n::Pick(L"获取", L"Get"), {}, fluent::ButtonKind::Standard, state});
        }
        divider_below(r);
    }
    painter_.DrawWrappedCaption(pack_text::SystemHint(vm),
        D2D1::Point2F(lay.preview_codec_hint.left, lay.preview_codec_hint.top),
        lay.preview_codec_hint.right - lay.preview_codec_hint.left, theme.text_secondary);
    text(pack_text::PacksSection(), lay.pack_section);

    // Summary: count, disk use and location.
    {
        const auto& r = lay.pack_summary;
        card(r);
        DrawIconText(r.left + 16*s, r.top + 24*s, 24*s, 24*s, L"\xE7B8", L"", theme.text_secondary, 0.85f);
        const bool compact = r.right - r.left < 560*s*static_cast<float>(vm.settings_ui_font_scale)/100;
        const float right = compact ? r.right - 16*s : lay.pack_open.left - 12*s;
        text(pack_text::Summary(vm), D2D1::RectF(r.left + 54*s, r.top + 14*s, right, r.top + 38*s));
        text(vm.settings_pack_root, D2D1::RectF(r.left + 54*s, r.top + 38*s, right, r.top + 58*s), true);
        button(lay.pack_open, pack_text::OpenFolder(), PackAction::OpenFolder);
    }

    // One pack card (geometry from LayoutSettingsPacks' card_layout).
    struct CardSpec {
        D2D1_RECT_F card, badge, enable, primary, notice_box;
        const wchar_t* icon;
        std::wstring_view title, desc, meta;
        pack_text::Badge badge_text;
        bool shows_enable, enabled, installed, installing, available;
        float progress;
        std::wstring_view primary_label;
        PackAction enable_action, install_action, remove_action;
        const std::wstring& notice;
    };
    auto draw_card = [&](const CardSpec& c) {
        const auto& r = c.card;
        card(r);
        const float text_left = r.left + 54*s, inner_right = r.right - 16*s;
        DrawIconText(r.left + 16*s, r.top + 18*s, 24*s, 24*s, c.icon, L"", theme.accent, 0.9f);
        const bool stacked = c.badge.top > r.top + 30*s;
        text(c.title, D2D1::RectF(text_left, r.top + 12*s,
            stacked ? inner_right : c.badge.left - 12*s, r.top + 40*s));
        fluent::BadgeSpec spec{};
        spec.bounds = c.badge;
        spec.text = c.badge_text.text;
        spec.kind = c.badge_text.kind;
        painter_.DrawBadge(spec);
        if (c.shows_enable) toggle(c.enable, c.enabled, c.enable_action);
        const float desc_top = stacked ? c.badge.bottom + 12*s : r.top + 40*s;
        painter_.DrawWrappedCaption(c.desc, D2D1::Point2F(text_left, desc_top),
                                    inner_right - text_left, theme.text_secondary);
        const float meta_right = c.primary.left - 12*s;
        if (c.installing) {
            // Status above a thin bar, both centred on the button's row.
            const float cy = (c.primary.top + c.primary.bottom) * 0.5f;
            text(pack_text::Downloading(c.progress), D2D1::RectF(text_left, cy - 16*s, meta_right, cy + 2*s), true);
            fluent::ProgressSpec bar;
            bar.bounds = D2D1::RectF(text_left, cy + 6*s, (std::min)(meta_right, text_left + 320*s), cy + 10*s);
            bar.value = c.progress;
            painter_.DrawProgressBar(bar);
        } else {
            text(c.meta, D2D1::RectF(text_left, c.primary.top + 6*s, meta_right, c.primary.bottom - 6*s), true);
        }
        const bool installed = c.installed && !c.installing;
        button(c.primary, c.primary_label, installed ? c.remove_action : c.install_action,
               installed || c.installing || !c.available ? fluent::ButtonKind::Standard : fluent::ButtonKind::Primary,
               pack_text::PrimaryEnabled(c.available, c.installed, c.installing));
        if (c.notice_box.bottom > c.notice_box.top) {
            const auto& n = c.notice_box;
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), n.left, n.top, n.right - n.left, n.bottom - n.top, 6*s);
            MakeBrush(dc, theme.accent, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), n.left, n.top + 6*s, 3*s, n.bottom - n.top - 12*s, 1.5f*s);
            painter_.DrawWrappedCaption(c.notice, D2D1::Point2F(n.left + 12*s, n.top + 6*s),
                                        n.right - n.left - 12*s, theme.text);
        }
    };

    // Media: the FFmpeg pack.
    text(pack_text::Media(), lay.pack_media_section);
    draw_card({lay.pack_card, lay.pack_badge, lay.pack_enable, lay.pack_primary, lay.pack_notice, L"\xE714",
               pack_text::MediaTitle(), pack_text::MediaDesc(), pack_text::MediaMeta(), pack_text::MediaBadge(vm),
               false, vm.settings_pack_ffmpeg_enabled, vm.settings_pack_media_installed,
               vm.settings_pack_installing, vm.settings_pack_media_available, vm.settings_pack_progress, pack_text::Primary(vm),
               PackAction::Enable, PackAction::Install, PackAction::Remove, vm.settings_pack_notice});

    // Images: the image pack.
    text(pack_text::Images(), lay.pack_images_section);
    draw_card({lay.pack_images_card, lay.pack_images_badge, lay.pack_images_enable, lay.pack_images_primary,
               lay.pack_images_notice, L"\xE91B",
               pack_text::ImagesTitle(), pack_text::ImagesDesc(), pack_text::ImagesMeta(), pack_text::ImagesBadge(vm),
               pack_text::ShowsImagesEnable(vm), vm.settings_pack_images_enabled, vm.settings_pack_images_installed,
               vm.settings_pack_images_installing, vm.settings_pack_images_available, vm.settings_pack_images_progress, pack_text::ImagesPrimary(vm),
               PackAction::ImagesEnable, PackAction::ImagesInstall, PackAction::ImagesRemove,
               vm.settings_pack_images_notice});

    // RAW camera photos.
    draw_card({lay.pack_raw_card, lay.pack_raw_badge, lay.pack_raw_enable, lay.pack_raw_primary,
               lay.pack_raw_notice, L"\xE722",
               pack_text::RawTitle(), pack_text::RawDesc(), pack_text::RawMeta(), pack_text::RawBadge(vm),
               pack_text::ShowsRawEnable(vm), vm.settings_pack_raw_enabled, vm.settings_pack_raw_installed,
               vm.settings_pack_raw_installing, vm.settings_pack_raw_available, vm.settings_pack_raw_progress, pack_text::RawPrimary(vm),
               PackAction::RawEnable, PackAction::RawInstall, PackAction::RawRemove,
               vm.settings_pack_raw_notice});

    // 7-Zip archive contents.
    text(pack_text::Archive(), lay.pack_archive_section);
    draw_card({lay.pack_archive_card, lay.pack_archive_badge, lay.pack_archive_enable, lay.pack_archive_primary,
               lay.pack_archive_notice, L"\xE8B7",
               pack_text::ArchiveTitle(), pack_text::ArchiveDesc(), pack_text::ArchiveMeta(), pack_text::ArchiveBadge(vm),
               pack_text::ShowsArchiveEnable(vm), vm.settings_pack_archive_enabled, vm.settings_pack_archive_installed,
               vm.settings_pack_archive_installing, vm.settings_pack_archive_available, vm.settings_pack_archive_progress, pack_text::ArchivePrimary(vm),
               PackAction::ArchiveEnable, PackAction::ArchiveInstall, PackAction::ArchiveRemove,
               vm.settings_pack_archive_notice});

    card(lay.pack_source_group);
    const bool source_switch = pack_text::ShowsEnable(vm);
    const float source_right = source_switch ? lay.pack_enable.left - 14*s : lay.pack_source_status.right - 16*s;
    text(pack_text::SourceTitle(), D2D1::RectF(lay.pack_source_status.left + 16*s,
        lay.pack_source_status.top + 10*s, source_right, lay.pack_source_status.top + 34*s));
    painter_.DrawWrappedCaption(pack_text::SourceStatus(vm),
        D2D1::Point2F(lay.pack_source_status.left + 16*s, lay.pack_source_status.top + 35*s),
        source_right - lay.pack_source_status.left - 16*s, theme.text_secondary);
    if (source_switch) toggle(lay.pack_enable, vm.settings_pack_ffmpeg_enabled, PackAction::Enable);
    divider_below(lay.pack_source_status);
    hover_row(lay.pack_custom_row, PackAction::UseCustom);
    setting(lay.pack_custom_row, L"\xE943", pack_text::CustomTitle(), pack_text::CustomDesc());
    toggle(row_switch(lay.pack_custom_row), vm.settings_pack_use_custom, PackAction::UseCustom);
    divider_below(lay.pack_custom_row);
    if (lay.pack_path_row.bottom > lay.pack_path_row.top) {
        const auto& r = lay.pack_path_row;
        const float right = r.right - 16*s;
        const std::wstring path = vm.settings_pack_custom_path.empty()
            ? std::wstring(pack_text::NoCustom()) : vm.settings_pack_custom_path;
        const bool active = vm.settings_pack_use_custom && vm.settings_pack_ffmpeg == 2;
        if (!pack_text::ShowsDetect(vm)) {
            painter_.DrawText(path, D2D1::RectF(r.left + 54*s, r.top + 14*s, right, r.top + 38*s),
                              compositor_->SmallFormat(), active ? theme.text : theme.text_secondary);
        } else {
            painter_.DrawText(path, D2D1::RectF(r.left + 54*s, r.top + 12*s, right, r.top + 34*s),
                              compositor_->SmallFormat(), active ? theme.text : theme.text_secondary);
            text(pack_text::Found(vm.settings_pack_detected_path),
                 D2D1::RectF(r.left + 54*s, r.top + 34*s, right, r.top + 54*s), true);
        }
        button(lay.pack_detect, pack_text::UseDetected(), PackAction::UseDetected);
        button(lay.pack_browse, pack_text::Browse(), PackAction::Browse);
        divider_below(r);
    }
    text(pack_text::Advanced(), lay.pack_advanced_section);
    card(lay.pack_group);
    hover_row(lay.pack_remove_row, PackAction::RemoveOnUninstall);
    setting(lay.pack_remove_row, L"\xE74D", pack_text::RemoveTitle(), pack_text::RemoveDesc());
    toggle(row_switch(lay.pack_remove_row), vm.settings_pack_remove_on_uninstall, PackAction::RemoveOnUninstall);

    painter_.DrawWrappedCaption(pack_text::Note(), D2D1::Point2F(lay.pack_note.left + 4*s, lay.pack_note.top),
                                lay.pack_note.right - lay.pack_note.left - 8*s, theme.text_secondary);
}

} // namespace pulse::ui
