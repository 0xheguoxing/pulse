#include "ui_renderer_internal.h"
#include "toolbar_layout.h"
#include "pane_header_icons.h"
#include "toolbar_pin_drag.h"
#include <unordered_map>

namespace pulse::ui {
namespace {
// Toolbar motion (pulse_motion_v2, the sidebar's and menus' plate): one hover
// plate per row glides between the buttons under the pointer - the navigation
// buttons, the address bar's breadcrumb segments, and the command row - and
// fades in and out at the ends. The state lives here, per renderer; frames are
// asked for with a one-shot TIMERPROC timer, so nothing ticks once it settles.
constexpr float kPlateFadeInMs = 90.0f, kPlateFadeOutMs = 120.0f;
constexpr UINT_PTR kToolbarFrameTimer = 0x54424D54;  // 'TBMT'

void CALLBACK ToolbarFrameTick(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    InvalidateRect(hwnd, nullptr, FALSE);
}

struct GlidePlate {
    motion::SpringRectMotion spring;
    uint64_t context = 0;
    uint64_t last = 0;
    int64_t key = -1;
    float alpha = 0.0f;
    float radius = 0.0f;
    D2D1_RECT_F rect{}, target{};

    // hot: the hovered button's rect (nullptr: nothing hovered). Returns true
    // while the plate still moves or fades.
    bool Step(int64_t hot_key, const D2D1_RECT_F* hot, float hot_radius, uint64_t frame, uint64_t now,
              bool animations) {
        const float dt = static_cast<float>((std::min<uint64_t>)(now - last, 64));
        last = now;
        spring.SetAnimationsEnabled(animations);
        if (hot) {
            // Coming from nothing, the plate appears at the button (no glide
            // from wherever it was last).
            if (alpha <= 0.0f) ++context;
            key = hot_key;
            target = *hot;
            radius = hot_radius;
            rect = spring.Update(context, key, target, frame, now, motion::spring::Plate);
            alpha = animations ? (std::min)(1.0f, alpha + dt / kPlateFadeInMs) : 1.0f;
        } else {
            // Fades out where it was; a pointer coming back mid-fade glides on.
            alpha = animations ? (std::max)(0.0f, alpha - dt / kPlateFadeOutMs) : 0.0f;
            if (alpha > 0.0f) rect = spring.Update(context, key, target, frame, now, motion::spring::Plate);
            else spring.Hide();
        }
        return spring.Active(now) || (alpha > 0.0f && alpha < 1.0f);
    }
    void Reset() noexcept {
        alpha = 0.0f;
        spring.Hide();
    }
};

// Pinned tools and apps (toolbar picker T2): a pin that shows up pops in
// from 0.55 on the Pop spring; a toggle that flips turns its glyph in from
// -50 degrees and half size (300 ms, slight overshoot). Keyed by command or
// app name, so a pin keeps its state when it moves along the row. Nothing
// pops on the first paint.
constexpr float kPinPopFrom = 0.55f;
constexpr float kToggleTurnMs = 300.0f;
constexpr float kToggleTurnDeg = -50.0f;

struct PinMotion {
    uint64_t born = 0, flipped = 0, seen = 0;
    bool checked = false;
    // Picker T3c: a pin whose place in the row changed (reordered, a neighbour
    // added or removed, or let go after a drag) slides there from where it was
    // drawn (240 ms, ease out). A resize or the selection chip moves pins
    // without changing their index, so they don't slide then.
    bool placed = false, dragged = false;
    int index = -1;
    float visual = 0.0f, slide_from = 0.0f;
    uint64_t slide_at = 0;
};
constexpr float kPinSlideMs = 240.0f;

struct PinMotions {
    std::unordered_map<std::wstring, PinMotion> map;
    bool primed = false;
    bool moving = false;   // set while any pin pops or turns this frame

    PinMotion& Touch(const std::wstring& key, bool checked, uint64_t frame, uint64_t now, bool animate) {
        auto [it, added] = map.try_emplace(key);
        PinMotion& m = it->second;
        if (added) {
            m.checked = checked;
            m.born = primed && animate ? (now ? now : 1) : 0;
        } else if (m.checked != checked) {
            m.checked = checked;
            m.flipped = animate ? (now ? now : 1) : 0;
        }
        m.seen = frame;
        return m;
    }
    // Pins not drawn this frame are forgotten: they pop again when they return.
    void Sweep(uint64_t frame) {
        std::erase_if(map, [frame](const auto& entry) { return entry.second.seen != frame; });
        primed = true;
    }
    // Scale of the whole button while it pops in (1 when settled).
    float Pop(PinMotion& m, uint64_t now) {
        if (!m.born) return 1.0f;
        const float t = static_cast<float>(now - m.born);
        if (t >= static_cast<float>(motion::SpringSettleMs(motion::spring::Pop))) { m.born = 0; return 1.0f; }
        moving = true;
        return kPinPopFrom + (1.0f - kPinPopFrom) * motion::SpringProgress(motion::spring::Pop, t);
    }
    // Where the pin is drawn this frame: its slot, sliding, or under the pointer.
    D2D1_RECT_F Place(PinMotion& m, const D2D1_RECT_F& b, int index, const ToolbarPinDragState& drag, uint64_t now,
                      bool animate) {
        const float w = b.right - b.left;
        if (drag.index == index && drag.index >= 0) {
            m.placed = m.dragged = true;
            m.index = index;
            m.visual = drag.left;
            m.slide_at = 0;
            return D2D1::RectF(drag.left, b.top + drag.dy, drag.left + w, b.bottom + drag.dy);
        }
        if (!m.placed) {
            m.placed = true;
            m.visual = b.left;
        } else if ((index != m.index || m.dragged) && animate && primed && std::fabs(m.visual - b.left) > 0.5f) {
            m.slide_from = m.visual - b.left;
            m.slide_at = now ? now : 1;
        }
        m.index = index;
        m.dragged = false;
        float off = 0.0f;
        if (m.slide_at) {
            const float t = static_cast<float>(now - m.slide_at) / kPinSlideMs;
            if (t >= 1.0f) m.slide_at = 0;
            else { off = m.slide_from * (1.0f - motion::EaseOutCubic(t)); moving = true; }
        }
        m.visual = b.left + off;
        return D2D1::RectF(b.left + off, b.top, b.right + off, b.bottom);
    }
    // Glyph turn and scale while a toggle flips.
    void Turn(PinMotion& m, uint64_t now, float& degrees, float& scale) {
        degrees = 0.0f;
        scale = 1.0f;
        if (!m.flipped) return;
        const float t = static_cast<float>(now - m.flipped) / kToggleTurnMs;
        if (t >= 1.0f) { m.flipped = 0; return; }
        moving = true;
        const float e = motion::EaseOutBack(t);
        degrees = kToggleTurnDeg * (1.0f - e);
        scale = 0.5f + 0.5f * e;
    }
};

// Scales (and turns) what is drawn inside around a centre; no-op at rest.
struct AroundCentre {
    ID2D1DeviceContext* dc;
    D2D1_MATRIX_3X2_F saved{};
    bool on = false;
    AroundCentre(ID2D1DeviceContext* context, D2D1_POINT_2F centre, float scale, float degrees = 0.0f) : dc(context) {
        if (!dc || (scale == 1.0f && degrees == 0.0f)) return;
        dc->GetTransform(&saved);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(degrees, centre) *
                         D2D1::Matrix3x2F::Scale(scale, scale, centre) * saved);
        on = true;
    }
    ~AroundCentre() { if (on) dc->SetTransform(&saved); }
    AroundCentre(const AroundCentre&) = delete;
    AroundCentre& operator=(const AroundCentre&) = delete;
};

struct ToolbarMotion {
    GlidePlate nav, crumbs, commands;
    PinMotions pins;
    // The More button's count of pins folded into it; pops when it changes.
    int folded = 0;
    uint64_t folded_born = 0;
    bool animations = true;
    uint64_t animations_checked = 0;
};

ToolbarMotion& ToolbarMotionFor(const void* renderer) {
    static std::unordered_map<const void*, ToolbarMotion> motions;
    return motions[renderer];
}

// GulpToolbarApp: the app pin that took a drop, per renderer.
struct ToolbarGulp { int app = -1; uint64_t born = 0; };
std::unordered_map<const void*, ToolbarGulp>& Gulps() {
    static std::unordered_map<const void*, ToolbarGulp> gulps;
    return gulps;
}
std::unordered_map<const void*, ToolbarPinDragState>& PinDrags() {
    static std::unordered_map<const void*, ToolbarPinDragState> drags;
    return drags;
}
const ToolbarPinDragState& PinDragFor(const void* renderer) {
    static const ToolbarPinDragState none;
    const auto it = PinDrags().find(renderer);
    return it == PinDrags().end() ? none : it->second;
}

// Dashed outline of the picker's preview pin; one per D2D factory.
ID2D1StrokeStyle* PreviewDashes(ID2D1DeviceContext* dc) {
    static ComPtr<ID2D1StrokeStyle> style;
    static ID2D1Factory* owner = nullptr;
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory.get()) return nullptr;
    if (owner != factory.get()) {
        style.reset();
        const float dashes[] = { 3.0f, 2.5f };
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                   D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                                   dashes, 2, &style);
        owner = factory.get();
    }
    return style.get();
}

int64_t PlateKey(HitTestResult::Region region, int index) {
    return static_cast<int64_t>(region) * 4096 + index;
}

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
    const bool plate_hover=!IsHighContrast();  // DrawToolbar's gliding plate shows the hover
    ID2D1DeviceContext* dc=compositor_->Dc();
    ToolbarMotion& tm=ToolbarMotionFor(this);
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
    // A drop's gulp (prototype .tb.gulp): 0.82 at 35 %, then an overshooting
    // spring back to 1, 340 ms.
    int gulp_app=-1;
    float gulp_scale=1.0f;
    if (auto it=Gulps().find(this); it!=Gulps().end()) {
        ToolbarGulp& g=it->second;
        if (!g.born) g.born=motion_now_ ? motion_now_ : 1;
        const float t=static_cast<float>(motion_now_-g.born)/340.0f;
        if (t>=1.0f || !tm.animations) {
            Gulps().erase(it);
        } else {
            gulp_app=g.app;
            if (t<0.35f) {
                const float v=1.0f-t/0.35f;
                gulp_scale=1.0f-0.18f*(1.0f-v*v*v);
            } else {
                const float u=(t-0.35f)/0.65f, c1=1.70158f*1.4f, c3=c1+1.0f;
                gulp_scale=0.82f+0.18f*(1.0f+c3*(u-1.0f)*(u-1.0f)*(u-1.0f)+c1*(u-1.0f)*(u-1.0f));
            }
            tm.pins.moving=true;
        }
    }
    for (size_t i=0;i<vm.toolbar_apps.size() && from+i<layout.pins.size();++i) {
        const D2D1_RECT_F slot=layout.pins[from+i];
        if (slot.right <= slot.left) continue;
        const WindowViewModel::ToolbarAppPin& app=vm.toolbar_apps[i];
        if (i==0 && from > 0) {   // short divider between the commands and the apps
            const float x=slot.left-6.5f*scale_;
            FillRect(dc,brStrokeDivider_.get(),x,slot.top+9*scale_,scale_,14*scale_);
        }
        PinMotion& pm=tm.pins.Touch(L"a"+app.name,false,motion_frame_,motion_now_,tm.animations);
        const ToolbarPinDragState& drag=PinDragFor(this);
        const bool dragged=drag.index==static_cast<int>(from+i);
        const D2D1_RECT_F b=tm.pins.Place(pm,slot,static_cast<int>(from+i),drag,motion_now_,tm.animations);
        const AroundCentre pop(dc,D2D1::Point2F((b.left+b.right)*0.5f,(b.top+b.bottom)*0.5f),
                               tm.pins.Pop(pm,motion_now_)*(dragged ? 1.08f : 1.0f));
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
        } else if (!plate_hover && IsHovered(vm,HitTestResult::ToolbarPin,static_cast<int>(from+i))) {
            painter_.FillRoundedRect(b,6*scale_,theme.fill_hover);
        }
        const float cx=std::round((b.left+b.right)*0.5f);
        const float cy=std::round((b.top+b.bottom)*0.5f-(app.fit!=0 ? 1.5f*scale_ : 0.0f));
        const float half=8*scale_;
        float opacity=app.fit==3 && !(target && vm.toolbar_drop_force) ? 0.35f : 1.0f;
        if (dragged && drag.out) opacity*=0.35f;
        const AroundCentre gulp(dc,D2D1::Point2F(cx,cy),gulp_app==static_cast<int>(i) ? gulp_scale : 1.0f);
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
    ToolbarMotion& tm=ToolbarMotionFor(this);
    if (motion_now_-tm.animations_checked > 2000 || !tm.animations_checked) {
        // The Windows animation switch, re-read every couple of seconds.
        tm.animations_checked=motion_now_ ? motion_now_ : 1;
        tm.animations=motion::SystemAnimationsEnabled();
    }
    // High contrast keeps each button's own static hover.
    const bool plates=!IsHighContrast();
    bool toolbar_moving=false;
    // Every return below asks for the next frame while a plate moves.
    struct FrameRequest {
        HWND hwnd;
        const bool& moving;
        ~FrameRequest() {
            if (moving && hwnd) SetTimer(hwnd,kToolbarFrameTimer,15,ToolbarFrameTick);
        }
    } frame_request{compositor_ ? compositor_->Hwnd() : nullptr,toolbar_moving};
    const auto draw_plate=[&](const GlidePlate& plate) {
        if (plate.alpha <= 0.0f) return;
        D2D1_COLOR_F fill=theme.fill_hover;
        fill.a*=plate.alpha;
        painter_.FillRoundedRect(plate.rect,plate.radius,fill);
    };
    if (searchOverlay || !plates) tm.nav.Reset();
    if (searchOverlay || !plates || (vm.address_editing && !vm.address_searching)) tm.crumbs.Reset();
    if (!searchOverlay) {
        const wchar_t* nav_glyphs[] = {kIconBack,kIconForward,kIconUp,kIconRefresh};
        const HitTestResult::Region nav_hits[] = {HitTestResult::NavBack,HitTestResult::NavForward,HitTestResult::NavUp,HitTestResult::NavRefresh};
        if (plates) {
            const D2D1_RECT_F* hot=nullptr;
            int hot_i=-1;
            for (int i=0;i<4;++i) {
                const auto& r=layout.navigation[i];
                const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
                if (r.right>r.left && enabled && IsHovered(vm,nav_hits[i])) { hot=&r; hot_i=i; }
            }
            if (tm.nav.Step(hot_i,hot,theme.radius_control*scale_,motion_frame_,motion_now_,tm.animations))
                toolbar_moving=true;
            draw_plate(tm.nav);
        }
        for (int i=0;i<4;++i) {
            const auto r=layout.navigation[i];
            if (r.right<=r.left) continue;
            const bool enabled=i==0 ? vm.can_go_back : i==1 ? vm.can_go_forward : true;
            fluent::ButtonSpec button;
            button.bounds = r; button.glyph = nav_glyphs[i];
            button.state.enabled = enabled;
            button.state.hovered = !plates && enabled && IsHovered(vm, nav_hits[i]);
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
            if (plates) {
                const int hot_i = vm.breadcrumb_hover >= 0 && vm.breadcrumb_hover < static_cast<int>(placed.size()) &&
                                  vm.breadcrumb_hover != vm.breadcrumb_drop ? vm.breadcrumb_hover : -1;
                if (tm.crumbs.Step(hot_i, hot_i >= 0 ? &placed[static_cast<size_t>(hot_i)].rc : nullptr,
                                   theme.radius_control * scale_, motion_frame_, motion_now_, tm.animations))
                    toolbar_moving = true;
                draw_plate(tm.crumbs);
            }
            for (size_t i = 0; i < placed.size(); ++i) {
                const auto& seg = placed[i];
                if ((int)i == vm.breadcrumb_drop) {
                    // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                    dc->DrawRoundedRectangle(
                        D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                        brAccent_.get(), 2.0f * scale_);
                } else if (!plates && (int)i == vm.breadcrumb_hover) {
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
    // The command row's plate: the file and pane commands, overflow, pinned
    // commands and apps, Sort / Filter / Group.
    const bool filter_field=!(vm.pane.filter_expand <= 0.015f && !vm.filter_editing);
    const bool group_chip=toolbar_group_ > 0;
    if (plates) {
        const D2D1_RECT_F* hot=nullptr;
        int64_t hot_key=-1;
        float hot_radius=theme.radius_control*scale_;
        const auto consider=[&](const D2D1_RECT_F& r, bool hovered, int64_t key, float radius) {
            if (!hot && hovered && r.right > r.left) { hot=&r; hot_key=key; hot_radius=radius; }
        };
        const float rc=theme.radius_control*scale_;
        consider(layout.overflow,IsHovered(vm,HitTestResult::ToolbarMore),PlateKey(HitTestResult::ToolbarMore,0),rc);
        const size_t from=vm.toolbar_pins.size();
        for (size_t i=0;i<layout.pins.size();++i) {
            const int idx=static_cast<int>(i);
            if (!IsHovered(vm,HitTestResult::ToolbarPin,idx)) continue;
            if (i<from) consider(layout.pins[i],vm.toolbar_pins[i].enabled,PlateKey(HitTestResult::ToolbarPin,idx),rc);
            else if (i-from<vm.toolbar_apps.size() && vm.toolbar_drop_app!=static_cast<int>(i-from))
                consider(layout.pins[i],true,PlateKey(HitTestResult::ToolbarPin,idx),6*scale_);
        }
        consider(layout.sort,IsHovered(vm,HitTestResult::ToolbarSort),PlateKey(HitTestResult::ToolbarSort,0),rc);
        if (!filter_field)
            consider(layout.filter,IsHovered(vm,HitTestResult::FilterBox),PlateKey(HitTestResult::FilterBox,0),rc);
        if (!group_chip)
            consider(layout.group,IsHovered(vm,HitTestResult::ToolbarGroup),PlateKey(HitTestResult::ToolbarGroup,0),rc);
        if (tm.commands.Step(hot_key,hot,hot_radius,motion_frame_,motion_now_,tm.animations)) toolbar_moving=true;
        draw_plate(tm.commands);
    } else {
        tm.commands.Reset();
    }
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
    // The split tool's pin shows the current layout (1 / 2 side by side /
    // 2 stacked / 3 / 4). A pane count that does not match the preset (a
    // custom split) is described from the pane rectangles instead.
    const auto split_icon=[&vm]() {
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
        return kIcons[preset];
    };
    if (layout.overflow.right > layout.overflow.left) {
        DrawButton(layout.overflow,theme,!plates && IsHovered(vm,HitTestResult::ToolbarMore) ? theme.fill_hover : kTransparent,
            L"\xE712",L"",theme.text_secondary,true,true);
    }
    // Pinned commands (自定义工具栏), after a divider.
    const ToolbarPinDragState& pin_drag=PinDragFor(this);
    for (size_t i=0;i<layout.pins.size() && i<vm.toolbar_pins.size();++i) {
        const D2D1_RECT_F slot=layout.pins[i];
        if (slot.right <= slot.left) continue;
        const SelectionChipAction& pin=vm.toolbar_pins[i];
        const bool split_pin=pin.glyph==kToolbarSplitGlyph;
        if (i==0) {
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            FillRect(dc,brStrokeDivider_.get(),slot.left-7*scale_,slot.top+7*scale_,scale_,18*scale_);
        }
        const bool dragged=pin_drag.index==static_cast<int>(i);
        const bool preview=pin_drag.preview_command!=0 && pin.command==pin_drag.preview_command;
        // Toggle tools that are on look like the selected pane buttons.
        const bool hovered=pin.enabled && IsHovered(vm,HitTestResult::ToolbarPin,static_cast<int>(i));
        D2D1_COLOR_F bg=hovered && !plates ? theme.fill_hover : pin.checked ? theme.fill_selected : kTransparent;
        D2D1_COLOR_F fg=!pin.enabled ? theme.text_disabled : pin.checked ? theme.accent : theme.text_secondary;
        if (dragged && pin_drag.out) { fg.a*=0.35f; bg.a*=0.35f; }
        PinMotion& pm=tm.pins.Touch(L"c"+std::to_wstring(pin.command),pin.checked,motion_frame_,motion_now_,tm.animations);
        float turn=0.0f, glyph_scale=1.0f;
        tm.pins.Turn(pm,motion_now_,turn,glyph_scale);
        const D2D1_RECT_F b=tm.pins.Place(pm,slot,static_cast<int>(i),pin_drag,motion_now_,tm.animations);
        const D2D1_POINT_2F centre=D2D1::Point2F((b.left+b.right)*0.5f,(b.top+b.bottom)*0.5f);
        const AroundCentre pop(dc,centre,tm.pins.Pop(pm,motion_now_)*(dragged ? 1.08f : 1.0f));
        if (preview) {
            // The picker's hovered tool: where it would go, dashed and faint.
            D2D1_COLOR_F line=theme.accent;
            line.a*=0.7f;
            ComPtr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(line,&brush);
            const float h=0.5f*scale_;
            if (brush.get())
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left+h,b.top+h,b.right-h,b.bottom-h),
                    theme.radius_control*scale_,theme.radius_control*scale_),brush.get(),scale_,PreviewDashes(dc));
            D2D1_COLOR_F faint=theme.text_secondary;
            faint.a*=0.5f;
            DrawIconText(b.left,b.top,b.right-b.left,b.bottom-b.top,pin.glyph,L"",faint);
        } else if (turn==0.0f && glyph_scale==1.0f) {
            DrawButton(b,theme,bg,split_pin ? L"" : pin.glyph.c_str(),L"",fg,true,true);
            if (split_pin) DrawPaneHeaderIcon(b,split_icon(),fg);
        } else {
            if (bg.a > 0.0f) painter_.FillRoundedRect(b,theme.radius_control*scale_,bg);
            const AroundCentre spin(dc,centre,glyph_scale,turn);
            if (split_pin) DrawPaneHeaderIcon(b,split_icon(),fg);
            else DrawIconText(b.left,b.top,b.right-b.left,b.bottom-b.top,pin.glyph,L"",fg);
        }
    }
    DrawToolbarApps(vm,layout,theme);
    // The picker's hovered pinned tile: its pin breathes an accent ring
    // (prototype .tb.linked: 1.5 px strong to 3 px soft, 0.9 s each way).
    if (pin_drag.linked>=0 && static_cast<size_t>(pin_drag.linked)<layout.pins.size() &&
        layout.pins[pin_drag.linked].right>layout.pins[pin_drag.linked].left) {
        float k=0.0f;
        if (tm.animations) {
            const float t=static_cast<float>(motion_now_%1800)/900.0f;
            const float tri=t<=1.0f ? t : 2.0f-t;
            k=tri*tri*(3.0f-2.0f*tri);
            toolbar_moving=true;
        }
        const float width=(1.5f+1.5f*k)*scale_;
        D2D1_COLOR_F ring=theme.accent;
        ring.a*=0.7f+(0.22f-0.7f)*k;
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(ring,&brush);
        if (brush.get()) {
            const D2D1_RECT_F r=layout.pins[pin_drag.linked];
            const float o=width*0.5f, radius=theme.radius_control*scale_+o;
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left-o,r.top-o,r.right+o,r.bottom+o),radius,radius),
                                     brush.get(),width);
        }
    }
    // Pins with no room (narrow window, open filter) are in More: a small
    // count on it says how many.
    if (layout.overflow.right > layout.overflow.left) {
        const size_t total=vm.toolbar_pins.size()+vm.toolbar_apps.size();
        int folded=0;
        for (size_t i=0;i<total;++i)
            if (i>=layout.pins.size() || layout.pins[i].right <= layout.pins[i].left) ++folded;
        if (folded!=tm.folded) {
            if (folded>tm.folded && tm.pins.primed && tm.animations) tm.folded_born=motion_now_ ? motion_now_ : 1;
            tm.folded=folded;
        }
        if (folded>0) {
            float s=1.0f;
            if (tm.folded_born) {
                const float t=static_cast<float>(motion_now_-tm.folded_born);
                if (t < static_cast<float>(motion::SpringSettleMs(motion::spring::Pop))) {
                    s=kPinPopFrom+(1.0f-kPinPopFrom)*motion::SpringProgress(motion::spring::Pop,t);
                    toolbar_moving=true;
                } else {
                    tm.folded_born=0;
                }
            }
            const std::wstring count=folded>9 ? L"9+" : std::to_wstring(folded);
            const float h=16*scale_;
            const float w=(count.size()>1 ? 22.0f : 16.0f)*scale_;
            const D2D1_RECT_F badge=D2D1::RectF(layout.overflow.right-w+3*scale_,layout.overflow.top-2*scale_,
                                                layout.overflow.right+3*scale_,layout.overflow.top-2*scale_+h);
            const AroundCentre pop(dc,D2D1::Point2F((badge.left+badge.right)*0.5f,(badge.top+badge.bottom)*0.5f),s);
            painter_.FillRoundedRect(badge,h*0.5f,theme.accent);
            // The caption box is taller than the pill; let it overhang so the
            // digits are not clipped.
            painter_.DrawText(count,D2D1::RectF(badge.left,badge.top-3*scale_,badge.right,badge.bottom+3*scale_),
                              compositor_->SmallFormat(),theme.accent_text,fluent::HorizontalAlignment::Center,theme.accent);
        }
    } else {
        tm.folded=0;
    }
    tm.pins.Sweep(motion_frame_);
    if (tm.pins.moving) toolbar_moving=true;
    tm.pins.moving=false;
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
        button.state.hovered=!plates && IsHovered(vm,region);
        painter_.DrawButton(button);
    };
    // The divider after 新建 comes with the first pin (above); the pane
    // buttons are pins now (T3d), so More needs none.
    // The shared drum picker turns the chevron of the button that opened it,
    // in step with its open/close fade.
    const float picker_open=group_wheel_.OpenAmount();
    const bool sort_picker=group_wheel_.IsSortPicker();
    command(layout.sort,L"\xE8CB",l10n::StringId::ToolbarSort,HitTestResult::ToolbarSort,true,
        sort_picker ? picker_open : 0.0f);
    if (!filter_field) {
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
        if (!group_chip) {
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

void GulpToolbarApp(const void* renderer, int app) {
    Gulps()[renderer] = ToolbarGulp{app, 0};   // born on the next frame
}
void SetToolbarPinDrag(const void* renderer, const ToolbarPinDragState& state) {
    if (state.index < 0 && state.preview_command == 0 && state.linked < 0) PinDrags().erase(renderer);
    else PinDrags()[renderer] = state;
}
}
