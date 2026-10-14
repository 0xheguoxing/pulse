#include "../common/windows_compat.h"
#include "toolbar_customize_dialog.h"

#include "confirm_dialog_view.h"
#include "typography.h"
#include "window_helpers.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr wchar_t kCustomizeClass[] = L"PulseToolbarCustomizeWindow";
constexpr float kWidth = 460.0f;
constexpr float kPad = 24.0f;
constexpr float kTitleBar = 36.0f;
constexpr float kCloseW = 46.0f;
constexpr float kHeadingH = 26.0f;
constexpr float kRowH = 36.0f;
constexpr float kRowInset = 10.0f;    // rows reach into the padding for their hover plate
constexpr float kButtonMinW = 96.0f;
constexpr float kButtonGap = 8.0f;
constexpr float kFooterPadY = 18.0f;

// Control ids: footer and close, then one id per row.
enum : int { kNone = 0, kDone = 1, kCancel = 2, kClose = 3, kPrimaryBase = 100, kPinBase = 200 };

float Dip(float scale, float value) { return value * scale; }

struct Formats {
    ComPtr<IDWriteTextFormat> title;
    ComPtr<IDWriteTextFormat> intro;
    bool Create(IDWriteFactory2* factory, float scale) {
        title.reset();
        intro.reset();
        if (!factory) return false;
        typography::CreateTextFormat(factory,
            {typography::FontRole::Display, 18.0f * scale, DWRITE_FONT_WEIGHT_SEMI_BOLD}, &title);
        typography::CreateTextFormat(factory, {typography::FontRole::Text, 12.5f * scale}, &intro);
        for (IDWriteTextFormat* format : {title.get(), intro.get()}) {
            if (!format) return false;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        return true;
    }
};

struct Layout {
    float width = 0.0f;
    float height = 0.0f;
    float title_bar = 0.0f;
    D2D1_RECT_F close{}, title{}, intro{}, primary_heading{}, pin_heading{};
    std::vector<D2D1_RECT_F> primary, pins;
    D2D1_RECT_F footer{}, done{}, cancel{};
};

struct Visual {
    int hover = kNone;
    int pressed = kNone;
    int focus = kDone;
    bool show_focus = false;
};

float Wrapped(IDWriteFactory2* factory, IDWriteTextFormat* format, std::wstring_view text, float width) {
    if (!factory || !format || text.empty() || width <= 0.0f) return 0.0f;
    return std::ceil(typography::MeasureWrapped(factory, format, text, width));
}

Layout LayoutDialog(const ToolbarCustomizeSpec& spec, const fluent::Painter& painter,
                    IDWriteFactory2* factory, const Formats& formats, float scale) {
    Layout l;
    const float pad = Dip(scale, kPad);
    const float width = std::ceil(Dip(scale, kWidth));
    const float text_w = width - pad * 2.0f;
    l.title_bar = Dip(scale, kTitleBar);
    l.close = D2D1::RectF(width - Dip(scale, kCloseW), 0, width, l.title_bar);
    float y = Dip(scale, 28.0f);
    const float title_h = std::max(Wrapped(factory, formats.title.get(), spec.title, text_w), Dip(scale, 24.0f));
    l.title = D2D1::RectF(pad, y, width - pad, y + title_h);
    y = l.title.bottom + Dip(scale, 4.0f);
    const float intro_h = Wrapped(factory, formats.intro.get(), spec.intro, text_w);
    l.intro = D2D1::RectF(pad, y, width - pad, y + intro_h);
    y = l.intro.bottom + Dip(scale, 14.0f);

    const auto rows = [&](D2D1_RECT_F& heading, std::vector<D2D1_RECT_F>& out, size_t count) {
        heading = D2D1::RectF(pad, y, width - pad, y + Dip(scale, kHeadingH));
        y = heading.bottom;
        for (size_t i = 0; i < count; ++i) {
            out.push_back(D2D1::RectF(pad - Dip(scale, kRowInset), y, width - pad + Dip(scale, kRowInset),
                                      y + Dip(scale, kRowH)));
            y += Dip(scale, kRowH);
        }
    };
    rows(l.primary_heading, l.primary, spec.primary.size());
    y += Dip(scale, 10.0f);
    rows(l.pin_heading, l.pins, spec.pins.size());
    y += Dip(scale, 20.0f);

    const float btn_h = painter.MeasureButtonHeight();
    const float done_w = std::max(painter.MeasureButtonWidth(spec.done_text), Dip(scale, kButtonMinW));
    const float cancel_w = std::max(painter.MeasureButtonWidth(spec.cancel_text), Dip(scale, kButtonMinW));
    l.footer = D2D1::RectF(0, y, width, y + btn_h + Dip(scale, kFooterPadY) * 2.0f);
    const float by = y + Dip(scale, kFooterPadY);
    l.done = D2D1::RectF(width - pad - done_w, by, width - pad, by + btn_h);
    l.cancel = D2D1::RectF(l.done.left - Dip(scale, kButtonGap) - cancel_w, by,
                           l.done.left - Dip(scale, kButtonGap), by + btn_h);
    l.width = width;
    l.height = std::ceil(l.footer.bottom);
    return l;
}

int HitTest(const Layout& l, const ToolbarCustomizeSpec& spec, float x, float y) {
    if (ContainsRect(l.close, x, y)) return kClose;
    if (ContainsRect(l.done, x, y)) return kDone;
    if (ContainsRect(l.cancel, x, y)) return kCancel;
    for (size_t i = 0; i < l.primary.size(); ++i)
        if (ContainsRect(l.primary[i], x, y)) return kPrimaryBase + static_cast<int>(i);
    for (size_t i = 0; i < l.pins.size(); ++i) {
        if (ContainsRect(l.pins[i], x, y))
            return spec.pins[i].locked ? kNone : kPinBase + static_cast<int>(i);
    }
    return kNone;
}

void DrawRows(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
              const std::vector<ToolbarCustomizeRow>& rows, const std::vector<D2D1_RECT_F>& rects,
              int base, bool radio, const Visual& visual) {
    const float scale = painter.Scale();
    for (size_t i = 0; i < rows.size() && i < rects.size(); ++i) {
        const ToolbarCustomizeRow& row = rows[i];
        const D2D1_RECT_F& r = rects[i];
        const int id = base + static_cast<int>(i);
        const bool hot = !row.locked && (visual.hover == id || visual.pressed == id);
        if (hot) painter.FillRoundedRect(r, Dip(scale, 7.0f), theme.fill_hover);
        if (visual.show_focus && visual.focus == id) painter.DrawFocusRing(r, Dip(scale, 7.0f));
        fluent::ControlState state{};
        state.enabled = !row.locked;
        state.checked = row.checked || row.locked;
        state.hovered = visual.hover == id;
        state.pressed = visual.pressed == id;
        const D2D1_RECT_F indicator = D2D1::RectF(r.left + Dip(scale, kRowInset), r.top,
                                                  r.left + Dip(scale, kRowInset + 20.0f), r.bottom);
        if (radio) painter.DrawRadioButton(indicator, L"", state);
        else painter.DrawCheckBox(indicator, L"", state);
        float x = indicator.right + Dip(scale, 12.0f);
        if (!row.glyph.empty()) {
            painter.DrawGlyph(row.glyph, D2D1::RectF(x, r.top, x + Dip(scale, 20.0f), r.bottom),
                              theme.text_secondary);
            x += Dip(scale, 28.0f);
        }
        float caption_left = r.right - Dip(scale, kRowInset);
        if (!row.caption.empty() && compositor.SmallFormat()) {
            const float w = std::ceil(typography::MeasureLine(&compositor, compositor.SmallFormat(), row.caption));
            caption_left -= w;
            painter.DrawText(row.caption, D2D1::RectF(caption_left, r.top, r.right - Dip(scale, kRowInset), r.bottom),
                             compositor.SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Right);
            caption_left -= Dip(scale, 12.0f);
        }
        painter.DrawText(row.label, D2D1::RectF(x, r.top, std::max(x, caption_left), r.bottom),
                         compositor.TextFormat(), row.locked ? theme.text_secondary : theme.text);
    }
}

void DrawDialog(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                const ToolbarCustomizeSpec& spec, const Layout& l, const Formats& formats,
                const Visual& visual, bool high_contrast) {
    const float scale = painter.Scale();
    fluent::ControlState close_state{};
    close_state.hovered = visual.hover == kClose;
    close_state.pressed = visual.pressed == kClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

    DrawDialogParagraph(compositor, formats.title.get(), l.title, spec.title, theme.text);
    DrawDialogParagraph(compositor, formats.intro.get(), l.intro, spec.intro,
                        high_contrast ? theme.text : theme.text_secondary);
    if (compositor.SmallFormat()) {
        painter.DrawText(spec.primary_heading, l.primary_heading, compositor.SmallFormat(), theme.text_secondary);
        painter.DrawText(spec.pin_heading, l.pin_heading, compositor.SmallFormat(), theme.text_secondary);
    }
    DrawRows(compositor, painter, theme, spec.primary, l.primary, kPrimaryBase, true, visual);
    DrawRows(compositor, painter, theme, spec.pins, l.pins, kPinBase, false, visual);

    // Footer band, as in the confirm dialog.
    painter.FillRoundedRect(l.footer, 0, theme.surface_sheet);
    painter.FillRoundedRect(D2D1::RectF(l.footer.left, l.footer.top, l.footer.right, l.footer.top + 1.0f),
                            0, theme.stroke_divider);
    const auto state_for = [&](int id) {
        fluent::ControlState state{};
        state.hovered = visual.hover == id;
        state.pressed = visual.pressed == id;
        state.keyboard_focus = visual.show_focus && visual.focus == id;
        return state;
    };
    painter.DrawButton({l.cancel, spec.cancel_text, {}, fluent::ButtonKind::Standard, state_for(kCancel)});
    painter.DrawButton({l.done, spec.done_text, {}, fluent::ButtonKind::Primary, state_for(kDone)});
    if (state_for(kDone).keyboard_focus) {
        const float gap = Dip(scale, 3.0f);
        painter.StrokeRoundedRect(D2D1::RectF(l.done.left - gap, l.done.top - gap, l.done.right + gap,
                                              l.done.bottom + gap),
                                  Dip(scale, theme.radius_control) + gap, theme.text, Dip(scale, 1.5f));
    }
    const float inset = 0.5f;
    painter.StrokeRoundedRect(D2D1::RectF(inset, inset, l.width - inset, l.height - inset),
                              Dip(scale, 12.0f), theme.stroke_card);
}

class CustomizeWindow {
public:
    bool Show(HWND owner, ToolbarCustomizeSpec& spec, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        spec_ = spec;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kCustomizeClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kCustomizeClass, &wc)) RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kCustomizeClass, spec_.title.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
            static_cast<int>(kWidth * scale_), static_cast<int>(520.0f * scale_), owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) return false;
        RECT placed{};
        GetWindowRect(hwnd_, &placed);
        CenterOwnedWindow(hwnd_, owner_, placed.right - placed.left, placed.bottom - placed.top);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            RedirectStrayModalKey(message, hwnd_);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        if (accepted_) spec = spec_;
        return accepted_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<CustomizeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<CustomizeWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void SizeToContent() {
        if (!hwnd_ || !compositor_.DwriteFactory()) return;
        formats_.Create(compositor_.DwriteFactory(), scale_);
        layout_ = LayoutDialog(spec_, painter_, compositor_.DwriteFactory(), formats_, scale_);
        const int width = static_cast<int>(std::ceil(layout_.width));
        const int height = static_cast<int>(std::ceil(layout_.height));
        sizing_ = true;
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        sizing_ = false;
        compositor_.Resize(width, height);
    }

    void Complete(bool accepted) {
        accepted_ = accepted;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int id) {
        if (id == kDone) { Complete(true); return; }
        if (id == kCancel || id == kClose) { Complete(false); return; }
        if (id >= kPinBase) {
            const size_t i = static_cast<size_t>(id - kPinBase);
            if (i < spec_.pins.size() && !spec_.pins[i].locked) spec_.pins[i].checked = !spec_.pins[i].checked;
        } else if (id >= kPrimaryBase) {
            const size_t pick = static_cast<size_t>(id - kPrimaryBase);
            for (size_t i = 0; i < spec_.primary.size(); ++i) spec_.primary[i].checked = i == pick;
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Keyboard order: the option rows, the changeable pin rows, Cancel, Done.
    std::vector<int> FocusOrder() const {
        std::vector<int> order;
        for (size_t i = 0; i < spec_.primary.size(); ++i) order.push_back(kPrimaryBase + static_cast<int>(i));
        for (size_t i = 0; i < spec_.pins.size(); ++i)
            if (!spec_.pins[i].locked) order.push_back(kPinBase + static_cast<int>(i));
        order.push_back(kCancel);
        order.push_back(kDone);
        return order;
    }

    void MoveFocus(int direction) {
        const std::vector<int> order = FocusOrder();
        auto it = std::find(order.begin(), order.end(), visual_.focus);
        int index = it == order.end() ? 0 : static_cast<int>(it - order.begin());
        const int count = static_cast<int>(order.size());
        index = (index + direction + count) % count;
        visual_.focus = order[static_cast<size_t>(index)];
        visual_.show_focus = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, scale_);
        DrawDialog(compositor_, painter_, theme, spec_, layout_, formats_, visual_, high_contrast);
        EndSurface(compositor_);
    }

    int Hit(LPARAM lparam) const {
        return HitTest(layout_, spec_, static_cast<float>(GET_X_LPARAM(lparam)),
                       static_cast<float>(GET_Y_LPARAM(lparam)));
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            SizeToContent();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            const LRESULT hit = BorderlessHitTest(hwnd_, lparam, layout_.title_bar, layout_.close);
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT ? HTCLIENT : hit;
        }
        case WM_SIZE:
            if (!sizing_ && compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
            SizeToContent();
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(lparam);
            if (next != visual_.hover) {
                visual_.hover = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            visual_.hover = kNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            visual_.pressed = Hit(lparam);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(lparam);
            const int pressed = visual_.pressed;
            visual_.pressed = kNone;
            ReleaseCapture();
            if (hit == pressed && hit != kNone) {
                if (hit != kClose && hit != kDone && hit != kCancel) visual_.focus = hit;
                Activate(hit);
            }
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wparam) {
            case VK_ESCAPE: Complete(false); return 0;
            case VK_TAB: MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0;
            case VK_UP:
            case VK_LEFT: MoveFocus(-1); return 0;
            case VK_DOWN:
            case VK_RIGHT: MoveFocus(1); return 0;
            case VK_SPACE: Activate(visual_.focus); return 0;
            case VK_RETURN:
                // Enter confirms the sheet, unless Cancel has the focus.
                Activate(visual_.focus == kCancel ? kCancel : kDone);
                return 0;
            }
            break;
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    Formats formats_;
    Layout layout_;
    Visual visual_;
    ToolbarCustomizeSpec spec_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool accepted_ = false;
    bool done_ = false;
    bool sizing_ = false;
};

} // namespace

bool ShowToolbarCustomizeDialog(HWND owner, ToolbarCustomizeSpec& spec, bool dark, D2D1_COLOR_F accent) {
    CustomizeWindow window;
    return window.Show(owner, spec, dark, accent);
}

} // namespace pulse::ui
