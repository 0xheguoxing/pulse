#include "notification_toast.h"

#include "snack_style.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
bool Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

// One line of capsule text, centered in `height`, cut with an ellipsis.
ComPtr<IDWriteTextLayout> Line(Compositor& compositor, std::wstring_view text, IDWriteTextFormat* format,
                               float max_width, float height, bool semibold) {
    ComPtr<IDWriteTextLayout> layout;
    IDWriteFactory2* const factory = compositor.DwriteFactory();
    if (!factory || !format ||
        FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                         (std::max)(1.0f, max_width), (std::max)(1.0f, height), &layout)))
        return {};
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (semibold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.get());
    }
    return layout;
}

float LineWidth(IDWriteTextLayout* layout) {
    DWRITE_TEXT_METRICS metrics{};
    return layout && SUCCEEDED(layout->GetMetrics(&metrics)) ? std::ceil(metrics.widthIncludingTrailingWhitespace) : 0.0f;
}

std::wstring_view SnackGlyph(SnackIcon icon) {
    switch (icon) {
    case SnackIcon::Window: return L"\xE8A7";   // OpenInNewWindow
    case SnackIcon::Undo: return L"\xE7A7";     // Undo
    case SnackIcon::Merge: return L"\xE7C4";    // TaskView: windows into one
    case SnackIcon::Suggest: return L"\xEA80";  // Lightbulb
    case SnackIcon::Tab:
    default: return L"\xE8B7";                  // Folder
    }
}
}
void NotificationToast::Show(HWND owner, std::wstring title, std::wstring message, bool persistent, UINT action_message) {
    title_ = std::move(title);
    message_ = std::move(message);
    persistent_ = persistent;
    top_center_ = false;
    snack_ = false;
    actions_.clear();
    action_rects_.clear();
    key_.clear();
    pressed_action_ = -1;
    duration_ = 6000;
    kind_ = persistent ? fluent::InfoBarKind::Warning : fluent::InfoBarKind::Informational;
    action_message_ = action_message;
    visible_ = true;
    hovered_ = pressed_close_ = pressed_card_ = false;
    born_ = last_tick_ = GetTickCount64();
    remaining_ = 6000;
    SetTimer(owner, timer, 16, nullptr);
    InvalidateRect(owner, nullptr, FALSE);
}
void NotificationToast::ShowSnack(HWND owner, SnackIcon icon, std::wstring text, std::wstring detail,
                                  std::vector<SnackAction> actions, std::wstring key, UINT duration_ms) {
    Show(owner, std::move(text), std::move(detail), false);
    snack_ = true;
    icon_ = icon;
    if (actions.size() > 2) actions.resize(2);
    actions_ = std::move(actions);
    action_rects_.assign(actions_.size(), D2D1_RECT_F{});
    key_ = std::move(key);
    duration_ = remaining_ = (std::max)(UINT{1500}, duration_ms);
}
void NotificationToast::ShowError(HWND owner, std::wstring title, std::wstring message) {
    Show(owner, std::move(title), std::move(message), false);
    top_center_ = true;
    kind_ = fluent::InfoBarKind::Error;
}
void NotificationToast::Dismiss(HWND owner) {
    visible_ = false;
    pressed_close_ = pressed_card_ = hovered_ = false;
    pressed_action_ = -1;
    KillTimer(owner, timer);
    InvalidateRect(owner, nullptr, FALSE);
}
bool NotificationToast::HandleMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { KillTimer(owner, timer); return false; }
    if (!visible_) return false;
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        Dismiss(owner);
        return true;
    }
    if (message == WM_TIMER && wparam == timer) {
        const auto now = GetTickCount64();
        if (!persistent_ && !hovered_) {
            const auto elapsed = now - last_tick_;
            remaining_ = elapsed >= remaining_ ? 0 : remaining_ - elapsed;
            if (!remaining_) { Dismiss(owner); return true; }
        }
        last_tick_ = now;
        if (now - born_ >= 180) {
            if (persistent_) KillTimer(owner, timer);
            else SetTimer(owner, timer, snack_ ? 100 : 200, nullptr);  // the capsule's line runs down smoothly
        }
        InvalidateRect(owner, nullptr, FALSE);
        return true;
    }
    const float x = static_cast<float>(GET_X_LPARAM(lparam));
    const float y = static_cast<float>(GET_Y_LPARAM(lparam));
    if (message == WM_MOUSEMOVE) {
        const bool hovered = Contains(card_, x, y);
        if (hovered != hovered_) {
            hovered_ = hovered;
            last_tick_ = GetTickCount64();
            InvalidateRect(owner, nullptr, FALSE);
        }
        if (hovered_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, owner, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(owner, nullptr, FALSE);
        }
        if (pressed_card_ || hovered_) return true;
    } else if (message == WM_MOUSELEAVE) {
        hovered_ = false;
        last_tick_ = GetTickCount64();
    } else if (message == WM_LBUTTONDOWN && Contains(card_, x, y)) {
        pressed_card_ = true;
        pressed_close_ = Contains(close_, x, y);
        pressed_action_ = -1;
        for (size_t i = 0; snack_ && i < action_rects_.size(); ++i)
            if (Contains(action_rects_[i], x, y)) pressed_action_ = static_cast<int>(i);
        SetCapture(owner);
        InvalidateRect(owner, nullptr, FALSE);
        return true;
    } else if (message == WM_LBUTTONUP && (pressed_card_ || Contains(card_, x, y))) {
        const bool close = pressed_close_ && Contains(close_, x, y);
        const int pressed_action = pressed_action_;
        const bool action = !snack_ && pressed_card_ && !pressed_close_ && Contains(card_, x, y) && action_message_;
        pressed_close_ = pressed_card_ = false;
        pressed_action_ = -1;
        if (GetCapture() == owner) ReleaseCapture();
        if (close) Dismiss(owner);
        else if (action) {
            PostMessageW(owner, action_message_, 0, 0);
            Dismiss(owner);
        } else if (snack_ && pressed_action >= 0 && static_cast<size_t>(pressed_action) < actions_.size() &&
                   Contains(action_rects_[static_cast<size_t>(pressed_action)], x, y)) {
            // The capsule acts on its buttons only (M7i): the rest of it is text.
            if (const UINT post = actions_[static_cast<size_t>(pressed_action)].message) PostMessageW(owner, post, 0, 0);
            Dismiss(owner);
        } else {
            InvalidateRect(owner, nullptr, FALSE);
        }
        return true;
    } else if (message == WM_CAPTURECHANGED) {
        pressed_close_ = pressed_card_ = false;
        pressed_action_ = -1;
    } else if ((message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) &&
               Contains(card_, x, y)) {
        return true;
    }
    return false;
}
void NotificationToast::Draw(Compositor& compositor, const Theme& theme, float scale, bool high_contrast) {
    if (!visible_) return;
    if (snack_) {
        DrawSnack(compositor, theme, scale, high_contrast);
        return;
    }
    painter_.SetCompositor(&compositor);
    painter_.SetScale(scale);
    if (!painter_.BeginFrame(theme, high_contrast)) return;
    const float margin = 16.0f * scale;
    const float width = std::min(420.0f * scale, compositor.Width() - 2 * margin);
    if (width < 100.0f * scale) return;
    float body_height = 40.0f * scale;
    const float close_inset = top_center_ ? 12.0f : 2.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(compositor.DwriteFactory()->CreateTextLayout(message_.c_str(),
            static_cast<UINT32>(message_.size()), compositor.SmallFormat(),
            width - (82 + close_inset) * scale, 400 * scale, &layout))) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) body_height = metrics.height;
    }
    const float height = std::min(std::max(72 * scale, body_height + 44 * scale),
        std::max(72 * scale, compositor.Height() - 110 * scale));
    const float age = std::min(1.0f, static_cast<float>(GetTickCount64() - born_) / 180.0f);
    const float slide = high_contrast ? 0 : 12 * scale * (1 - age) * (1 - age);
    const float right = top_center_ ? (compositor.Width() + width) * 0.5f : compositor.Width() - margin;
    const float top = top_center_ ? 64 * scale - slide : compositor.Height() - 48 * scale + slide - height;
    card_ = D2D1::RectF(right - width, top, right, top + height);
    const float center = (card_.top + card_.bottom) * 0.5f;
    close_ = D2D1::RectF(right - (28 + close_inset) * scale, center - 14 * scale,
        right - close_inset * scale, center + 14 * scale);
    fluent::InfoBarSpec spec;
    spec.bounds = card_;
    spec.title = title_;
    spec.message = message_;
    spec.kind = kind_;
    spec.show_icon_background = !top_center_;
    spec.close_inset = close_inset;
    spec.show_close = true;
    POINT cursor{};
    if (GetCursorPos(&cursor) && ScreenToClient(compositor.Hwnd(), &cursor))
        spec.state.hovered = Contains(close_, static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    painter_.DrawInfoBar(spec);
}

void NotificationToast::DrawSnack(Compositor& compositor, const Theme& theme, float scale, bool high_contrast) {
    painter_.SetCompositor(&compositor);
    painter_.SetScale(scale);
    if (!painter_.BeginFrame(theme, high_contrast)) return;
    ID2D1DeviceContext* const dc = compositor.Dc();
    if (!dc) return;
    const SnackPalette pal = MakeSnackPalette(theme, high_contrast);
    const auto px = [scale](float dip) { return dip * scale; };
    const float window_w = static_cast<float>(compositor.Width());
    const float window_h = static_cast<float>(compositor.Height());
    const float height = px(36.0f);
    const float max_width = window_w - px(32.0f);
    if (max_width < px(160.0f)) return;
    IDWriteTextFormat* const text_format = compositor.TextFormat();
    IDWriteTextFormat* const small_format = compositor.SmallFormat();

    // Measure: [pad icon gap] title [· detail] [gap | gap] buttons [key] [close pad].
    const std::wstring detail = message_.empty() ? std::wstring() : L" \u00B7 " + message_;
    auto title = Line(compositor, title_, text_format, max_width, height, true);
    ComPtr<IDWriteTextLayout> detail_line;
    if (!detail.empty()) detail_line = Line(compositor, detail, text_format, max_width, height, false);
    std::vector<ComPtr<IDWriteTextLayout>> labels;
    float buttons_w = 0.0f;
    for (size_t i = 0; i < actions_.size(); ++i) {
        labels.push_back(Line(compositor, actions_[i].label, text_format, max_width, height, i == 0));
        buttons_w += LineWidth(labels.back().get()) + px(16.0f) + (i ? px(2.0f) : 0.0f);
    }
    ComPtr<IDWriteTextLayout> key;
    if (!key_.empty()) key = Line(compositor, key_, small_format, max_width, px(18.0f), true);
    const float lead = px(14.0f + 16.0f + 9.0f);
    const float close_w = px(4.0f + 24.0f + 6.0f);
    const float divider_w = actions_.empty() ? 0.0f : px(10.0f + 1.0f + 4.0f);
    float title_w = LineWidth(title.get());
    float detail_w = LineWidth(detail_line.get());
    float key_w = key.get() ? LineWidth(key.get()) + px(10.0f + 6.0f) : 0.0f;
    const auto total = [&] { return lead + title_w + detail_w + divider_w + buttons_w + key_w + close_w; };
    if (total() > max_width) detail_w = 0.0f;  // the detail goes first
    if (total() > max_width) key_w = 0.0f;     // then the shortcut
    if (total() > max_width) title_w = (std::max)(px(40.0f), title_w - (total() - max_width));
    const float width = (std::min)(max_width, total());

    const float age = (std::min)(1.0f, static_cast<float>(GetTickCount64() - born_) / 180.0f);
    const float slide = high_contrast ? 0.0f : px(10.0f) * (1.0f - age) * (1.0f - age);
    const float left = std::round((window_w - width) * 0.5f);
    const float top = std::round(window_h - px(48.0f) - height + slide);
    card_ = D2D1::RectF(left, top, left + width, top + height);
    const float radius = height * 0.5f;
    const float mid = top + height * 0.5f;

    POINT cursor{};
    float cx = -1.0f, cy = -1.0f;
    if (GetCursorPos(&cursor) && ScreenToClient(compositor.Hwnd(), &cursor)) {
        cx = static_cast<float>(cursor.x);
        cy = static_cast<float>(cursor.y);
    }

    // Acrylic plate in the window's theme, with the flyout shadow.
    DrawSnackPlate(dc, card_, radius, pal, scale, high_contrast);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(pal.text, &brush))) return;
    const auto draw = [&](IDWriteTextLayout* layout, float x, float w, float h, float y, const D2D1_COLOR_F& color) {
        if (!layout || w <= 0.0f) return;
        layout->SetMaxWidth(w);
        layout->SetMaxHeight(h);
        brush->SetColor(color);
        dc->DrawTextLayout(D2D1::Point2F(x, y), layout, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    float x = left + px(14.0f);
    painter_.DrawGlyph(SnackGlyph(icon_), D2D1::RectF(x - px(2.0f), mid - px(10.0f), x + px(18.0f), mid + px(10.0f)),
                       pal.accent);
    x += px(16.0f + 9.0f);
    draw(title.get(), x, title_w, height, top, pal.text);
    x += title_w;
    if (detail_w > 0.0f) {
        draw(detail_line.get(), x, detail_w, height, top, pal.text_secondary);
        x += detail_w;
    }
    if (!actions_.empty()) {
        x += px(10.0f);
        painter_.FillRoundedRect(D2D1::RectF(x, mid - px(8.0f), x + px(1.0f), mid + px(8.0f)), 0.0f, pal.divider);
        x += px(1.0f + 4.0f);
    }
    for (size_t i = 0; i < actions_.size(); ++i) {
        if (i) x += px(2.0f);
        const float label_w = LineWidth(labels[i].get());
        const D2D1_RECT_F button = D2D1::RectF(x, mid - px(13.0f), x + label_w + px(16.0f), mid + px(13.0f));
        action_rects_[i] = button;
        const bool hot = Contains(button, cx, cy) || pressed_action_ == static_cast<int>(i);
        if (i == 0 && !high_contrast) {
            // The first button reads as one: a wash of the accent, deeper under the cursor.
            painter_.FillRoundedRect(button, px(13.0f), hot ? SnackAlpha(pal.accent_wash, pal.accent_wash.a * 1.8f)
                                                            : pal.accent_wash);
        } else if (hot) {
            painter_.FillRoundedRect(button, px(13.0f), pal.hover);
        }
        draw(labels[i].get(), x + px(8.0f), label_w + 1.0f, height, top, i == 0 ? pal.accent : pal.text_secondary);
        x = button.right;
    }
    if (key_w > 0.0f && key.get()) {
        x += px(6.0f);
        const float chip_w = key_w - px(6.0f);
        const D2D1_RECT_F chip = D2D1::RectF(x, mid - px(9.0f), x + chip_w, mid + px(9.0f));
        D2D1_COLOR_F edge = pal.text_secondary;
        edge.a *= 0.6f;
        painter_.StrokeRoundedRect(chip, px(4.0f), edge, px(1.0f));
        draw(key.get(), x + px(5.0f), chip_w - px(10.0f) + 1.0f, px(18.0f), chip.top, pal.text_secondary);
        x = chip.right;
    }
    // Close: a small x in a round hover target.
    close_ = D2D1::RectF(card_.right - px(6.0f + 24.0f), mid - px(12.0f), card_.right - px(6.0f), mid + px(12.0f));
    if (Contains(close_, cx, cy) || pressed_close_) painter_.FillRoundedRect(close_, px(12.0f), pal.hover);
    {
        const float c = (close_.left + close_.right) * 0.5f, arm = px(3.5f);
        brush->SetColor(pal.text_secondary);
        dc->DrawLine(D2D1::Point2F(c - arm, mid - arm), D2D1::Point2F(c + arm, mid + arm), brush.get(), px(1.2f));
        dc->DrawLine(D2D1::Point2F(c + arm, mid - arm), D2D1::Point2F(c - arm, mid + arm), brush.get(), px(1.2f));
    }
    // The time left runs down along the bottom; it stops under the cursor.
    if (!high_contrast && duration_ > 0) {
        const float share = static_cast<float>(remaining_) / static_cast<float>(duration_);
        const float inset = radius * 0.75f;
        const float span = (card_.right - card_.left - 2.0f * inset) * (std::clamp)(share, 0.0f, 1.0f);
        const D2D1_COLOR_F line = pal.progress;  // the theme color, as set in Settings
        if (span > 0.0f)
            painter_.FillRoundedRect(D2D1::RectF(card_.left + inset, card_.bottom - px(2.5f), card_.left + inset + span,
                                                 card_.bottom - px(1.0f)),
                                     px(0.75f), line);
    }
}
}
