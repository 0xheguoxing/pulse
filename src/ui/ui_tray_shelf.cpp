// ui_tray_shelf.cpp — staging tray shelf (pulse_tray_redesign.html).
//
// The tray panel shows one of three layers, swapped with a short cross-fade
// while the panel height springs to the new layer:
//   strip  (40 dip)  empty tray: icon, 暂存区, "拖入文件，或 Ctrl Shift S"
//   drop   (112 dip) files dragged over the window: 复制暂存 | 移动暂存
//   shelf  (150 dip) fan of the newest items, the release split button that
//                    names its target, ZIP / rename, 送到 chips
// Geometry lives in TrayShelfGeometry (ui_renderer_internal.h), shared with
// the hit test.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "typography.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {

namespace {

using pulse::l10n::StringId;

std::wstring Fill(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, wcslen(key), value);
    return text;
}

D2D1_COLOR_F Rgba(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                        a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float EaseOutCubic(float t) { t = Clamp01(t); const float u = 1.0f - t; return 1.0f - u * u * u; }
// cubic-bezier(.2,1.2,.3,1)-like: a small overshoot that settles.
float EaseOutBack(float t) {
    t = Clamp01(t);
    const float c1 = 1.2f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

struct ShelfPalette {
    D2D1_COLOR_F tray, edge, strong, text, sub, faint;
    D2D1_COLOR_F chip, chip_hot, ib_hot, kbd;
    D2D1_COLOR_F acc, acc2, move, move2, warn, on_acc;
    D2D1_COLOR_F folder;
};

ShelfPalette MakeShelfPalette(bool dark, bool hc, const Theme& theme) {
    ShelfPalette p{};
    p.acc = theme.accent;
    p.acc.a = 1.0f;
    p.acc2 = Mix(p.acc, Rgba(0xFFFFFF), dark ? 0.30f : 0.0f);
    p.move = Rgba(0xD9772F);
    p.move2 = Rgba(0xE08A48);
    p.warn = Rgba(0xF2A93B);
    p.on_acc = Rgba(0xFFFFFF);
    p.folder = Rgba(0xF5B942);
    if (hc) {
        p.tray = theme.surface_flyout;
        p.edge = theme.text;
        p.strong = theme.text;
        p.text = theme.text;
        p.sub = theme.text_secondary;
        p.faint = theme.text_secondary;
        p.chip = theme.fill_hover;
        p.chip_hot = theme.fill_selected;
        p.ib_hot = theme.fill_hover;
        p.kbd = theme.fill_hover;
        return p;
    }
    if (dark) {
        p.tray = Mix(Rgba(0x0F1521), p.acc, 0.02f);
        p.edge = Alpha(p.acc, 0.32f);
        p.strong = Rgba(0xEEF0FF);
        p.text = Rgba(0xCFD5EA);
        p.sub = Rgba(0x8A93AD);
        p.faint = Rgba(0x6F7891);
        p.chip = Rgba(0xFFFFFF, 0.055f);
        p.chip_hot = Alpha(p.acc, 0.26f);
        p.ib_hot = Rgba(0xFFFFFF, 0.07f);
        p.kbd = Rgba(0xFFFFFF, 0.05f);
    } else {
        p.tray = Mix(Rgba(0xF6F7FB), p.acc, 0.035f);
        p.edge = Alpha(p.acc, 0.30f);
        p.strong = Rgba(0x1B1F2A);
        p.text = Rgba(0x2E3442);
        p.sub = Rgba(0x5B6478);
        p.faint = Rgba(0x7B8396);
        p.chip = Rgba(0x000000, 0.05f);
        p.chip_hot = Alpha(p.acc, 0.16f);
        p.ib_hot = Rgba(0x000000, 0.06f);
        p.kbd = Rgba(0x000000, 0.06f);
    }
    return p;
}

// Small vector icons from the prototype (16x16 viewBox).
enum class ShelfIcon { Tray, Up, Down, Zip, Rename, More, Copy, Move };

void DrawShelfIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ShelfIcon icon,
                   const D2D1_RECT_F& box, float px /* drawn edge in pixels */) {
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory.get()) return;
    const float k = px / 16.0f;
    const float ox = (box.left + box.right - px) * 0.5f;
    const float oy = (box.top + box.bottom - px) * 0.5f;
    auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * k, oy + y * k); };
    ComPtr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
    auto line = [&](float x0, float y0, float x1, float y1, float w) {
        dc->DrawLine(P(x0, y0), P(x1, y1), br, w * k, round.get());
    };
    auto rrect = [&](float x, float y, float w, float h, float r, float sw) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + x * k, oy + y * k,
            ox + (x + w) * k, oy + (y + h) * k), r * k, r * k), br, sw * k, round.get());
    };
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts, bool closed, float sw) {
        ComPtr<ID2D1PathGeometry> path;
        factory->CreatePathGeometry(&path);
        if (!path.get()) return;
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        auto it = pts.begin();
        sink->BeginFigure(P(it->x, it->y), D2D1_FIGURE_BEGIN_HOLLOW);
        for (++it; it != pts.end(); ++it) sink->AddLine(P(it->x, it->y));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        dc->DrawGeometry(path.get(), br, sw * k, round.get());
    };
    switch (icon) {
    case ShelfIcon::Tray:
        poly({{2.5f, 9.5f}, {5.5f, 9.5f}, {6.5f, 11.1f}, {9.5f, 11.1f}, {10.5f, 9.5f}, {13.5f, 9.5f}},
             false, 1.5f);
        poly({{2.5f, 9.5f}, {4.1f, 4.3f}, {5.2f, 3.5f}, {10.8f, 3.5f}, {11.9f, 4.3f}, {13.5f, 9.5f},
              {13.5f, 12.5f}, {12.5f, 13.5f}, {3.5f, 13.5f}, {2.5f, 12.5f}}, true, 1.5f);
        break;
    case ShelfIcon::Up:
        poly({{4.0f, 10.0f}, {8.0f, 6.0f}, {12.0f, 10.0f}}, false, 1.7f);
        break;
    case ShelfIcon::Down:
        poly({{4.0f, 6.0f}, {8.0f, 10.0f}, {12.0f, 6.0f}}, false, 1.7f);
        break;
    case ShelfIcon::Zip:
        rrect(3.0f, 1.8f, 10.0f, 12.4f, 1.8f, 1.4f);
        line(8.0f, 2.0f, 8.0f, 3.4f, 1.4f);
        line(8.0f, 4.4f, 8.0f, 5.8f, 1.4f);
        line(8.0f, 6.8f, 8.0f, 8.2f, 1.4f);
        rrect(6.5f, 9.2f, 3.0f, 2.7f, 0.8f, 1.4f);
        break;
    case ShelfIcon::Rename:
        rrect(1.8f, 4.5f, 8.5f, 7.0f, 1.6f, 1.4f);
        line(4.2f, 8.0f, 7.8f, 8.0f, 1.4f);
        line(12.8f, 2.2f, 12.8f, 13.8f, 1.4f);
        line(11.4f, 2.2f, 14.2f, 2.2f, 1.4f);
        line(11.4f, 13.8f, 14.2f, 13.8f, 1.4f);
        break;
    case ShelfIcon::More:
        for (float x : {3.5f, 8.0f, 12.5f})
            dc->FillEllipse(D2D1::Ellipse(P(x, 8.0f), 1.3f * k, 1.3f * k), br);
        break;
    case ShelfIcon::Copy:
        rrect(5.0f, 5.0f, 8.5f, 8.5f, 1.8f, 1.2f);
        poly({{3.0f, 10.5f}, {3.0f, 3.8f}, {3.4f, 2.9f}, {4.3f, 2.5f}, {11.0f, 2.5f}}, false, 1.2f);
        break;
    case ShelfIcon::Move:
        line(2.5f, 8.0f, 11.5f, 8.0f, 1.2f);
        poly({{8.5f, 4.8f}, {11.7f, 8.0f}, {8.5f, 11.2f}}, false, 1.2f);
        line(13.5f, 3.0f, 13.5f, 13.0f, 1.2f);
        break;
    }
}

// Text in a box: one line, end ellipsis, centered vertically.
void ShelfText(ID2D1DeviceContext* dc, IDWriteFactory2* factory, IDWriteTextFormat* fmt,
               ID2D1SolidColorBrush* br, const std::wstring& text, const D2D1_RECT_F& rc,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!dc || !factory || !fmt || text.empty() || w <= 1.0f || h <= 1.0f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, w, h,
                                         &layout)) || !layout.get())
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
    layout->SetTrimming(&trimming, ellipsis.get());
    dc->DrawTextLayout(D2D1::Point2F(rc.left, rc.top), layout.get(), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

} // namespace

IDWriteTextFormat* MainRenderer::ShelfFormat(int role) const {
    if (!compositor_ || role < 0 || role >= kShelfFormatCount) return nullptr;
    if (shelf_formats_scale_ != scale_) {
        for (auto& f : shelf_formats_) f.reset();
        shelf_formats_scale_ = scale_;
    }
    if (!shelf_formats_[role].get()) {
        struct Spec { float size; DWRITE_FONT_WEIGHT weight; };
        static constexpr Spec kSpecs[kShelfFormatCount] = {
            {13.0f, DWRITE_FONT_WEIGHT_BOLD},       // 0 title / half label
            {12.0f, DWRITE_FONT_WEIGHT_NORMAL},     // 1 hint, chips
            {12.5f, DWRITE_FONT_WEIGHT_BOLD},       // 2 release label
            {14.0f, DWRITE_FONT_WEIGHT_BOLD},       // 3 meta count
            {11.5f, DWRITE_FONT_WEIGHT_NORMAL},     // 4 sub lines, 送到
            {10.5f, DWRITE_FONT_WEIGHT_BOLD},       // 5 badges
            {10.5f, DWRITE_FONT_WEIGHT_NORMAL},     // 6 kbd
        };
        typography::CreateTextFormat(compositor_->DwriteFactory(),
            {typography::FontRole::Text, kSpecs[role].size * scale_, kSpecs[role].weight},
            &shelf_formats_[role]);
        if (shelf_formats_[role].get()) {
            shelf_formats_[role]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            shelf_formats_[role]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    return shelf_formats_[role].get() ? shelf_formats_[role].get() : compositor_->SmallFormat();
}

float MainRenderer::ShelfMeasure(const std::wstring& text, bool compact_text) const {
    if (!compositor_) return 0.0f;
    return MeasureTextWidth(compositor_->DwriteFactory(), ShelfFormat(compact_text ? 1 : 0), text);
}

void MainRenderer::DrawTrayShelf(const WindowViewModel& vm, const D2D1_RECT_F& rc,
                                 const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const TrayDeckView& d = vm.tray_deck;
    const ShelfPalette p = MakeShelfPalette(vm.dark, IsHighContrast(), theme);
    const float s = scale_;
    const float radius = 12.0f * s;

    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.tray, &br);
    if (!br.get()) return;
    dc->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), br.get());
    // Drop target: the edge brightens while files hover the tray itself.
    const D2D1_COLOR_F edge = vm.tray_drop ? Alpha(p.acc2, 0.75f) : p.edge;
    br->SetColor(edge);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f,
        rc.right - 0.5f, rc.bottom - 0.5f), radius - 0.5f, radius - 0.5f), br.get(), 1.0f);

    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    if (factory.get())
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rc, radius, radius), &clip);
    if (clip.get())
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.get()), nullptr);

    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const float swap_ms = Clamp01(d.swap) * 410.0f;
    if (d.prev_stage >= 0 && d.prev_stage != d.stage && swap_ms < 130.0f) {
        const float t = swap_ms / 130.0f;
        const float a = 1.0f - EaseOutCubic(t);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -4.0f * s * t) * old);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
        DrawTrayShelfLayer(vm, d.prev_stage, rc, theme);
        dc->PopLayer();
        dc->SetTransform(old);
    }
    const float tin = d.prev_stage >= 0 ? Clamp01((swap_ms - 60.0f) / 280.0f) : 1.0f;
    if (tin > 0.0f) {
        const float e = EaseOutBack(tin);
        const float scale = 0.985f + 0.015f * e;
        const D2D1_POINT_2F origin = D2D1::Point2F((rc.left + rc.right) * 0.5f, rc.top);
        const bool layered = tin < 1.0f;
        if (layered) {
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, origin) *
                             D2D1::Matrix3x2F::Translation(0.0f, 10.0f * s * (1.0f - e)) * old);
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), EaseOutCubic(tin)), nullptr);
        }
        DrawTrayShelfLayer(vm, d.stage, rc, theme);
        if (layered) {
            dc->PopLayer();
            dc->SetTransform(old);
        }
    }
    if (clip.get()) dc->PopLayer();
}

void MainRenderer::DrawTrayShelfLayer(const WindowViewModel& vm, int stage, const D2D1_RECT_F& rc,
                                      const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* dw = compositor_->DwriteFactory();
    const TrayDeckView& d = vm.tray_deck;
    const ShelfPalette p = MakeShelfPalette(vm.dark, IsHighContrast(), theme);
    const float s = scale_;
    const TrayShelfGeom g = TrayShelfGeometry(d, rc, s,
        [this](const std::wstring& t, bool compact_text) { return ShelfMeasure(t, compact_text); });

    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.text, &br);
    if (!br.get()) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br.get(); };
    auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), B(c));
    };
    auto hot = [&](TrayShelfHit h, int sub = -1) {
        return IsHovered(vm, HitTestResult::TrayShelf, static_cast<int>(h), sub);
    };
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);

    if (stage == static_cast<int>(TrayStage::Strip)) {
        DrawShelfIcon(dc, B(p.acc2), ShelfIcon::Tray, g.strip_icon, 16.0f * s);
        ShelfText(dc, dw, ShelfFormat(0), B(p.acc2), l10n::Get(StringId::TrayShelfTitle), g.strip_title);
        ShelfText(dc, dw, ShelfFormat(1), B(p.faint), l10n::Get(StringId::TrayStripHint), g.strip_hint);
        if (g.strip_kbd.right > g.strip_kbd.left) {
            fill(g.strip_kbd, 4.0f * s, p.kbd);
            ShelfText(dc, dw, ShelfFormat(6), B(p.sub), l10n::Get(StringId::TrayStripKbd), g.strip_kbd,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        return;
    }

    if (stage == static_cast<int>(TrayStage::Drop)) {
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1StrokeStyle> dash;
        if (factory.get()) {
            const float dashes[] = {3.0f, 2.5f};
            factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT,
                D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                D2D1_DASH_STYLE_CUSTOM, 0.0f), dashes, 2, &dash);
        }
        for (int i = 0; i < 2; ++i) {
            const bool mv = i == 1;
            // mode_t follows the active half while dragging (0 copy, 1 move).
            const float on = mv ? Clamp01(d.mode_t) : 1.0f - Clamp01(d.mode_t);
            const D2D1_RECT_F h = g.half[i];
            const float cx = (h.left + h.right) * 0.5f, cy = (h.top + h.bottom) * 0.5f;
            const float sc = 1.0f + 0.035f * EaseOutBack(on);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(cx, cy)) * old);
            const D2D1_COLOR_F on_fill = mv ? Rgba(0xD9772F, 0.20f) : Alpha(p.acc, 0.22f);
            const D2D1_COLOR_F on_edge = mv ? p.move2 : p.acc2;
            const D2D1_COLOR_F off_edge = Alpha(p.acc2, 0.28f);
            const float r = 10.0f * s;
            const D2D1_RECT_F inset = D2D1::RectF(h.left + 0.75f * s, h.top + 0.75f * s,
                                                  h.right - 0.75f * s, h.bottom - 0.75f * s);
            if (on > 0.01f) fill(h, r, Alpha(on_fill, on));
            if (on < 0.99f)
                dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, r, r), B(Alpha(off_edge, 1.0f - on)),
                                         1.5f * s, dash.get());
            if (on > 0.01f)
                dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, r, r), B(Alpha(on_edge, on)), 1.5f * s);
            const D2D1_COLOR_F icon_c = Mix(p.sub, mv ? Rgba(0xFFC89E) : Rgba(0xC6CEFF), on);
            const D2D1_COLOR_F b_c = Mix(p.text, vm.dark ? Rgba(0xFFFFFF) : p.strong, on);
            const float block = 20.0f * s + 5.0f * s + 17.0f * s + 5.0f * s + 14.0f * s;
            float y = cy - block * 0.5f;
            DrawShelfIcon(dc, B(icon_c), mv ? ShelfIcon::Move : ShelfIcon::Copy,
                          D2D1::RectF(cx - 10.0f * s, y, cx + 10.0f * s, y + 20.0f * s), 20.0f * s);
            y += 25.0f * s;
            ShelfText(dc, dw, ShelfFormat(0), B(b_c),
                      l10n::Get(mv ? StringId::TrayDropMoveTitle : StringId::TrayDropCopyTitle),
                      D2D1::RectF(h.left + 4.0f * s, y, h.right - 4.0f * s, y + 17.0f * s),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 22.0f * s;
            ShelfText(dc, dw, ShelfFormat(4), B(p.faint),
                      l10n::Get(mv ? StringId::TrayDropOrShift : StringId::TrayDropDefault),
                      D2D1::RectF(h.left + 4.0f * s, y, h.right - 4.0f * s, y + 14.0f * s),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            dc->SetTransform(old);
        }
        return;
    }

    // ---------------------------------------------------------------- shelf
    // r1: fan.
    {
        const D2D1_RECT_F f = g.fan;
        const float b = Clamp01(d.bump);
        float bump = 0.0f;
        if (b > 0.0f && b < 1.0f) bump = b < 0.3f ? EaseOutCubic(b / 0.3f) : 1.0f - EaseOutCubic((b - 0.3f) / 0.7f);
        const float bs = 1.0f + 0.16f * bump;
        const D2D1_MATRIX_3X2_F fan_m = D2D1::Matrix3x2F::Scale(bs, bs,
            D2D1::Point2F((f.left + f.right) * 0.5f, (f.top + f.bottom) * 0.5f)) *
            D2D1::Matrix3x2F::Translation(0.0f, -3.0f * s * bump) * old;
        const int n = static_cast<int>(d.fan.size());
        const float dw_ = 26.0f * s, dh = 33.0f * s;
        const float spread = EaseOutBack(d.fan_spread);
        for (int k = 0; k < n; ++k) {
            const TrayCardView& c = d.fan[static_cast<size_t>(k)];
            float r = static_cast<float>(k - (n - 1)) * 9.0f;
            float tx = r * 0.7f;
            if (n >= 2 && k == 0) {  // :nth-child(1) spreads left
                tx += (-10.0f - tx) * spread;
                r += (-14.0f - r) * spread;
            } else if (n == 3 && k == 2) {  // :nth-child(3) spreads right
                tx += (10.0f - tx) * spread;
                r += (12.0f - r) * spread;
            }
            const D2D1_RECT_F doc = D2D1::RectF(f.left + 12.0f * s, f.top + 3.0f * s,
                                                f.left + 12.0f * s + dw_, f.top + 3.0f * s + dh);
            const D2D1_POINT_2F center = D2D1::Point2F((doc.left + doc.right) * 0.5f, (doc.top + doc.bottom) * 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(r, center) *
                             D2D1::Matrix3x2F::Translation(tx * s, 0.0f) * fan_m);
            const float rr = (c.is_dir ? 3.0f : 4.0f) * s;
            // Shadow: 0 3px 10px rgba(0,0,0,.4).
            for (int l = 3; l >= 1; --l) {
                const float grow = static_cast<float>(l) * 1.4f * s;
                fill(D2D1::RectF(doc.left - grow, doc.top - grow + 3.0f * s, doc.right + grow,
                                 doc.bottom + grow + 3.0f * s), rr + grow,
                     Rgba(0x000000, (vm.dark ? 0.16f : 0.07f) * (1.0f - 0.25f * (l - 1))));
            }
            if (c.is_dir) {
                fill(doc, rr, Rgba(0xE8B64A));
                fill(D2D1::RectF(doc.left, doc.top, doc.right, doc.top + dh * 0.45f), rr, Rgba(0xF0C35A));
                fill(D2D1::RectF(doc.left + 2.0f * s, doc.top - 2.5f * s, doc.left + 11.0f * s, doc.top + 3.0f * s),
                     2.0f * s, Rgba(0xE0A93A));
            } else {
                // Paper with a folded corner and a type strip (.doc).
                fill(doc, rr, Rgba(0xF4F5F7));
                fill(D2D1::RectF(doc.left, doc.top + dh * 0.5f, doc.right, doc.bottom), rr, Rgba(0xE6E8EC));
                const D2D1_RECT_F art = D2D1::RectF(doc.left + 3.0f * s, doc.top + 3.0f * s,
                                                    doc.right - 3.0f * s, doc.bottom - 16.0f * s);
                bool drew = false;
                if (!c.missing) {
                    drew = thumbnail_cache_.Draw(dc, D2D1::RectF(doc.left + 1.0f * s, doc.top + 1.0f * s,
                        doc.right - 1.0f * s, doc.bottom - 1.0f * s), c.path, c.attrs, 64, 0, 0, 0, 1.0f) ==
                        PreviewDrawResult::Bitmap;
                }
                if (!drew) {
                    const float icon = std::round(14.0f * s);
                    if (ID2D1Bitmap* bitmap = icon_cache_.BitmapFor(c.missing ? L"" : c.path, c.name, false,
                                                                    c.attrs, icon)) {
                        const float ix = (art.left + art.right - icon) * 0.5f;
                        const float iy = (art.top + art.bottom - icon) * 0.5f + 1.0f * s;
                        const D2D1_RECT_F dest = D2D1::RectF(ix, iy, ix + icon, iy + icon);
                        dc->DrawBitmap(bitmap, &dest, c.missing ? 0.45f : 1.0f,
                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                    }
                    const std::wstring chip = TypeChipLabel(c.name, false);
                    const D2D1_RECT_F strip = D2D1::RectF(doc.left + 3.0f * s, doc.bottom - 14.0f * s,
                                                          doc.right - 3.0f * s, doc.bottom - 4.0f * s);
                    fill(strip, 2.0f * s, Rgba(TypeChipRgb(chip)));
                    // Folded corner.
                    fill(D2D1::RectF(doc.right - 8.0f * s, doc.top, doc.right, doc.top + 8.0f * s), 2.0f * s,
                         Rgba(0xC3C6CD));
                    if (chip.size() <= 4) {
                        ShelfText(dc, dw, ShelfFormat(5), B(Rgba(0xFFFFFF)), chip,
                                  D2D1::RectF(strip.left - 2.0f * s, strip.top - 1.0f * s, strip.right + 2.0f * s,
                                              strip.bottom + 1.0f * s), DWRITE_TEXT_ALIGNMENT_CENTER);
                    }
                }
            }
        }
        dc->SetTransform(fan_m);
        // Count badge, ringed in the tray colour.
        const std::wstring count = std::to_wstring(std::max(0, d.total_count - d.stale_count));
        const float tw = MeasureTextWidth(dw, ShelfFormat(5), count);
        const float bw = std::max(20.0f * s, tw + 12.0f * s);
        const D2D1_RECT_F badge = D2D1::RectF(f.right - bw, f.bottom - 20.0f * s, f.right, f.bottom);
        const float ring = 2.5f * s;
        fill(D2D1::RectF(badge.left - ring, badge.top - ring, badge.right + ring, badge.bottom + ring),
             10.0f * s + ring, p.tray);
        fill(badge, 10.0f * s, Mix(p.acc, p.move, d.mode_t));
        ShelfText(dc, dw, ShelfFormat(5), B(p.on_acc), count, badge, DWRITE_TEXT_ALIGNMENT_CENTER);
        dc->SetTransform(old);
    }
    // r1: meta.
    {
        const D2D1_RECT_F m = g.meta;
        const float y = m.top + 4.0f * s;
        const std::wstring count = Fill(l10n::Get(StringId::TrayItemsFormat), L"{n}", std::to_wstring(std::max(0, d.total_count - d.stale_count)));
        const float cw = std::min(m.right - m.left, MeasureTextWidth(dw, ShelfFormat(3), count) + 1.0f);
        ShelfText(dc, dw, ShelfFormat(3), B(p.strong), count, D2D1::RectF(m.left, y, m.left + cw, y + 20.0f * s));
        if (d.total_size > 0)
            ShelfText(dc, dw, ShelfFormat(1), B(p.sub), pulse::format::ByteSize(d.total_size, true),
                      D2D1::RectF(m.left + cw + 6.0f * s, y + 1.0f * s, m.right, y + 20.0f * s));
        const D2D1_RECT_F line_b = D2D1::RectF(m.left, y + 21.0f * s, m.right, y + 37.0f * s);
        if (d.stale_count > 0)
            ShelfText(dc, dw, ShelfFormat(4), B(p.warn),
                      Fill(l10n::Get(StringId::TrayStaleFormat), L"{n}", std::to_wstring(d.stale_count)), line_b);
        else
            ShelfText(dc, dw, ShelfFormat(4), B(p.faint), d.sources_text, line_b);
    }
    auto icon_button = [&](const D2D1_RECT_F& r, ShelfIcon icon, bool is_hot, float px) {
        if (is_hot) fill(r, 8.0f * s, p.ib_hot);
        DrawShelfIcon(dc, B(is_hot ? p.strong : p.sub), icon, r, px);
    };
    if (g.has_expand) icon_button(g.expand, ShelfIcon::Up, hot(TrayShelfHit::Expand), 14.0f * s);
    icon_button(g.more, ShelfIcon::More, hot(TrayShelfHit::More), 16.0f * s);

    // r2: split button.
    {
        const D2D1_RECT_F sp = g.split;
        const float r = 9.0f * s;
        const bool usable = !d.target.empty();
        const D2D1_COLOR_F base = Mix(p.acc, p.move, d.mode_t);
        // 0 4px 14px -6px glow under the button.
        for (int l = 0; l < 3; ++l) {
            const float off = (3.0f + 1.5f * static_cast<float>(l)) * s;
            const float inset = (4.0f - 1.0f * static_cast<float>(l)) * s;
            fill(D2D1::RectF(sp.left + inset, sp.top + off, sp.right - inset, sp.bottom + off), r,
                 Alpha(base, (usable ? 0.16f : 0.06f) * (1.0f - 0.3f * static_cast<float>(l))));
        }
        fill(sp, r, usable ? base : Alpha(base, 0.45f));
        // inset 0 1px 0 rgba(255,255,255,.14).
        dc->DrawLine(D2D1::Point2F(sp.left + r * 0.6f, sp.top + 0.5f * s),
                     D2D1::Point2F(sp.right - r * 0.6f, sp.top + 0.5f * s), B(Rgba(0xFFFFFF, 0.14f)), 1.0f * s);
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        if (factory.get()) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(sp, r, r), &clip);
        if (clip.get()) dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.get()), nullptr);
        if (usable && hot(TrayShelfHit::Release)) fill(g.go, 0.0f, Rgba(0xFFFFFF, 0.12f));
        if (hot(TrayShelfHit::ReleaseMenu)) fill(g.dd, 0.0f, Rgba(0xFFFFFF, 0.12f));
        if (clip.get()) dc->PopLayer();
        dc->DrawLine(D2D1::Point2F(g.dd.left, g.dd.top), D2D1::Point2F(g.dd.left, g.dd.bottom),
                     B(Rgba(0xFFFFFF, 0.22f)), 1.0f);
        DrawShelfIcon(dc, B(p.on_acc), ShelfIcon::Down, g.dd, 14.0f * s);
        if (!d.zipping) {
            std::wstring label = usable
                ? Fill(l10n::Get(d.move_mode ? StringId::TrayMoveToFormat : StringId::TrayCopyToFormat),
                       L"{name}", d.target)
                : l10n::Get(StringId::TrayNoTarget);
            const float lt = Clamp01(d.label_t);
            const float e = EaseOutBack(lt);
            const D2D1_RECT_F lr = D2D1::RectF(g.go.left + 8.0f * s, g.go.top + 6.0f * s * (1.0f - e),
                                               g.go.right - 8.0f * s, g.go.bottom + 6.0f * s * (1.0f - e));
            ShelfText(dc, dw, ShelfFormat(2), B(Alpha(p.on_acc, (usable ? 1.0f : 0.75f) * EaseOutCubic(lt))),
                      label, lr, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (d.conflicts > 0 && usable) {
            const std::wstring c = Fill(l10n::Get(StringId::TrayConflictsFormat), L"{n}", std::to_wstring(d.conflicts));
            const float cw = MeasureTextWidth(dw, ShelfFormat(5), c) + 12.0f * s;
            const D2D1_RECT_F cb = D2D1::RectF(sp.right + 5.0f * s - cw, sp.top - 8.0f * s, sp.right + 5.0f * s,
                                               sp.top + 8.0f * s);
            const float ring = 2.5f * s;
            fill(D2D1::RectF(cb.left - ring, cb.top - ring, cb.right + ring, cb.bottom + ring), 8.0f * s + ring, p.tray);
            fill(cb, 8.0f * s, p.warn);
            ShelfText(dc, dw, ShelfFormat(5), B(Rgba(0x2A1800)), c, cb, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    // r2: ZIP (ring while compressing) and rename.
    if (d.zipping) {
        const D2D1_POINT_2F c = D2D1::Point2F((g.zip.left + g.zip.right) * 0.5f, (g.zip.top + g.zip.bottom) * 0.5f);
        const float rad = 6.0f * s, sw = 1.9f * s;
        dc->DrawEllipse(D2D1::Ellipse(c, rad, rad), B(vm.dark ? Rgba(0xFFFFFF, 0.15f) : Rgba(0x000000, 0.12f)), sw);
        const float prog = Clamp01(d.zip_progress);
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1PathGeometry> arc;
        if (factory.get() && prog > 0.005f) factory->CreatePathGeometry(&arc);
        if (arc.get()) {
            ComPtr<ID2D1GeometrySink> sink;
            arc->Open(&sink);
            const float a0 = -3.14159265f * 0.5f;
            const float a1 = a0 + 2.0f * 3.14159265f * std::min(prog, 0.999f);
            sink->BeginFigure(D2D1::Point2F(c.x + rad * std::cos(a0), c.y + rad * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + rad * std::cos(a1), c.y + rad * std::sin(a1)),
                D2D1::SizeF(rad, rad), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                prog > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            ComPtr<ID2D1StrokeStyle> cap;
            factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND),
                                       nullptr, 0, &cap);
            dc->DrawGeometry(arc.get(), B(vm.dark ? Rgba(0xA7B2FF) : p.acc), sw, cap.get());
        }
    } else {
        icon_button(g.zip, ShelfIcon::Zip, hot(TrayShelfHit::Zip), 16.0f * s);
    }
    icon_button(g.rename, ShelfIcon::Rename, hot(TrayShelfHit::Rename), 16.0f * s);

    // r3: 送到 + chips + ＋.
    ShelfText(dc, dw, ShelfFormat(4), B(p.faint), l10n::Get(StringId::TraySendTo), g.send_label);
    for (size_t i = 0; i < g.chips.size(); ++i) {
        const D2D1_RECT_F c = g.chips[i];
        const int dest = g.chip_dest[i];
        const bool is_hot = hot(TrayShelfHit::Chip, dest);
        D2D1_COLOR_F bg = is_hot ? p.chip_hot : p.chip;
        float sc = 1.0f;
        if (d.flash_chip == dest && d.flash_t < 1.0f) {
            const float t = EaseOutCubic(d.flash_t);
            bg = Mix(Alpha(p.acc2, 0.7f), bg, t);
            sc = 1.08f - 0.08f * t;
        }
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc,
            D2D1::Point2F((c.left + c.right) * 0.5f, (c.top + c.bottom) * 0.5f)) * old);
        fill(c, 13.0f * s, bg);
        // Yellow folder (.fo): body 13x10 with a small tab.
        const float fx = c.left + 8.0f * s, fy = (c.top + c.bottom) * 0.5f - 4.0f * s;
        fill(D2D1::RectF(fx, fy - 1.5f * s, fx + 6.0f * s, fy + 2.0f * s), 1.2f * s, Mix(p.folder, Rgba(0xC88A1E), 0.25f));
        fill(D2D1::RectF(fx, fy, fx + 13.0f * s, fy + 10.0f * s), 2.0f * s, p.folder);
        ShelfText(dc, dw, ShelfFormat(1), B(is_hot ? (vm.dark ? Rgba(0xFFFFFF) : p.strong) : p.text),
                  d.dests[static_cast<size_t>(dest)].label,
                  D2D1::RectF(fx + 13.0f * s + 6.0f * s, c.top, c.right - 10.0f * s, c.bottom));
        dc->SetTransform(old);
    }
    {
        const bool is_hot = hot(TrayShelfHit::Pick);
        fill(g.pick, 13.0f * s, is_hot ? p.chip_hot : p.chip);
        ShelfText(dc, dw, ShelfFormat(1), B(is_hot ? p.strong : p.sub), L"\xFF0B", g.pick,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

} // namespace pulse::ui
