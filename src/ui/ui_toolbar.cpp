#include "ui_renderer_internal.h"
#include "toolbar_layout.h"
#include "pane_header_icons.h"

namespace pulse::ui {
namespace {
std::wstring SelectionChipLabel(int count) {
    wchar_t buf[64]{};
    swprintf_s(buf,l10n::Get(l10n::StringId::SelectedCountFormat).c_str(),count);
    return buf;
}
}
float MainRenderer::SelectionChipLabelWidth(float width, int selected) const {
    // Narrow windows show the buttons only, as in the prototype.
    if (selected <= 0 || width-EffectiveSidebarWidth(width) < 700*scale_ || !compositor_ || !compositor_->SmallFormat())
        return 0.0f;
    return std::ceil(typography::MeasureLine(compositor_,compositor_->SmallFormat(),SelectionChipLabel(selected)));
}
// Pinned apps (prototype app_pins.html): program icons after the pinned
// commands. The dot under an icon says it opens every selected (or dragged)
// item, a ring some of them; an app that opens none is dimmed. In drop mode
// they sit on a faint accent band behind a label, and the app under the
// drag gets an accent outline.
void MainRenderer::DrawToolbarApps(const WindowViewModel& vm, const ToolbarLayout& layout, const Theme& theme) {
    ID2D1DeviceContext* dc=compositor_->Dc();
    const size_t from=vm.toolbar_pins.size();
    D2D1_RECT_F band=layout.pin_label;
    bool any=band.right > band.left;
    for (size_t i=0;i<vm.toolbar_apps.size() && from+i<layout.pins.size();++i) {
        const D2D1_RECT_F b=layout.pins[from+i];
        if (b.right <= b.left) continue;
        if (!any) band=b;
        band.left=std::min(band.left,b.left);
        band.right=std::max(band.right,b.right);
        any=true;
    }
    if (!any) return;
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    if (vm.toolbar_drop_mode) {
        D2D1_COLOR_F fill=theme.accent;
        fill.a*=0.07f;
        painter_.FillRoundedRect(D2D1::RectF(band.left-3*scale_,band.top-2*scale_,band.right+3*scale_,band.bottom+2*scale_),
            8*scale_,fill);
    }
    FillRect(dc,brStrokeDivider_.get(),band.left-7*scale_,band.top+7*scale_,scale_,18*scale_);
    if (layout.pin_label.right > layout.pin_label.left && !vm.toolbar_drop_label.empty()) {
        const D2D1_RECT_F l=layout.pin_label;
        if (vm.toolbar_drop_pin) {
            D2D1_COLOR_F hot=theme.accent;
            hot.a*=0.18f;
            painter_.FillRoundedRect(l,7*scale_,hot);
        }
        DrawIconText(l.left+8*scale_,l.top,20*scale_,l.bottom-l.top,vm.toolbar_drop_pin ? L"\xE718" : L"\xE8E5",
            L"",theme.accent,0.85f);
        painter_.DrawText(vm.toolbar_drop_label,D2D1::RectF(l.left+32*scale_,l.top,l.right-6*scale_,l.bottom),
            compositor_->SmallFormat(),theme.accent);
    }
    ComPtr<ID2D1SolidColorBrush> accent;
    dc->CreateSolidColorBrush(theme.accent,&accent);
    for (size_t i=0;i<vm.toolbar_apps.size() && from+i<layout.pins.size();++i) {
        const D2D1_RECT_F b=layout.pins[from+i];
        if (b.right <= b.left) continue;
        const WindowViewModel::ToolbarAppPin& app=vm.toolbar_apps[i];
        if (i==0 && from > 0) {   // short divider between the commands and the apps
            const float x=b.left-6.5f*scale_;
            FillRect(dc,brStrokeDivider_.get(),x,b.top+9*scale_,scale_,14*scale_);
        }
        const bool target=vm.toolbar_drop_app==static_cast<int>(i);
        if (target) {
            D2D1_COLOR_F hot=theme.accent;
            hot.a*=0.16f;
            painter_.FillRoundedRect(b,6*scale_,hot);
            if (accent.get()) {
                const float h=scale_;
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left+h,b.top+h,b.right-h,b.bottom-h),
                    6*scale_,6*scale_),accent.get(),2*scale_);
            }
        } else if (IsHovered(vm,HitTestResult::ToolbarPin,static_cast<int>(from+i))) {
            painter_.FillRoundedRect(b,6*scale_,theme.fill_hover);
        }
        const float cx=std::round((b.left+b.right)*0.5f);
        const float cy=std::round((b.top+b.bottom)*0.5f-(app.fit!=0 ? 1.5f*scale_ : 0.0f));
        const float half=8*scale_;
        const float opacity=app.fit==3 && !(target && vm.toolbar_drop_force) ? 0.35f : 1.0f;
        if (ID2D1Bitmap* bitmap=AppIconBitmap(app.icon)) {
            dc->DrawBitmap(bitmap,D2D1::RectF(cx-half,cy-half,cx+half,cy+half),opacity,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            D2D1_COLOR_F c=theme.text_secondary;
            c.a*=opacity;
            DrawIconText(b.left,cy-15*scale_,b.right-b.left,30*scale_,L"\xECAA",L"",c,0.9f);
        }
        if (accent.get() && (app.fit==1 || app.fit==2)) {
            const D2D1_POINT_2F dot=D2D1::Point2F(cx,b.bottom-4.5f*scale_);
            if (app.fit==1) dc->FillEllipse(D2D1::Ellipse(dot,2*scale_,2*scale_),accent.get());
            else dc->DrawEllipse(D2D1::Ellipse(dot,1.75f*scale_,1.75f*scale_),accent.get(),scale_);
        }
    }
}

ToolbarSelectionChip MainRenderer::SelectionChipAt(const ToolbarLayout& layout, float width, int selected,
                                                   int action_count) const {
    if (selected <= 0 || embedded_) return {};
    return MakeToolbarSelectionChip(layout,scale_,SelectionChipLabelWidth(width,selected),action_count);
}
void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    auto* dc = compositor_->Dc();
    const float left = EffectiveSidebarWidth(rect.right);
    const bool compact = rect.right-left < 600*scale_;
    const auto layout = ToolbarLayoutAt(rect.right,NewButtonWidthPx(compact),vm.pane.filter_expand);
    const bool searchOverlay=vm.address_searching && rect.right-left<480*scale_;
    if (!searchOverlay) {
        const wchar_t* nav_glyphs[] = {kIconBack,kIconForward,kIconUp,kIconRefresh};
        const HitTestResult::Region nav_hits[] = {HitTestResult::NavBack,HitTestResult::NavForward,HitTestResult::NavUp,HitTestResult::NavRefresh};
        for (int i=0;i<4;++i) {
            const auto r=layout.navigation[i];
            if (r.right<=r.left) continue;
            const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
            fluent::ButtonSpec button;
            button.bounds = r; button.glyph = nav_glyphs[i];
            button.state.enabled = enabled;
            button.state.hovered = enabled && IsHovered(vm, nav_hits[i]);
            painter_.DrawCommandButton(button);
        }
        // Breadcrumb address bar: segments clickable, empty area -> edit mode.
        D2D1_RECT_F addrRc = AddressBarRect(rect.right);
        fluent::ControlState addrState{};
        addrState.focused = vm.address_editing && !vm.address_searching;
        addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
        painter_.DrawTextFieldFrame(addrRc, addrState);
        dc->PushAxisAlignedClip(addrRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (!vm.address_editing || vm.address_searching) {
            std::vector<BreadcrumbPlaced> placed;
            BreadcrumbLayout(vm.pane, rect.right, placed);
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& seg = placed[i];
                if ((int)i == vm.breadcrumb_drop) {
                    // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                    dc->DrawRoundedRectangle(
                        D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                        brAccent_.get(), 2.0f * scale_);
                } else if ((int)i == vm.breadcrumb_hover) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                        theme.radius_control * scale_);
                }
                if (i > 0) {
                    // Chevron separator.
                    float chX = seg.rc.left - 14.0f * scale_;
                    DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                        kIconChevronRight, L">", theme.text_secondary, 0.55f);
                }
                const bool search_crumb = vm.pane.is_query_search && vm.pane.has_search_origin &&
                    i + 1 == placed.size();
                if (search_crumb && (int)i != vm.breadcrumb_hover && (int)i != vm.breadcrumb_drop) {
                    auto tint = theme.accent;
                    tint.a *= 0.16f;
                    ComPtr<ID2D1SolidColorBrush> tint_brush;
                    dc->CreateSolidColorBrush(tint, &tint_brush);
                    if (tint_brush.get()) FillRoundedRect(dc, tint_brush.get(), seg.rc.left, seg.rc.top,
                        seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top, theme.radius_control * scale_);
                }
                MakeBrush(dc, theme.text, brText_);
                // Width measured exactly; let the ink use the right padding as slack
                // so the trailing glyph is not shaved by the clip rect.
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                    seg.rc.left + 8 * scale_, seg.rc.top, seg.rc.right - seg.rc.left - 8 * scale_,
                    seg.rc.bottom - seg.rc.top);
            }
            if (placed.empty() && !vm.pane.path.empty()) {
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                    addrRc.left + 12.0f * scale_, addrRc.top,
                    addrRc.right - addrRc.left - 18.0f * scale_,
                    addrRc.bottom - addrRc.top);
            }
        }
        dc->PopAxisAlignedClip();
    }
    DrawAddressSearchChrome(vm, rect.right, theme);
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    FillRect(dc,brStrokeDivider_.get(),left+margin_,title_bar_height_+44*scale_,rect.right-left-2*margin_,scale_);
    // Embedded hosts (the file picker) keep the address row only.
    if (embedded_) return;
    // Split New button: the main part creates a folder (Ctrl+Shift+N), the
    // chevron segment opens the New menu. Each segment gets its own hover.
    const std::wstring label=NewButtonLabel();
    const D2D1_RECT_F newArrow=NewArrowSegment(layout.create,compact);
    fluent::ButtonSpec create;
    create.bounds=layout.create; create.text=compact ? std::wstring_view{} : std::wstring_view{label};
    create.glyph=NewButtonGlyph(); create.kind=fluent::ButtonKind::Primary;
    create.drop_down=true;
    // This PC, a server's shares, Pulse views: nothing can be created there.
    create.state.enabled=vm.pane.can_create;
    painter_.DrawButton(create);
    const bool newMainHot=vm.pane.can_create && IsHovered(vm,HitTestResult::NewButton);
    const bool newArrowHot=IsHovered(vm,HitTestResult::NewButtonArrow);
    if (newMainHot || newArrowHot) {
        D2D1_RECT_F hot=layout.create;
        if (newArrowHot) hot.left=newArrow.left; else hot.right=newArrow.left;
        dc->PushAxisAlignedClip(hot,D2D1_ANTIALIAS_MODE_ALIASED);
        create.state.hovered=true;
        painter_.DrawButton(create);
        dc->PopAxisAlignedClip();
    }
    if (newArrow.right > newArrow.left && newArrow.left > layout.create.left) {
        auto divider=theme.accent_text;
        divider.a*=0.35f;
        ComPtr<ID2D1SolidColorBrush> divider_brush;
        dc->CreateSolidColorBrush(divider,&divider_brush);
        if (divider_brush.get()) FillRect(dc,divider_brush.get(),newArrow.left-0.5f*scale_,newArrow.top+7*scale_,
            scale_,newArrow.bottom-newArrow.top-14*scale_);
    }
    const wchar_t* glyphs[]={kIconCut,kIconCopy,kIconPaste,kIconRename,kIconDelete,kIconSplit,
        vm.details_visible ? kIconDetailsClose : kIconDetailsOpen,L""};
    const HitTestResult::Region hits[]={HitTestResult::Cut,HitTestResult::Copy,HitTestResult::Paste,
        HitTestResult::Rename,HitTestResult::Delete,HitTestResult::SplitButton,HitTestResult::DetailsToggle,HitTestResult::PaneColumnLayout};
    for(int i=0;i<8;++i) {
        if (layout.commands[i].right <= layout.commands[i].left) continue;
        // Cut / copy / rename / delete follow what is selected (a drive takes
        // a rename only; app::SelectionCaps); paste needs something to paste
        // (clipboard files or a tray batch) and a folder that takes it.
        const bool can_paste=vm.paste_target && (vm.clipboard_files || vm.tray_deck.batch_count>0);
        const bool allowed[]={vm.selection_cut,vm.selection_copy,can_paste,vm.selection_rename,vm.selection_delete};
        const bool enabled=i>=5 || allowed[i];
        const bool selected=(i==5 && vm.pane_slots.size()>1) || (i==6 && vm.details_visible) ||
            (i==7 && vm.pane.column_strip.enabled);
        DrawButton(layout.commands[i],theme,enabled && IsHovered(vm,hits[i]) ? theme.fill_hover :
            selected ? theme.fill_selected : kTransparent,
            i==5 ? L"" : glyphs[i],L"",!enabled ? theme.text_disabled : selected ? theme.accent : theme.text_secondary,true,true);
        if (i==5) {
            // The split button shows the current layout (1 / 2 side by side /
            // 2 stacked / 3 / 4). A pane count that does not match the preset
            // (a custom split) is described from the pane rectangles instead.
            constexpr int kPaneCount[]={1,2,2,3,4};
            int preset=std::clamp(vm.layout_preset,0,4);
            const int panes=static_cast<int>(vm.pane_slots.size());
            if (panes>0 && kPaneCount[preset]!=panes) {
                if (panes==1) preset=0;
                else if (panes==2) preset=vm.pane_slots[1].rect.top>=vm.pane_slots[0].rect.bottom-1.0f ? 2 : 1;
                else preset=panes==3 ? 3 : 4;
            }
            constexpr PaneHeaderIcon kIcons[]={PaneHeaderIcon::SplitSingle,PaneHeaderIcon::Split,
                PaneHeaderIcon::SplitStacked,PaneHeaderIcon::SplitThree,PaneHeaderIcon::SplitFour};
            DrawPaneHeaderIcon(layout.commands[i],kIcons[preset],selected ? theme.accent : theme.text_secondary);
        }
        if (i==7) DrawPaneHeaderIcon(layout.commands[i],PaneHeaderIcon::Columns,selected ? theme.accent : theme.text_secondary);
    }
    if (layout.overflow.right > layout.overflow.left) {
        DrawButton(layout.overflow,theme,IsHovered(vm,HitTestResult::ToolbarMore) ? theme.fill_hover : kTransparent,
            L"\xE712",L"",theme.text_secondary,true,true);
    }
    // Pinned commands (自定义工具栏), after a divider.
    for (size_t i=0;i<layout.pins.size() && i<vm.toolbar_pins.size();++i) {
        const D2D1_RECT_F b=layout.pins[i];
        if (b.right <= b.left) continue;
        const SelectionChipAction& pin=vm.toolbar_pins[i];
        if (i==0) {
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            FillRect(dc,brStrokeDivider_.get(),b.left-7*scale_,b.top+7*scale_,scale_,18*scale_);
        }
        DrawButton(b,theme,pin.enabled && IsHovered(vm,HitTestResult::ToolbarPin,static_cast<int>(i)) ? theme.fill_hover : kTransparent,
            pin.glyph,L"",pin.enabled ? theme.text_secondary : theme.text_disabled,true,true);
    }
    DrawToolbarApps(vm,layout,theme);
    const auto command = [&](D2D1_RECT_F bounds, const wchar_t* glyph, l10n::StringId label,
                             HitTestResult::Region region, bool dropdown, float turn = 0.0f) {
        fluent::ButtonSpec button;
        button.bounds=bounds;
        button.glyph=glyph;
        button.icon_only=bounds.right-bounds.left < 60*scale_;
        button.text=button.icon_only ? std::wstring_view{} : std::wstring_view{l10n::Get(label)};
        button.kind=fluent::ButtonKind::Transparent;
        button.bordered=false;
        button.drop_down=dropdown && !button.icon_only;
        button.chevron_turn=turn;
        button.state.hovered=IsHovered(vm,region);
        painter_.DrawButton(button);
    };
    // Group dividers as in the prototype: after 新建, and between the view
    // commands and the pane layout buttons.
    MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
    if (layout.commands[0].right > layout.commands[0].left)
        FillRect(dc,brStrokeDivider_.get(),layout.create.right+6*scale_,layout.create.top+7*scale_,scale_,18*scale_);
    const D2D1_RECT_F view_tail=layout.group.right > layout.group.left ? layout.group : layout.filter;
    const float pane_left=layout.commands[5].right > layout.commands[5].left ? layout.commands[5].left
                        : layout.overflow.right > layout.overflow.left ? layout.overflow.left : 0.0f;
    if (pane_left > view_tail.right+8*scale_)
        FillRect(dc,brStrokeDivider_.get(),pane_left-7*scale_,layout.sort.top+7*scale_,scale_,18*scale_);
    // The shared drum picker turns the chevron of the button that opened it,
    // in step with its open/close fade.
    const float picker_open=group_wheel_.OpenAmount();
    const bool sort_picker=group_wheel_.IsSortPicker();
    command(layout.sort,L"\xE8CB",l10n::StringId::ToolbarSort,HitTestResult::ToolbarSort,true,
        sort_picker ? picker_open : 0.0f);
    if (vm.pane.filter_expand <= 0.015f && !vm.filter_editing) {
        command(layout.filter,kIconFilter,l10n::StringId::ToolbarFilter,HitTestResult::FilterBox,false);
    } else {
        fluent::TextFieldSpec field;
        field.bounds=layout.filter;
        field.placeholder=l10n::Get(l10n::StringId::FilterPlaceholder);
        field.text=vm.pane.filter_text;
        field.leading_glyph=kIconFilter;
        field.compact_leading_glyph=true;
        field.suppress_text=vm.filter_editing;
        field.state.focused=vm.filter_editing;
        field.state.hovered=IsHovered(vm,HitTestResult::FilterBox);
        if (!vm.pane.filter_text.empty()) field.trailing_width=30;
        painter_.DrawTextField(field);
        if (!vm.pane.filter_text.empty() && vm.pane.filter_expand >= 0.985f) {
            DrawButton(FilterClearRect(rect,vm.pane.filter_expand),theme,
                IsHovered(vm,HitTestResult::FilterClear) ? theme.fill_hover : kTransparent,
                L"\xE711",L"",theme.text_secondary,true,true,0.65f);
        }
    }
    // "Group" sits with Sort and Filter so grouping is findable and, once on,
    // always visible: the chip names the grouping and its x switches it off.
    if (layout.group.right > layout.group.left) {
        const D2D1_RECT_F g=layout.group;
        const bool icon_only=g.right-g.left < 60*scale_;
        if (toolbar_group_ <= 0) {
            command(g,L"\xF168",l10n::StringId::ToolbarGroup,HitTestResult::ToolbarGroup,true,
                sort_picker ? 0.0f : picker_open);
        } else if (icon_only) {
            DrawButton(g,theme,IsHovered(vm,HitTestResult::ToolbarGroup) ? theme.fill_hover : theme.fill_selected,
                L"\xF168",L"",theme.accent,true,true);
        } else {
            const float radius=(g.bottom-g.top)*0.5f;
            D2D1_COLOR_F fill=theme.accent;
            fill.a*=IsHovered(vm,HitTestResult::ToolbarGroup) ? 0.26f : 0.16f;
            painter_.FillRoundedRect(g,radius,fill);
            DrawIconText(g.left+6*scale_,g.top,24*scale_,g.bottom-g.top,L"\xF168",L"",theme.accent,0.8f);
            const D2D1_RECT_F x=ToolbarGroupClearRect(g,scale_);
            if (IsHovered(vm,HitTestResult::ToolbarGroupClear)) {
                D2D1_COLOR_F xf=theme.accent;
                xf.a*=0.28f;
                painter_.FillRoundedRect(x,(x.bottom-x.top)*0.5f,xf);
            }
            DrawIconText(x.left,x.top,x.right-x.left,x.bottom-x.top,kIconCloseSmall,L"x",theme.accent,0.5f);
            std::wstring group_label=ToolbarGroupLabel();
            const D2D1_RECT_F text_rc=D2D1::RectF(g.left+30*scale_,g.top,x.left-2*scale_,g.bottom);
            if (IDWriteFactory2* f=compositor_->DwriteFactory()) {
                group_label=FitEndEllipsis(group_label,std::max(0.0f,text_rc.right-text_rc.left),
                    [&](const std::wstring& t){ return MeasureTextWidth(f,compositor_->TextFormat(),t); });
            }
            painter_.DrawText(group_label,text_rc,compositor_->TextFormat(),theme.accent);
        }
    }
    // Selection context (prototype note 4): shows up after the file commands
    // while something is selected and folds away with the selection.
    const ToolbarSelectionChip sel=SelectionChipAt(layout,rect.right,vm.toolbar_drop_mode ? 0 : vm.pane.selected_count,
        static_cast<int>(vm.selection_actions.size()));
    if (sel.chip.right > sel.chip.left) {
        FillRect(dc,brStrokeDivider_.get(),sel.chip.left-7*scale_,sel.chip.top+7*scale_,scale_,18*scale_);
        D2D1_COLOR_F fill=theme.accent;
        fill.a*=0.10f;
        painter_.FillRoundedRect(sel.chip,7*scale_,fill);
        D2D1_COLOR_F edge=theme.accent;
        edge.a*=0.28f;
        ComPtr<ID2D1SolidColorBrush> edge_brush;
        dc->CreateSolidColorBrush(edge,&edge_brush);
        if (edge_brush.get()) {
            const float h=0.5f*scale_;
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sel.chip.left+h,sel.chip.top+h,
                sel.chip.right-h,sel.chip.bottom-h),7*scale_,7*scale_),edge_brush.get(),scale_);
        }
        if (sel.label.right > sel.label.left)
            painter_.DrawText(SelectionChipLabel(vm.pane.selected_count),sel.label,compositor_->SmallFormat(),theme.accent);
        D2D1_COLOR_F hot=theme.accent;
        hot.a*=0.22f;
        int last_action=-1;
        for (int i=0;i<kSelChipSlots;++i) {
            const D2D1_RECT_F b=sel.buttons[static_cast<size_t>(i)];
            if (b.right <= b.left) continue;
            if (i < kSelChipMore) last_action=i;
            const std::wstring glyph=i==kSelChipMore ? std::wstring(L"\xE712")
                : i==kSelChipClear ? std::wstring(kIconCloseSmall) : vm.selection_actions[static_cast<size_t>(i)].glyph;
            DrawButton(b,theme,IsHovered(vm,HitTestResult::ToolbarSelAction,i) ? hot : kTransparent,
                glyph,L"",theme.accent,true,true,i==kSelChipClear ? 0.6f : 0.85f);
        }
        if (last_action >= 0) {
            // Quick actions | more and clear.
            const D2D1_RECT_F b=sel.buttons[static_cast<size_t>(last_action)];
            FillRect(dc,brStrokeDivider_.get(),b.right+4.5f*scale_,b.top+7*scale_,scale_,b.bottom-b.top-14*scale_);
        }
    }
}
std::wstring MainRenderer::ToolbarGroupLabel() const {
    constexpr l10n::StringId names[]={l10n::StringId::GroupByName,l10n::StringId::GroupByDate,
        l10n::StringId::GroupByType,l10n::StringId::GroupBySize,l10n::StringId::GroupByTag,
        l10n::StringId::Location};
    std::wstring label=l10n::Get(l10n::StringId::ToolbarGroupActive);
    const size_t at=label.find(L"{g}");
    if (at!=std::wstring::npos) label.replace(at,3,l10n::Get(names[std::clamp(toolbar_group_,1,6)-1]));
    return label;
}
}
