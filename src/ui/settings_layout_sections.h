// Included inside ui_renderer_internal.h's anonymous namespace.
// Default file manager card helpers shared by layout, drawing and tests.
struct IntegrationBadgeInfo { std::wstring text; fluent::BadgeKind kind = fluent::BadgeKind::Neutral; };
// One switch: on hands folders and drives, This PC and Win + E to Pulse together, so
// Explorer never ends up as a launcher that bounces every double-click to Pulse.
inline bool IntegrationHasProblem(const WindowViewModel& vm) {
    return vm.settings_integration_state==2 || vm.settings_integration_state==3;
}
inline IntegrationBadgeInfo MakeIntegrationBadge(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(vm.settings_integration_state==3) return {l10n::Get(I::IntegrationFailed),fluent::BadgeKind::Danger};
    if(vm.settings_integration_state==2) return {l10n::Get(I::IntegrationPartial),fluent::BadgeKind::Warning};
    if(!vm.settings_integration_enabled) return {l10n::Get(I::IntegrationOff),fluent::BadgeKind::Neutral};
    return {l10n::Get(I::IntegrationOn),fluent::BadgeKind::Success};
}
inline std::wstring IntegrationProblemTitle(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(vm.settings_integration_state==3) return l10n::Get(I::IntegrationFailTitle);
    return l10n::Get(vm.settings_integration_other_exe.empty() ? I::IntegrationDriftTitle : I::IntegrationOtherTitle);
}
inline std::wstring IntegrationProblemMessage(const WindowViewModel& vm) {
    if(vm.settings_integration_state==3)
        return vm.settings_integration_summary.empty() ? l10n::Get(l10n::StringId::IntegrationFailed) : vm.settings_integration_summary;
    if(vm.settings_integration_state!=2) return {};
    return vm.settings_integration_other_exe.empty() ? l10n::Get(l10n::StringId::IntegrationDriftDesc)
                                                     : vm.settings_integration_other_exe;
}
inline l10n::StringId IntegrationRetryLabel(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(vm.settings_integration_state==3) return I::IntegrationRetry;
    return vm.settings_integration_other_exe.empty() ? I::IntegrationReapply : I::IntegrationUseThis;
}
// The footer: one plain sentence about what happens now.
inline std::wstring IntegrationHint(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(!vm.settings_integration_enabled) return l10n::Get(I::IntegrationInactiveHint);
    if(vm.settings_integration_just_enabled) return l10n::Get(I::IntegrationJustOn);
    // Taking over works only while Pulse runs, and closing the window ends
    // Pulse unless it keeps running (AppPrefs::KeepsRunningInBackground).
    if(vm.settings_integration_experimental && !vm.settings_keep_running) return l10n::Get(I::IntegrationHintKeep);
    if(vm.settings_integration_experimental && !vm.settings_launch_on_startup) return l10n::Get(I::IntegrationHintLaunch);
    return l10n::Get(I::IntegrationOnFoot);
}
inline bool IntegrationShowsTry(const WindowViewModel& vm) {
    return vm.settings_integration_enabled && !IntegrationHasProblem(vm);
}
inline constexpr l10n::StringId kIntegrationCover[]={l10n::StringId::IntegrationFolders,
    l10n::StringId::ThisPc,l10n::StringId::SettingsWinE};
inline bool IntegrationCoverActive(const WindowViewModel& vm,int i) {
    return vm.settings_integration_enabled && (i==0 ? vm.settings_integration_folders_active
        : i==1 ? vm.settings_integration_this_pc_active : vm.settings_integration_win_e_active);
}

float LayoutSettingsGeneral(SettingsLayout& l, const WindowViewModel& vm, float scale,
                            float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20*scale, right = l.content.right - 20*scale;
    const bool narrow = right - left < 560*scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y+h*scale); y=r.bottom; return r; };
    auto section = [&](int i) { y+=24*scale; l.section[i]=row(28); };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left+16*scale : r.right-(width+16)*scale,
            r.bottom-44*scale, r.right-16*scale, r.bottom-12*scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c=choice(r,width); const float w=(c.right-c.left)/count;
        for(int i=0;i<count;++i) output[i]=D2D1::RectF(c.left+i*w,c.top,c.left+(i+1)*w,c.bottom);
    };
    section(0);
    l.theme_row=row(narrow ? 142.0f : 112.0f);
    const float tw=(std::min)(96*scale,(right-left-32*scale)/3);
    const float tile_left=narrow ? left+16*scale : right-16*scale-3*tw;
    for(int i=0;i<3;++i) l.theme_tile[i]=D2D1::RectF(tile_left+i*tw+4*scale,
        l.theme_row.bottom-90*scale,tile_left+(i+1)*tw-4*scale,l.theme_row.bottom-12*scale);
    l.accent_card=row(96);
    const float picker=kBloomPickerDip*scale;
    l.accent_picker=D2D1::RectF(right-16*scale-picker,l.accent_card.top+(96*scale-picker)/2,
        right-16*scale,l.accent_card.top+(96*scale+picker)/2);
    // The tiles reuse SettingsEffect's existing hit regions and controller.
    // A single tile selector avoids duplicate controls for the same setting.
    const int effect_columns = right-left < 440*scale ? 2 : 4;
    const int effect_rows = kWindowEffectCount/effect_columns;
    const float effect_top = 66.0f;
    l.effect_card=row(effect_top+82.0f*effect_rows+12.0f);
    l.effect_choice = {};
    const float effect_gap=8*scale;
    const float effect_width=(right-left-32*scale-effect_gap*(effect_columns-1))/effect_columns;
    for(int i=0;i<kWindowEffectCount;++i) {
        const float tile_x=left+16*scale+(i%effect_columns)*(effect_width+effect_gap);
        const float tile_y=l.effect_card.top+effect_top*scale+(i/effect_columns)*82*scale;
        l.effect_row[i]=D2D1::RectF(tile_x,tile_y,tile_x+effect_width,tile_y+74*scale);
    }
    l.menu_acrylic_row=row(64);
    l.language_card=row(narrow ? 98.0f : 64.0f); l.language_choice=choice(l.language_card,176);
    l.text_render_card=row(narrow ? 98.0f : 64.0f); segments(l.text_render_card,l.text_render_row,3,282);
    l.ui_font_size_card=row(narrow ? 98.0f : 64.0f); segments(l.ui_font_size_card,l.ui_font_size_row,4,340);
    l.group[0]=D2D1::RectF(left,l.theme_row.top,right,y);
    // Default file manager: a titled card of its own, independent of startup and background running.
    // The switch says what it does; the tiles below show what actually opens in Pulse.
    y+=24*scale; l.integration_section=row(28);
    const float integration_top=y;
    auto caption_height=[&](std::wstring_view value,float width) {
        return value.empty() ? 0.0f : painter
            ? painter->MeasureWrappedCaptionHeight(value,(std::max)(40*scale,width)) : 42*scale;
    };
    {
        const auto badge=MakeIntegrationBadge(vm);
        const float badge_w=painter ? painter->MeasureBadgeWidth(badge.text) : 96*scale;
        const float switch_left=right-60*scale;
        float text_right=switch_left-24*scale-badge_w;
        const bool stacked=text_right-(left+54*scale)<220*scale;
        if(stacked) text_right=switch_left-12*scale;
        const float desc_h=caption_height(l10n::Get(l10n::StringId::IntegrationMasterDesc),text_right-left-54*scale);
        const float body=35*scale+desc_h+(stacked ? 30*scale : 0.0f)+14*scale;
        l.default_manager_row=row((std::max)(64.0f,body/scale));
        const float top=l.default_manager_row.top;
        l.integration_badge=stacked
            ? D2D1::RectF(left+54*scale,top+35*scale+desc_h+6*scale,left+54*scale+badge_w,top+35*scale+desc_h+28*scale)
            : D2D1::RectF(text_right+12*scale,top+21*scale,text_right+12*scale+badge_w,top+43*scale);
        l.integration_text_right=text_right;
    }
    if(IntegrationHasProblem(vm)) {
        const float bar_left=left+16*scale, bar_right=right-16*scale;
        const float msg_h=caption_height(IntegrationProblemMessage(vm),bar_right-bar_left-60*scale);
        const auto retry_text=l10n::Get(IntegrationRetryLabel(vm));
        const float retry_w=!vm.settings_integration_can_retry ? 0.0f
            : painter ? painter->MeasureButtonWidth(retry_text) : 96*scale;
        const float restore_w=!vm.settings_integration_can_restore ? 0.0f
            : painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::IntegrationRestore)) : 200*scale;
        const float button_left=bar_left+52*scale, button_right=bar_right-12*scale;
        const bool stack=retry_w>0 && restore_w>0 && button_left+retry_w+8*scale+restore_w>button_right;
        const float buttons_h=(retry_w>0 || restore_w>0) ? (stack ? 72*scale : 32*scale) : 0.0f;
        y+=4*scale;
        l.integration_bar=D2D1::RectF(bar_left,y,bar_right,y+32*scale+msg_h+(buttons_h>0 ? 10*scale+buttons_h : 0.0f)+12*scale);
        y=l.integration_bar.bottom+12*scale;
        const float button_top=l.integration_bar.top+32*scale+msg_h+10*scale;
        if(retry_w>0) l.integration_retry=D2D1::RectF(button_left,button_top,(std::min)(button_left+retry_w,button_right),button_top+32*scale);
        if(restore_w>0) {
            const float x=retry_w>0 && !stack ? l.integration_retry.right+8*scale : button_left;
            const float top=retry_w>0 && stack ? button_top+40*scale : button_top;
            l.integration_restore=D2D1::RectF(x,top,(std::min)(x+restore_w,button_right),top+32*scale);
        }
    }
    l.integration_list_head=row(32);
    const float item_text_left=left+112*scale, item_text_right=right-16*scale;
    {
        // What opens in Pulse: three read-only tiles that wrap with the width.
        const float x0=left+54*scale, x1=right-16*scale, tile_h=36*scale, gap=8*scale;
        float x=x0, top=y;
        for(int i=0;i<3;++i) {
            const auto name=l10n::Get(kIntegrationCover[i]);
            // Button width is the label plus 16; the tile adds the icon (34), the check (22) and slack.
            const float w=(std::min)(x1-x0,(painter ? painter->MeasureButtonWidth(name) : 140*scale)+64*scale);
            if(x>x0 && x+w>x1) { x=x0; top+=tile_h+gap; }
            l.integration_cover[i]=D2D1::RectF(x,top,x+w,top+tile_h);
            x+=w+gap;
        }
        y=top+tile_h+6*scale;
    }
    l.integration_options_head=row(32);
    auto item_row=[&](l10n::StringId desc,float text_right) {
        const float h=caption_height(l10n::Get(desc),text_right-item_text_left);
        return row((std::max)(52.0f,(31*scale+h+12*scale)/scale));
    };
    l.recycle_bin_row=item_row(l10n::StringId::SettingsRecycleBinDesc,item_text_right);
    {
        // The experimental tag sits at the end of the title line.
        const auto tag=l10n::Get(l10n::StringId::IntegrationExperimentalTag);
        const float w=painter ? painter->MeasureBadgeWidth(tag) : 60*scale;
        l.explorer_windows_row=item_row(l10n::StringId::IntegrationExperimentalDesc,item_text_right);
        const float top=l.explorer_windows_row.top;
        l.integration_chip[0]=D2D1::RectF(item_text_right-w,top+9*scale,item_text_right,top+31*scale);
    }
    {
        // Footer: the try-it button (while on) and one sentence about what happens now.
        const auto hint=IntegrationHint(vm);
        const float foot_left=left+54*scale;
        y+=6*scale;
        const float top=y+12*scale;
        float text_left=foot_left, text_top=top+6*scale, bottom=top;
        if(IntegrationShowsTry(vm)) {
            const auto label=l10n::Get(l10n::StringId::IntegrationTry);
            const float w=painter ? painter->MeasureButtonWidth(label) : 180*scale;
            l.integration_try=D2D1::RectF(foot_left,top,(std::min)(foot_left+w,item_text_right),top+32*scale);
            bottom=l.integration_try.bottom;
            if(item_text_right-(l.integration_try.right+12*scale)>=200*scale) text_left=l.integration_try.right+12*scale;
            else text_top=l.integration_try.bottom+8*scale;
        }
        if(vm.settings_integration_just_enabled) text_left+=24*scale;
        const float text_h=caption_height(hint,item_text_right-text_left);
        l.integration_hint_text=D2D1::RectF(text_left,text_top,item_text_right,text_top+text_h);
        bottom=(std::max)(bottom,l.integration_hint_text.bottom);
        l.integration_hint=D2D1::RectF(left,y,right,bottom+14*scale);
        y=l.integration_hint.bottom;
    }
    l.integration_card=D2D1::RectF(left,integration_top,right,y);
    section(1);
    l.startup_row[0]=row(64); l.start_in_tray_row=row(64); l.startup_row[1]=row(64);
    l.notify_icon_card=row(narrow ? 98.0f : 64.0f); segments(l.notify_icon_card,l.notify_icon_row,3,282);
    l.home_folder_card=row(narrow ? 98.0f : 64.0f);
    {
        const float rw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::ThisPc)) : 80*scale;
        const float cw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::SettingsHomeFolderPick)) : 120*scale;
        l.home_folder_reset=D2D1::RectF(right-16*scale-rw,y-44*scale,right-16*scale,y-12*scale);
        l.home_folder_choose=D2D1::RectF(l.home_folder_reset.left-8*scale-cw,y-44*scale,l.home_folder_reset.left-8*scale,y-12*scale);
    }
    l.startup_open_card=row(narrow ? 98.0f : 64.0f); segments(l.startup_open_card,l.startup_open_row,2,282);
    l.new_tab_open_card=row(narrow ? 98.0f : 64.0f); segments(l.new_tab_open_card,l.new_tab_open_row,2,282);
    l.external_open_card=row(narrow ? 98.0f : 64.0f); segments(l.external_open_card,l.external_open_row,3,282);
    l.close_last_tab_row=row(64);
    l.close_tab_double_click_row=row(64);
    l.group[1]=D2D1::RectF(left,l.startup_row[0].top,right,y);
    section(2);
    l.density_card=row(narrow ? 98.0f : 64.0f); segments(l.density_card,l.density_row,3,282);
    l.performance_row=row(64);
    for(auto& list_row : l.list_style_row) list_row=row(64);
    l.folder_sort_card=row(narrow ? 98.0f : 64.0f); segments(l.folder_sort_card,l.folder_sort_row,3,282);
    l.confirm_delete_row=row(64);
    l.group[2]=D2D1::RectF(left,l.density_card.top,right,y);
    y+=18*scale;
    l.disclosure[0]=row(64);
    if(vm.settings_expanded & 1u) {
        y+=10*scale;
        l.wallpaper_card=row(124);
        l.wallpaper_preview=D2D1::RectF(left+16*scale,l.wallpaper_card.top+12*scale,left+112*scale,l.wallpaper_card.top+68*scale);
        const float cw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::Clear)) : 80*scale;
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::ChooseImage)) : 120*scale;
        l.wallpaper_clear=D2D1::RectF(right-16*scale-cw,y-44*scale,right-16*scale,y-12*scale);
        l.wallpaper_choose=D2D1::RectF(l.wallpaper_clear.left-8*scale-bw,y-44*scale,l.wallpaper_clear.left-8*scale,y-12*scale);
        y+=8*scale; l.wallpaper_look_card=row(narrow ? 98.0f : 64.0f); segments(l.wallpaper_look_card,l.wallpaper_look_row,3,282);
        y+=8*scale; l.wallpaper_blur_card=row(narrow ? 98.0f : 64.0f); segments(l.wallpaper_blur_card,l.wallpaper_blur_row,3,282);
        y+=8*scale; l.tray_icon_card=row(narrow ? 98.0f : 64.0f); segments(l.tray_icon_card,l.tray_icon_row,3,282);
        y+=8*scale; l.shell_tags_row=row(64);
        y+=8*scale; l.hidden_files_row=row(64);
        y+=8*scale; l.protected_files_row=row(64);
        y+=8*scale; l.pinned_names_row=row(64);
        y+=8*scale; l.vertical_tabs_row=row(64);
        y+=8*scale; l.hints_row=row(64);
        y+=8*scale; l.hints_reset_row=row(64);
        {
            const float rw=painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::HintsResetButton)) : 80*scale;
            l.hints_reset_button=D2D1::RectF(right-16*scale-rw,l.hints_reset_row.top+16*scale,right-16*scale,l.hints_reset_row.top+48*scale);
        }
        y+=8*scale; l.blank_click_row=row(narrow ? 98.0f : 64.0f); segments(l.blank_click_row,l.blank_click_choice,3,282);
        y+=8*scale; l.change_tracking_row=row(64);
        l.change_days_row=row(narrow ? 98.0f : 64.0f); segments(l.change_days_row,l.change_days,3,282);
    }
    y+=12*scale; l.footer=row(28); y+=16*scale;
    return y;
}

float LayoutSettingsContent(SettingsLayout& l, const WindowViewModel& vm, float scale,
                           float y, const fluent::Painter* painter) {
    const float left=l.content.left+20*scale,right=l.content.right-20*scale;
    const bool narrow=right-left<560*scale;
    auto row=[&](float h) { auto r=D2D1::RectF(left,y,right,y+h*scale);y=r.bottom;return r; };
    y+=14*scale; l.section[1]=row(28);
    l.content_header=row(72);
    l.content_types=row(narrow ? 236.0f : 160.0f);
    if(vm.settings_content_folders.empty()) l.content_empty=row(84);
    else if (!vm.settings_content_instant) {
        auto r=row(52);
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Get(vm.settings_content_paused ? l10n::StringId::ContentIndexResume : l10n::StringId::ContentIndexPause)) : 150*scale;
        l.content_pause=D2D1::RectF(left+16*scale,r.top+10*scale,left+16*scale+bw,r.bottom-10*scale);
    }
    l.content_options=row(64);
    if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) l.content_rebuild=row(64);
    l.group[1]=D2D1::RectF(left,l.content_header.top,right,y);
    y+=8*scale; l.footer=row(40);
    y+=24*scale;
    return y;
}

// ---- 预览增强包 page (5) ---------------------------------------------------
// Texts shared by layout (measuring) and drawing. l10n::Pick converts to
// Traditional Chinese at run time, like the Quick Look format catalog.
namespace pack_text {
inline const wchar_t* Title() { return l10n::Get(l10n::StringId::QuickPreview).c_str(); }
inline const wchar_t* Intro() { return l10n::Get(l10n::StringId::HoverOverOrPressSpaceToPreviewFilesChooseHowExploreFormatSup).c_str(); }
inline const wchar_t* TriggerSection() { return l10n::Get(l10n::StringId::PreviewTrigger).c_str(); }
inline const wchar_t* TriggerTitle() { return l10n::Get(l10n::StringId::OpenPreviews).c_str(); }
inline const wchar_t* TriggerDesc(int trigger) {
    return trigger == 1
        ? l10n::Get(l10n::StringId::SelectAFileAndPressSpace).c_str()
        : l10n::Get(l10n::StringId::RestThePointerOnAFileForAPreviewCardSpaceStillOpensTheFullPr).c_str();
}
inline const wchar_t* TriggerChoice(int i) {
    return i == 0 ? l10n::Get(l10n::StringId::Hover).c_str() : l10n::Get(l10n::StringId::Space).c_str();
}
inline const wchar_t* DelayTitle() { return l10n::Get(l10n::StringId::HoverDelay).c_str(); }
inline const wchar_t* DelayDesc() { return l10n::Get(l10n::StringId::HowLongThePointerRestsBeforeTheCardAppears).c_str(); }
inline const wchar_t* DelayChoice(int i) {
    if (i == 0) return l10n::Get(l10n::StringId::S03S).c_str();
    if (i == 1) return l10n::Get(l10n::StringId::S05S).c_str();
    return l10n::Get(l10n::StringId::S08S).c_str();
}
inline const wchar_t* SizeTitle() { return l10n::Get(l10n::StringId::CardSize).c_str(); }
inline const wchar_t* SizeDesc() {
    return l10n::Get(l10n::StringId::FollowsTheContentSShapeHoldCtrlToEnlarge).c_str();
}
inline const wchar_t* SizeChoice(int i) {
    if (i == 0) return l10n::Get(l10n::StringId::UiFontSmall).c_str();
    if (i == 1) return l10n::Get(l10n::StringId::Medium).c_str();
    return l10n::Get(l10n::StringId::UiFontLarge).c_str();
}
inline const wchar_t* PacksSection() { return l10n::Get(l10n::StringId::PreviewPacks).c_str(); }
inline const wchar_t* PacksIntro() { return l10n::Get(l10n::StringId::PacksPageIntro).c_str(); }
inline const wchar_t* ManagePacks() { return l10n::Get(l10n::StringId::ManagePacks).c_str(); }
inline const wchar_t* SearchSection() { return l10n::Get(l10n::StringId::PackSearchSection).c_str(); }
inline const wchar_t* ContentTitle() { return l10n::Get(l10n::StringId::ContentPackTitle).c_str(); }
inline const wchar_t* ContentDesc() { return l10n::Get(l10n::StringId::ContentPackDesc).c_str(); }
inline const wchar_t* ContentMeta() { return l10n::Get(l10n::StringId::ContentPackMeta).c_str(); }
inline const wchar_t* OcrTitle() { return l10n::Get(l10n::StringId::OcrPackTitle).c_str(); }
inline const wchar_t* OcrDesc() { return l10n::Get(l10n::StringId::OcrPackDesc).c_str(); }
inline const wchar_t* ComingSoon() { return l10n::Get(l10n::StringId::PackComingSoon).c_str(); }
// app::ContentPackState: 0 bundled (dev build), 1 ready, 2 missing, 3 outdated, 4 damaged, 5 unavailable.
inline bool ContentReady(const WindowViewModel& vm) {
    return vm.settings_content_pack_state == 0 || vm.settings_content_pack_state == 1;
}
inline bool ContentAvailable(const WindowViewModel& vm) { return vm.settings_content_pack_state != 5; }
inline const wchar_t* ContentPrimary(const WindowViewModel& vm) {
    return ContentReady(vm) ? l10n::Get(l10n::StringId::ManageContentIndex).c_str()
                            : l10n::Get(l10n::StringId::DownloadAndInstall).c_str();
}
inline std::wstring UpdateAll(int count) {
    return std::wstring(l10n::Get(l10n::StringId::UpdateAllPacks).c_str()) + L" (" + std::to_wstring(count) + L")";
}
inline const wchar_t* FormatsSection() { return l10n::Get(l10n::StringId::Formats).c_str(); }
inline std::wstring FormatsSummary(const WindowViewModel& vm) {
    const int packs = int(vm.settings_pack_media_installed) + int(vm.settings_pack_images_installed) +
                      int(vm.settings_pack_raw_installed) + int(vm.settings_pack_archive_installed);
    int codecs = 0;
    for (int i = 0; i < kPreviewCodecCount; ++i) if (vm.settings_preview_codecs & (1u << i)) ++codecs;
    const auto count = std::to_wstring(PreviewFormatCount());
    std::wstring result = l10n::IsChinese()
        ? l10n::Cn(L"共 ") + count + l10n::Cn(L" 种格式 · 增强包 ") + std::to_wstring(packs) + L"/4"
        : count + L" formats · Preview packs " + std::to_wstring(packs) + L"/4";
    if (vm.settings_preview_codecs & kPreviewCodecsDetected)
        result += std::wstring(l10n::Get(l10n::StringId::SystemExtensions_2).c_str()) + std::to_wstring(codecs) + L"/4";
    else result += l10n::Get(l10n::StringId::CheckingSystemExtensions).c_str();
    return result;
}
inline const wchar_t* FormatsTitle() { return l10n::Get(l10n::StringId::SupportedFormats).c_str(); }
inline const wchar_t* SystemTitle() { return l10n::Get(l10n::StringId::SystemExtensions).c_str(); }
inline const wchar_t* Legend(int source) {
    if (source == 0) return l10n::Get(l10n::StringId::BuiltIn).c_str();
    if (source == 1) return l10n::Get(l10n::StringId::PreviewPack).c_str();
    return l10n::Get(l10n::StringId::PackOrSystemExtension).c_str();
}
inline const wchar_t* LegendNote() { return l10n::Get(l10n::StringId::DashedEnhancedSupportIsNotEnabled).c_str(); }
inline const wchar_t* SystemHint(const WindowViewModel& vm) { return vm.settings_pack_images_installed && vm.settings_pack_images_enabled
    ? l10n::Get(l10n::StringId::TheImagePackIsEnabledItDecodesHEICAVIFWithoutTheCorrespondin).c_str()
    : l10n::Get(l10n::StringId::TheImagePackAlsoSupportsHEICAVIFWithoutAMicrosoftAccount).c_str(); }

inline const wchar_t* OpenFolder() { return l10n::Get(l10n::StringId::OpenFolder).c_str(); }
inline const wchar_t* Media() { return l10n::Get(l10n::StringId::Media).c_str(); }
inline const wchar_t* Advanced() { return l10n::Get(l10n::StringId::Advanced_2).c_str(); }
inline const wchar_t* MediaTitle() { return l10n::Get(l10n::StringId::FFmpegMedia).c_str(); }
inline const wchar_t* MediaDesc() { return l10n::Get(l10n::StringId::ThumbnailsPlayingTimeAndMediaDetailsForVideosWindowsHasNoDec).c_str(); }
inline const wchar_t* MediaMeta() { return l10n::Get(l10n::StringId::LGPL21DecodeOnlyBuildNoGPLParts).c_str(); }
inline const wchar_t* Images() { return l10n::Get(l10n::StringId::Images_2).c_str(); }
inline const wchar_t* ImagesTitle() { return l10n::Get(l10n::StringId::ModernImageFormats).c_str(); }
inline const wchar_t* ImagesDesc() { return l10n::Get(l10n::StringId::PreviewIPhonePhotosHEICAVIFJPEGXLAndHDREXRPicturesWithoutThe).c_str(); }
inline const wchar_t* ImagesMeta() { return l10n::Get(l10n::StringId::LibheifDav1dLibjxlOpenEXRLGPL30BSD).c_str(); }
inline const wchar_t* RawTitle() { return l10n::Get(l10n::StringId::RAWCameraPhotos).c_str(); }
inline const wchar_t* RawDesc() { return l10n::Get(l10n::StringId::UseLibRawForPreviewsAndThumbnailsOfCameraOriginalsSuchAsCR2C).c_str(); }
inline const wchar_t* RawMeta() { return L"LibRaw · LGPL-2.1 / CDDL"; }
inline const wchar_t* Archive() { return l10n::Get(l10n::StringId::ArcFamArchive).c_str(); }
inline const wchar_t* ArchiveTitle() { return l10n::Get(l10n::StringId::S7ZipArchives).c_str(); }
inline const wchar_t* ArchiveDesc() { return l10n::Get(l10n::StringId::Use7ZipToPreviewArchiveContentsIncluding7zRARISOCABAndWIM).c_str(); }
inline const wchar_t* ArchiveMeta() { return L"7-Zip · LGPL-2.1 + unRAR"; }
inline const wchar_t* Install() { return l10n::Get(l10n::StringId::DownloadAndInstall).c_str(); }
inline const wchar_t* Remove() { return l10n::Get(l10n::StringId::Remove_2).c_str(); }
inline const wchar_t* CustomTitle() { return l10n::Get(l10n::StringId::UseExistingFFmpeg).c_str(); }
inline const wchar_t* CustomDesc() { return l10n::Get(l10n::StringId::OptionalSourceTurnOffToUseThePulseManagedPack).c_str(); }
inline const wchar_t* NoCustom() { return l10n::Get(l10n::StringId::NoFfmpegExeChosen).c_str(); }
inline const wchar_t* Browse() { return l10n::Get(l10n::StringId::Browse).c_str(); }
inline const wchar_t* UseDetected() { return l10n::Get(l10n::StringId::UseFound).c_str(); }
inline const wchar_t* RemoveTitle() { return l10n::Get(l10n::StringId::RemovePacksWhenUninstallingPulse).c_str(); }
inline const wchar_t* RemoveDesc() { return l10n::Get(l10n::StringId::OtherwiseTheyStayAndWorkAgainAfterReinstallingPulse).c_str(); }
inline const wchar_t* AutoUpdateTitle() { return l10n::Get(l10n::StringId::UpdatePreviewPacksAutomatically).c_str(); }
inline const wchar_t* AutoUpdateDesc() { return l10n::Get(l10n::StringId::RunsWithThePulseUpdateCheckAndDownloadsInTheBackground).c_str(); }
inline const wchar_t* Note() { return l10n::Get(l10n::StringId::PacksRunInTheIsolatedPreviewProcessACrashOrTimeoutOnlyAffect).c_str(); }
inline std::wstring Bytes(uint64_t bytes) {
    wchar_t text[48]{};
    if (bytes >= 1024ull * 1024 * 1024) swprintf_s(text, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) swprintf_s(text, L"%.0f MB", bytes / (1024.0 * 1024));
    else swprintf_s(text, L"%.0f KB", bytes / 1024.0);
    return text;
}
inline std::wstring Summary(const WindowViewModel& vm) {
    if (!vm.settings_pack_installed) return l10n::Get(l10n::StringId::NoPreviewPacksInstalled).c_str();
    wchar_t text[128]{};
    swprintf_s(text, l10n::Get(l10n::StringId::UInstalledS).c_str(),
               vm.settings_pack_installed, Bytes(vm.settings_pack_bytes).c_str());
    return text;
}
inline std::wstring Found(const std::wstring& path) {
    return std::wstring(l10n::Get(l10n::StringId::Found).c_str()) + path;
}
struct Badge { std::wstring text; fluent::BadgeKind kind = fluent::BadgeKind::Neutral; };
inline Badge ContentBadge(const WindowViewModel& vm) {
    switch (vm.settings_content_pack_state) {
    case 0: return {l10n::Get(l10n::StringId::ContentPackBundled).c_str(), fluent::BadgeKind::Success};
    case 1: return {l10n::Get(l10n::StringId::Installed).c_str(), fluent::BadgeKind::Success};
    case 3: case 4: return {l10n::Get(l10n::StringId::ContentPackRepair).c_str(), fluent::BadgeKind::Warning};
    default: return {l10n::Get(l10n::StringId::NotInstalled).c_str(), fluent::BadgeKind::Neutral};
    }
}
inline Badge OcrBadge() { return {l10n::Get(l10n::StringId::PackPlanned).c_str(), fluent::BadgeKind::Neutral}; }
inline const wchar_t* PrimaryLabel(bool available, bool installed, bool installing) {
    if (installing) return l10n::Get(l10n::StringId::Cancel).c_str();
    if (installed) return Remove();
    return available ? Install() : l10n::Get(l10n::StringId::NotPublished).c_str();
}
inline bool PrimaryEnabled(bool available, bool installed, bool installing) {
    return available || installed || installing;
}
// "更新到 1.0.2" beside 卸载 while an installed pack is older than the
// catalog; empty (no button) while it downloads or when it is current.
inline std::wstring UpdateLabel(bool installed, bool installing, const std::wstring& version) {
    if (!installed || installing || version.empty()) return {};
    return std::wstring(l10n::Get(l10n::StringId::UpdateTo).c_str()) + version;
}
inline Badge PackBadge(bool available, bool installed, bool installing, bool enabled, const std::wstring& version) {
    if (installing) return {installed ? l10n::Get(l10n::StringId::Updating).c_str() : l10n::Get(l10n::StringId::Installing).c_str(),
                            fluent::BadgeKind::Accent};
    if (!installed) return {l10n::Pick(available ? L"未安装" : L"尚未发布", available ? L"Not installed" : L"Not published"), fluent::BadgeKind::Neutral};
    if (!enabled) return {l10n::Get(l10n::StringId::Off).c_str(), fluent::BadgeKind::Neutral};
    std::wstring text = l10n::Get(l10n::StringId::Installed).c_str();
    if (!version.empty()) text += L" " + version;
    return {text, fluent::BadgeKind::Success};
}
inline Badge MediaBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_media_available, vm.settings_pack_media_installed,
        vm.settings_pack_installing, true, vm.settings_pack_version);
}
inline const wchar_t* Primary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_media_available, vm.settings_pack_media_installed, vm.settings_pack_installing);
}
inline std::wstring MediaUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_media_installed, vm.settings_pack_installing, vm.settings_pack_update_version);
}
inline const wchar_t* SourceTitle() { return l10n::Get(l10n::StringId::EnableMediaEnhancement).c_str(); }
inline const wchar_t* SourceStatus(const WindowViewModel& vm) {
    if (!vm.settings_pack_use_custom && !vm.settings_pack_media_installed)
        return vm.settings_pack_media_available
            ? l10n::Get(l10n::StringId::NoMediaSourceAvailableDownloadAPackOrChooseExistingFFmpeg).c_str()
            : l10n::Get(l10n::StringId::NoMediaSourceAvailableChooseExistingFFmpeg).c_str();
    if (vm.settings_pack_use_custom && vm.settings_pack_ffmpeg != 2)
        return l10n::Get(l10n::StringId::SourceExistingFFmpegInvalidPathChooseAgain).c_str();
    if (!vm.settings_pack_ffmpeg_enabled)
        return vm.settings_pack_use_custom
            ? l10n::Get(l10n::StringId::OffSelectedSourceExistingFFmpeg).c_str()
            : l10n::Get(l10n::StringId::OffSelectedSourcePulseManagedPack).c_str();
    if (vm.settings_pack_use_custom)
        return l10n::Get(l10n::StringId::SourceExistingFFmpegUserFileNotVerified).c_str();
    return l10n::Get(l10n::StringId::SourcePulseManagedPack).c_str();
}
// "正在下载… 42%" while a pack downloads.
inline std::wstring Downloading(float progress) {
    const int percent = static_cast<int>(progress * 100.0f + 0.5f);
    return std::wstring(l10n::Get(l10n::StringId::Downloading).c_str()) +
           std::to_wstring(percent < 0 ? 0 : percent > 100 ? 100 : percent) + L"%";
}
inline std::wstring Downloading(const WindowViewModel& vm) { return Downloading(vm.settings_pack_progress); }
inline Badge ImagesBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_images_available, vm.settings_pack_images_installed,
        vm.settings_pack_images_installing, vm.settings_pack_images_enabled, vm.settings_pack_images_version);
}
inline const wchar_t* ImagesPrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_images_available, vm.settings_pack_images_installed, vm.settings_pack_images_installing);
}
inline bool ShowsImagesEnable(const WindowViewModel& vm) { return vm.settings_pack_images_installed; }
inline std::wstring ImagesUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_images_installed, vm.settings_pack_images_installing,
                       vm.settings_pack_images_update_version);
}
inline Badge RawBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_raw_available, vm.settings_pack_raw_installed,
        vm.settings_pack_raw_installing, vm.settings_pack_raw_enabled, vm.settings_pack_raw_version);
}
inline const wchar_t* RawPrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_raw_available, vm.settings_pack_raw_installed, vm.settings_pack_raw_installing);
}
inline bool ShowsRawEnable(const WindowViewModel& vm) { return vm.settings_pack_raw_installed; }
inline std::wstring RawUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_raw_installed, vm.settings_pack_raw_installing, vm.settings_pack_raw_update_version);
}
inline Badge ArchiveBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_archive_available, vm.settings_pack_archive_installed,
        vm.settings_pack_archive_installing, vm.settings_pack_archive_enabled, vm.settings_pack_archive_version);
}
inline const wchar_t* ArchivePrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_archive_available, vm.settings_pack_archive_installed, vm.settings_pack_archive_installing);
}
inline bool ShowsArchiveEnable(const WindowViewModel& vm) { return vm.settings_pack_archive_installed; }
inline std::wstring ArchiveUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_archive_installed, vm.settings_pack_archive_installing,
                       vm.settings_pack_archive_update_version);
}
// Switch and path rows exist once there is something to switch.
inline bool ShowsEnable(const WindowViewModel& vm) { return vm.settings_pack_ffmpeg != 0 || vm.settings_pack_use_custom; }
inline bool ShowsPath(const WindowViewModel&) {
    return true;
}
inline bool ShowsDetect(const WindowViewModel& vm) {
    return !vm.settings_pack_detected_path.empty() &&
        _wcsicmp(vm.settings_pack_detected_path.c_str(), vm.settings_pack_custom_path.c_str()) != 0;
}
} // namespace pack_text

float LayoutSettingsPacks(SettingsLayout& l, const WindowViewModel& vm, float scale,
                          float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20*scale, right = l.content.right - 20*scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h*scale); y = r.bottom; return r; };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 96*scale; };
    auto caption_h = [&](std::wstring_view text, float width) {
        return text.empty() ? 0.0f : painter
            ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40*scale, width)) : 40*scale;
    };
    const float font = static_cast<float>(vm.settings_ui_font_scale) / 100.0f;
    const float open_w = button_w(pack_text::OpenFolder());
    // 快速预览 (page 5): how previews open and which formats they cover, then a
    // card leading to the 增强包 page (8), which holds the packs themselves.
    if (vm.settings_page != 8) {
    y += caption_h(pack_text::Intro(), right - left) + 10*scale;
    // 预览方式: one card per choice row, segments at the right (below when narrow).
    {
        const bool narrow = right - left < 560*scale;
        auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
            const auto c = D2D1::RectF(narrow ? r.left + 16*scale : r.right - (width + 16)*scale,
                r.bottom - 44*scale, r.right - 16*scale, r.bottom - 12*scale);
            const float w = (c.right - c.left) / static_cast<float>(count);
            for (int i = 0; i < count; ++i)
                output[i] = D2D1::RectF(c.left + w*static_cast<float>(i), c.top, c.left + w*static_cast<float>(i + 1), c.bottom);
        };
        const float h = narrow ? 112.0f : 64.0f + 8.0f*(font - 1.0f)*4.0f;
        l.trigger_section = row(28*font);
        l.trigger_row = row(h);
        segments(l.trigger_row, l.trigger_choice, 2, 220*font);
        if (vm.settings_preview_trigger == 0) {
            y += 8*scale;
            l.hover_delay_row = row(h);
            segments(l.hover_delay_row, l.hover_delay_choice, 3, 240*font);
            y += 8*scale;
            l.hover_size_row = row(h);
            segments(l.hover_size_row, l.hover_size_choice, 3, 240*font);
        }
        y += 24*scale;
    }
    l.preview_section = row(28*font);
    const float header_top = y;
    l.preview_header_title = D2D1::RectF(left + 54*scale, y + 12*scale, right - 48*scale, y + (12 + 26*font)*scale);
    l.preview_header_summary = D2D1::RectF(left + 54*scale, l.preview_header_title.bottom, right - 48*scale,
        l.preview_header_title.bottom + caption_h(pack_text::FormatsSummary(vm), right - left - 102*scale));
    y = l.preview_header_summary.bottom + 12*scale;
    l.disclosure[2] = D2D1::RectF(left, header_top, right, y);
    if (vm.settings_expanded & 4u) {
        l.preview_legend = D2D1::RectF(left + 54*scale, y + 4*scale, right - 16*scale, y + 4*scale);
        float legend_x = l.preview_legend.left, legend_y = l.preview_legend.top;
        for (int i = 0; i < 3; ++i) {
            const float width = painter ? painter->MeasureBadgeWidth(pack_text::Legend(i)) : (i == 2 ? 180 : 100)*scale;
            if (legend_x > l.preview_legend.left && legend_x + width > l.preview_legend.right) {
                legend_x = l.preview_legend.left; legend_y += 30*font*scale;
            }
            l.preview_legend_chip[i] = D2D1::RectF(legend_x, legend_y, (std::min)(legend_x + width, l.preview_legend.right), legend_y + 24*font*scale);
            legend_x += width + 10*scale;
        }
        const bool inline_note = l.preview_legend.right - legend_x > 250*font*scale;
        const float note_left = inline_note ? legend_x + 6*scale : l.preview_legend.left;
        const float note_top = inline_note ? legend_y + 2*scale : legend_y + 32*font*scale;
        l.preview_legend_note = D2D1::RectF(note_left, note_top, right - 16*scale,
            note_top + caption_h(pack_text::LegendNote(), right - 16*scale - note_left));
        y = (std::max)(legend_y + 24*font*scale, l.preview_legend_note.bottom) + 14*scale;
        l.preview_legend.bottom = y;
        l.preview_formats = D2D1::RectF(left, y, right, y);
        y += LayoutPreviewFormats(l.preview_formats, scale, nullptr, nullptr, caption_h);
        l.preview_formats.bottom = y;
    }
    l.preview_group = D2D1::RectF(left, header_top, right, y);
    y += 24*scale; l.preview_codec_section = row(28*font);
    const float codecs_top = y;
    const bool stacked_codecs = right - left < 620*font*scale;
    for (int i = 0; i < kPreviewCodecCount; ++i) {
        const auto info = PreviewCodec(i);
        const bool detected = (vm.settings_preview_codecs & kPreviewCodecsDetected) != 0;
        const bool has = (vm.settings_preview_codecs & (1u << i)) != 0;
        const bool button = detected && !has && info.store_id;
        const auto status = !detected ? l10n::Get(l10n::StringId::Checking).c_str() : has
            ? l10n::Get(l10n::StringId::Installed).c_str() : l10n::Get(l10n::StringId::NotInstalled).c_str();
        const float badge_w = painter ? painter->MeasureBadgeWidth(status) : 110*scale;
        const float bw = button ? button_w(l10n::Get(l10n::StringId::Get).c_str()) : 0;
        const float text_right = stacked_codecs ? right - 16*scale : right - 28*scale - badge_w - (button ? bw + 10*scale : 0);
        const float desc_h = caption_h(info.description, text_right - left - 54*scale);
        const float text_h = 14*scale + 26*font*scale + desc_h;
        const float row_h = (std::max)(64*scale, text_h + (stacked_codecs ? 46 : 12)*scale);
        l.preview_codec_row[i] = row(row_h / scale);
        const auto r = l.preview_codec_row[i];
        l.preview_codec_text[i] = D2D1::RectF(left + 54*scale, r.top + 10*scale, text_right, r.top + text_h);
        const float controls_y = stacked_codecs ? r.bottom - 42*scale : (r.top + r.bottom - 32*scale)/2;
        if (button) l.preview_codec_button[i] = D2D1::RectF(right - 16*scale - bw, controls_y, right - 16*scale, controls_y + 32*scale);
        const float badge_right = button ? l.preview_codec_button[i].left - 10*scale : right - 16*scale;
        l.preview_codec_badge[i] = D2D1::RectF(badge_right - badge_w, controls_y + (32 - 24*font)*scale/2,
                                            badge_right, controls_y + (32 + 24*font)*scale/2);
    }
    l.preview_codec_hint = D2D1::RectF(left + 16*scale, y + 12*scale, right - 16*scale,
        y + 12*scale + caption_h(pack_text::SystemHint(vm), right - left - 32*scale));
    y = l.preview_codec_hint.bottom + 14*scale;
    l.preview_codec_group = D2D1::RectF(left, codecs_top, right, y);
    y += 30*scale; l.pack_section = row(32*font);
    {
        const bool compact = right - left < 560*font*scale;
        l.pack_summary = row(compact ? 116.0f : 76.0f);
        const float manage_w = button_w(pack_text::ManagePacks());
        l.pack_manage = D2D1::RectF(right - 16*scale - manage_w, l.pack_summary.bottom - 48*scale,
                                    right - 16*scale, l.pack_summary.bottom - 16*scale);
    }
    return y + 24*scale;
    }
    y += caption_h(pack_text::PacksIntro(), right - left) + 18*scale;
    // Summary: how many packs, their disk use and where they live; 全部更新
    // left of 打开文件夹 while some pack has a newer release.
    const bool compact_pack = right - left < 560*font*scale;
    l.pack_summary = row(compact_pack ? 116.0f : 76.0f);
    l.pack_open = D2D1::RectF(right - 16*scale - open_w, l.pack_summary.bottom - 48*scale,
                              right - 16*scale, l.pack_summary.bottom - 16*scale);
    if (vm.settings_pack_updates > 0) {
        const float all_w = button_w(pack_text::UpdateAll(vm.settings_pack_updates));
        l.pack_update_all = D2D1::RectF(l.pack_open.left - 8*scale - all_w, l.pack_open.top,
                                        l.pack_open.left - 8*scale, l.pack_open.bottom);
    }

    // One pack card: icon, title + badge (+ switch), description, meta line
    // with the primary button (and 更新到 left of it), and the last action's
    // message.
    const float text_left = left + 54*scale, inner_right = right - 16*scale;
    struct CardRects { D2D1_RECT_F *card, *badge, *enable, *primary, *update, *notice; float* desc_h; };
    auto card_layout = [&](const CardRects& out, bool shows_enable, const pack_text::Badge& badge,
                           std::wstring_view desc, std::wstring_view primary, std::wstring_view update,
                           const std::wstring& notice) {
        const float top = y;
        float badge_right = inner_right;
        const float badge_top = top + (compact_pack ? 44*font : 19)*scale;
        if (shows_enable) {
            *out.enable = D2D1::RectF(inner_right - 42*scale, badge_top - 5*scale, inner_right, badge_top + 27*scale);
            badge_right = out.enable->left - 12*scale;
        }
        const float badge_w = painter ? painter->MeasureBadgeWidth(badge.text) : 96*scale;
        *out.badge = D2D1::RectF(badge_right - badge_w, badge_top, badge_right, badge_top + 24*font*scale);
        *out.desc_h = caption_h(desc, inner_right - text_left);
        const float desc_top = compact_pack ? out.badge->bottom + 12*scale : top + 40*scale;
        const float footer = desc_top + *out.desc_h + 12*scale;
        const float primary_w = button_w(primary);
        *out.primary = D2D1::RectF(inner_right - primary_w, footer, inner_right, footer + 32*scale);
        if (!update.empty()) {
            const float update_w = button_w(update);
            *out.update = D2D1::RectF(out.primary->left - 8*scale - update_w, footer,
                                      out.primary->left - 8*scale, footer + 32*scale);
        }
        float bottom = footer + 32*scale + 14*scale;
        if (!notice.empty()) {
            const float notice_h = caption_h(notice, inner_right - text_left - 12*scale);
            *out.notice = D2D1::RectF(text_left - 8*scale, bottom - 4*scale, inner_right, bottom + notice_h + 12*scale);
            bottom = out.notice->bottom + 12*scale;
        }
        *out.card = D2D1::RectF(left, top, right, bottom);
        y = bottom;
    };

    // Media: the FFmpeg pack card.
    y += 24*scale; l.pack_media_section = row(28);
    card_layout({&l.pack_card, &l.pack_badge, &l.pack_enable, &l.pack_primary, &l.pack_update, &l.pack_notice,
                 &l.pack_desc_h},
                false, pack_text::MediaBadge(vm), pack_text::MediaDesc(),
                pack_text::Primary(vm), pack_text::MediaUpdate(vm), vm.settings_pack_notice);

    // Optional existing FFmpeg is adjacent to the independently installable media pack.
    y += 12*scale;
    const float source_top = y;
    const bool source_switch = pack_text::ShowsEnable(vm);
    const float source_text_width = right - left - (source_switch ? 88 : 32)*scale;
    l.pack_source_status = row((caption_h(pack_text::SourceStatus(vm), source_text_width) + 47*scale) / scale);
    if (source_switch) {
        const float cy = (l.pack_source_status.top + l.pack_source_status.bottom) * 0.5f;
        l.pack_enable = D2D1::RectF(inner_right - 42*scale, cy - 16*scale, inner_right, cy + 16*scale);
    }
    l.pack_custom_row = row((caption_h(pack_text::CustomDesc(), right - left - 126*scale) + 47*scale) / scale);
    if (pack_text::ShowsPath(vm)) {
        l.pack_path_row = row(pack_text::ShowsDetect(vm) ? 110.0f : 90.0f);
        const float browse_w = button_w(pack_text::Browse());
        const float button_top = l.pack_path_row.bottom - 44*scale;
        l.pack_browse = D2D1::RectF(inner_right - browse_w, button_top, inner_right, button_top + 32*scale);
        if (pack_text::ShowsDetect(vm)) {
            const float detect_w = button_w(pack_text::UseDetected());
            l.pack_detect = D2D1::RectF(l.pack_browse.left - 8*scale - detect_w, button_top,
                                        l.pack_browse.left - 8*scale, button_top + 32*scale);
        }
    }
    l.pack_source_group = D2D1::RectF(left, source_top, right, y);

    // Images: the image pack card.
    y += 24*scale; l.pack_images_section = row(28);
    card_layout({&l.pack_images_card, &l.pack_images_badge, &l.pack_images_enable, &l.pack_images_primary,
                 &l.pack_images_update, &l.pack_images_notice, &l.pack_images_desc_h},
                pack_text::ShowsImagesEnable(vm), pack_text::ImagesBadge(vm), pack_text::ImagesDesc(),
                pack_text::ImagesPrimary(vm), pack_text::ImagesUpdate(vm), vm.settings_pack_images_notice);

    // RAW camera photos card.
    y += 12*scale;
    card_layout({&l.pack_raw_card, &l.pack_raw_badge, &l.pack_raw_enable, &l.pack_raw_primary,
                 &l.pack_raw_update, &l.pack_raw_notice, &l.pack_raw_desc_h},
                pack_text::ShowsRawEnable(vm), pack_text::RawBadge(vm), pack_text::RawDesc(),
                pack_text::RawPrimary(vm), pack_text::RawUpdate(vm), vm.settings_pack_raw_notice);

    // 7-Zip archive contents card.
    y += 24*scale; l.pack_archive_section = row(28);
    card_layout({&l.pack_archive_card, &l.pack_archive_badge, &l.pack_archive_enable, &l.pack_archive_primary,
                 &l.pack_archive_update, &l.pack_archive_notice, &l.pack_archive_desc_h},
                pack_text::ShowsArchiveEnable(vm), pack_text::ArchiveBadge(vm), pack_text::ArchiveDesc(),
                pack_text::ArchivePrimary(vm), pack_text::ArchiveUpdate(vm), vm.settings_pack_archive_notice);

    // 搜索: the 内容搜索 module (its index status as the notice) and OCR, planned.
    y += 24*scale; l.pack_search_section = row(28);
    card_layout({&l.pack_content_card, &l.pack_content_badge, &l.pack_content_enable, &l.pack_content_primary,
                 &l.pack_content_update, &l.pack_content_notice, &l.pack_content_desc_h},
                false, pack_text::ContentBadge(vm), pack_text::ContentDesc(), pack_text::ContentPrimary(vm), {},
                pack_text::ContentReady(vm) ? vm.settings_content_status : std::wstring{});
    y += 12*scale;
    card_layout({&l.pack_ocr_card, &l.pack_ocr_badge, &l.pack_ocr_enable, &l.pack_ocr_primary,
                 &l.pack_ocr_update, &l.pack_ocr_notice, &l.pack_ocr_desc_h},
                false, pack_text::OcrBadge(), pack_text::OcrDesc(), pack_text::ComingSoon(), {}, std::wstring{});

    // Background updates and the uninstall choice remain separate from the media source.
    y += 24*scale; l.pack_advanced_section = row(28);
    const float group_top = y;
    l.pack_auto_update_row = row((caption_h(pack_text::AutoUpdateDesc(), right - left - 126*scale) + 47*scale) / scale);
    l.pack_remove_row = row((caption_h(pack_text::RemoveDesc(), right - left - 126*scale) + 47*scale) / scale);
    l.pack_group = D2D1::RectF(left, group_top, right, y);
    y += 12*scale;
    l.pack_note = D2D1::RectF(left, y, right, y + caption_h(pack_text::Note(), right - left - 8*scale) + 4*scale);
    y = l.pack_note.bottom + 24*scale;
    return y;
}
