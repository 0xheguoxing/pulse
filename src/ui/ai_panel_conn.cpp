// ai_panel_conn.cpp - 管理连接 card (plans/ai/Pulse_AI助手_管理连接.html,
// .win.card-form): layout, drawing and hit rects. Sizes are the design's CSS
// pixels used as DIPs.
#include "ai_panel_conn.h"
#include "ai_panel_draw.h"

#include "typography.h"
#include "ui_compositor.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace pulse::ui {
using namespace ai_draw;
namespace {

constexpr float kPi = 3.14159265358979f;

void Spin(const Ctx& c, D2D1_POINT_2F mid, float d, D2D1_COLOR_F track, D2D1_COLOR_F accent, uint64_t now) {
    // .spin: 2 px ring in line with an accent top, .9 s a turn.
    const float r = d * 0.5f - c.s;
    c.dc->DrawEllipse(D2D1::Ellipse(mid, r, r), c.B(track), 2.0f * c.s);
    const float a = static_cast<float>(now % 900) / 900.0f * 2.0f * kPi - kPi * 0.5f;
    ID2D1Factory* f = nullptr;
    c.dc->GetFactory(&f);
    if (!f) return;
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* k = nullptr;
        if (SUCCEEDED(g->Open(&k))) {
            const float a0 = a - kPi * 0.25f, a1 = a + kPi * 0.25f;
            k->BeginFigure(D2D1::Point2F(mid.x + r * std::cos(a0), mid.y + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            k->AddArc(D2D1::ArcSegment(D2D1::Point2F(mid.x + r * std::cos(a1), mid.y + r * std::sin(a1)),
                                       D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            k->EndFigure(D2D1_FIGURE_END_OPEN);
            k->Close();
            k->Release();
            c.dc->DrawGeometry(g, c.B(accent), 2.0f * c.s);
        }
        g->Release();
    }
    f->Release();
}

template <class T>
void Drop(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

} // namespace

void AiPanelView::ConnGeom::ReleaseFormats() {
    Drop(mono);
    Drop(link);
    Drop(tag);
    fmt_scale = 0.0f;
}

bool AiPanelView::ConnFieldRect(int field, D2D1_RECT_F& rect) const {
    if (!conn_ || !drawn_ || !open_) return false;
    if (field == 0 && conn_->key_place) { rect = conn_->key_text; return true; }
    if (field == 1 && conn_->url_place) { rect = conn_->url_text; return true; }
    return false;
}

float AiPanelView::DrawConnCard(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                                const D2D1_RECT_F& card, float s, uint64_t now_ms, bool draw) {
    if (!conn_ || !conn_->view) return 0.0f;
    ConnGeom& g = *conn_;
    const AiConnView& v = *g.view;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    {
        const float font_scale = typography::UiFontScale();
        if (g.fmt_scale != s || g.fmt_font_scale != font_scale || !g.link) {
            g.ReleaseFormats();
            if (IDWriteFactory2* f = compositor->DwriteFactory()) {
                auto make = [&](float dip, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat*& out) {
                    typography::TextFormatSpec spec;
                    spec.size = dip * s;
                    spec.weight = weight;
                    typography::CreateTextFormat(f, spec, &out);
                };
                make(12.0f, DWRITE_FONT_WEIGHT_MEDIUM, g.link);
                make(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, g.tag);
                // .mask: Consolas 13, letter-spacing .5.
                f->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, 13.0f * s * font_scale, L"", &g.mono);
                if (g.mono) g.mono->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            g.fmt_scale = s;
            g.fmt_font_scale = font_scale;
        }
    }
    if (draw) {
        g.hits.clear();
        g.key_place = g.url_place = false;
    }
    // While the body scrolls it draws inside `body_clip`; what is outside
    // takes no clicks.
    bool clip_on = false;
    D2D1_RECT_F body_clip{};
    auto inside = [&](const D2D1_RECT_F& r) {
        return r.top >= body_clip.top - 0.5f && r.bottom <= body_clip.bottom + 0.5f;
    };
    auto hit = [&](const D2D1_RECT_F& r, AiPanelPart part) {
        if (!draw) return;
        if (clip_on) {
            if (r.bottom <= body_clip.top || r.top >= body_clip.bottom) return;
            g.hits.push_back({D2D1::RectF(r.left, (std::max)(r.top, body_clip.top), r.right,
                                          (std::min)(r.bottom, body_clip.bottom)), part});
            return;
        }
        g.hits.push_back({r, part});
    };
    auto hot = [&](AiPanelPart part) { return vm.hover.part == part; };
    const float left = card.left, right = card.right, top = card.top;
    float y = top;

    // .head: 46 high, padding 0 8 0 6, a 32 px button each side.
    {
        const D2D1_RECT_F back = D2D1::RectF(left + 6.0f * s, top + 7.0f * s, left + 38.0f * s, top + 39.0f * s);
        const D2D1_RECT_F close = D2D1::RectF(right - 40.0f * s, top + 7.0f * s, right - 8.0f * s, top + 39.0f * s);
        if (draw) {
            for (const auto& [r, part, icon] : {std::tuple{back, AiPanelPart::ConnBack, AiIcon::ChevronLeft},
                                                std::tuple{close, AiPanelPart::ConnClose, AiIcon::ChevronDown}}) {
                const bool h = hot(part);
                if (h) c.Fill(r, 8.0f * s, pal.hover);
                c.Icon(icon, r.left + 7.0f * s, r.top + 7.0f * s, 18.0f * s, h ? pal.text : pal.text2);
            }
            c.Text(v.title, formats_.body_b, D2D1::RectF(back.right + 6.0f * s, top, close.left - 6.0f * s, top + 46.0f * s),
                   pal.text);
            c.Fill(D2D1::RectF(left, top + 45.0f * s, right, top + 46.0f * s), 0.0f, pal.line2);
        }
        hit(back, AiPanelPart::ConnBack);
        hit(close, AiPanelPart::ConnClose);
        y += 46.0f * s;
    }
    // .body: padding 12 14 4.
    const float x0 = left + 14.0f * s, x1 = right - 14.0f * s;
    const float foot_h = 55.0f * s;   // .foot: 1 + 10 + 32 + 12
    if (draw && g.card_scroll_max > 0.0f) {
        clip_on = true;
        body_clip = D2D1::RectF(left, y, right, top + g.card_view_h - foot_h);
        c.dc->PushAxisAlignedClip(body_clip, D2D1_ANTIALIAS_MODE_ALIASED);
        y -= g.card_scroll;
    }
    y += 12.0f * s;
    // .svc (transparent in the card): padding 2 0 4; 36 px logo, gap 12.
    {
        y += 2.0f * s;
        const float st_w = c.Measure(v.status_text, formats_.caption);
        const float st_left = x1 - st_w - 13.0f * s;
        if (draw) {
            const D2D1_RECT_F logo = D2D1::RectF(x0, y, x0 + 36.0f * s, y + 36.0f * s);
            c.Fill(logo, 10.0f * s, pal.accent_tint);
            c.Icon(AiIcon::Spark, logo.left + 7.0f * s, logo.top + 7.0f * s, 22.0f * s, pal.accent);
            const float tx = logo.right + 12.0f * s;
            c.Text(v.service, formats_.body_b, D2D1::RectF(tx, y, st_left - 8.0f * s, y + 19.0f * s), pal.text);
            c.Text(v.service_sub, formats_.caption, D2D1::RectF(tx, y + 19.0f * s, st_left - 8.0f * s, y + 36.0f * s),
                   pal.text3);
            const D2D1_COLOR_F sc = v.status == AiConnStatus::Ok ? pal.ok : v.status == AiConnStatus::Bad ? pal.bad : pal.text3;
            const D2D1_COLOR_F dot = v.status == AiConnStatus::Off ? pal.line : sc;
            const float my = y + 18.0f * s;
            c.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(st_left + 3.5f * s, my), 3.5f * s, 3.5f * s), c.B(dot));
            c.Text(v.status_text, formats_.caption, D2D1::RectF(x1 - st_w, my - 9.0f * s, x1 + 2.0f * s, my + 9.0f * s), sc);
        }
        y += 36.0f * s + 4.0f * s;
    }
    // A text field: .inp 38 high, radius 10, padding 0 4 0 12; focus and error rings.
    auto field_box = [&](const D2D1_RECT_F& r, bool focus, bool err) {
        if (!draw) return;
        if (focus || err) c.Fill(Inflate(r, 3.0f * s), 13.0f * s, err ? pal.bad_bg : pal.accent_tint);
        c.Fill(r, 10.0f * s, pal.card);
        c.Stroke(r, 10.0f * s, err ? pal.bad_line : focus ? pal.accent : pal.line);
    };
    auto ghost = [&](const std::wstring& text, float fy, AiPanelPart part) {
        const float w = c.Measure(text, formats_.caption) + 20.0f * s;
        const D2D1_RECT_F r = D2D1::RectF(x1 - 5.0f * s - w, fy + 5.0f * s, x1 - 5.0f * s, fy + 33.0f * s);
        if (draw) {
            c.Fill(r, 7.0f * s, pal.hover);
            TextOptions o;
            o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
            c.Text(text, formats_.caption, r, hot(part) ? pal.text : pal.text2, o);
        }
        hit(r, part);
        return r.left;
    };
    auto label = [&](const std::wstring& text) {
        if (draw) c.Text(text, formats_.small125b, D2D1::RectF(x0 + 2.0f * s, y, x1, y + 17.0f * s), pal.text);
    };
    auto help = [&](const std::wstring& text) {
        y += 8.0f * s;
        TextOptions o;
        o.wrap = true;
        o.line_height = 18.0f * s;
        const D2D1_RECT_F r = D2D1::RectF(x0 + 2.0f * s, y, x1 - 2.0f * s, y + 200.0f * s);
        const float h = c.MeasureWrapped(text, formats_.caption, r.right - r.left, o.line_height);
        if (draw) c.Text(text, formats_.caption, r, pal.text3, o);
        y += h;
    };
    // API Key.
    {
        y += 12.0f * s;
        label(v.key_label);
        const float lw = c.Measure(v.key_link, g.link);
        const D2D1_RECT_F link = D2D1::RectF(x1 - 2.0f * s - lw, y - 2.0f * s, x1, y + 19.0f * s);
        if (draw) {
            const D2D1_COLOR_F lc = hot(AiPanelPart::ConnLink) ? Mix(pal.accent, pal.text, 0.25f) : pal.accent;
            c.Text(v.key_link, g.link, D2D1::RectF(link.left, y, link.right + 2.0f * s, y + 17.0f * s), lc);
        }
        hit(link, AiPanelPart::ConnLink);
        y += 24.0f * s;
        const D2D1_RECT_F box = D2D1::RectF(x0, y, x1, y + 38.0f * s);
        field_box(box, v.key_focused && v.key != AiConnKey::Saved, v.key_error);
        float text_right = x1 - 4.0f * s;
        if (v.key == AiConnKey::Empty) {
            text_right = ghost(v.paste, y, AiPanelPart::ConnPaste) - 6.0f * s;
        } else if (v.key == AiConnKey::Saved) {
            text_right = ghost(v.change, y, AiPanelPart::ConnChange) - 6.0f * s;
        } else {
            const D2D1_RECT_F eye = D2D1::RectF(x1 - 34.0f * s, y + 4.0f * s, x1 - 4.0f * s, y + 34.0f * s);
            if (draw) {
                const bool h = hot(AiPanelPart::ConnEye);
                if (h) c.Fill(eye, 8.0f * s, pal.hover);
                c.Icon(v.key_revealed ? AiIcon::EyeOff : AiIcon::Eye, eye.left + 6.0f * s, eye.top + 6.0f * s, 18.0f * s,
                       h ? pal.text : pal.text2);
            }
            hit(eye, AiPanelPart::ConnEye);
            text_right = eye.left - 4.0f * s;
        }
        const D2D1_RECT_F text = D2D1::RectF(x0 + 12.0f * s, y + 1.0f * s, text_right, y + 37.0f * s);
        if (draw) {
            if (v.key == AiConnKey::Saved) {
                c.Text(v.key_mask, g.mono, text, pal.text2);
            } else if (!v.key_edit) {
                if (v.key == AiConnKey::Empty) c.Text(v.key_placeholder, formats_.body13, text, pal.text3);
                else c.Text(v.key_mask, v.key_revealed ? formats_.body13 : g.mono, text, v.key_revealed ? pal.text : pal.text2);
            }
            if (v.key != AiConnKey::Saved) {
                g.key_text = text;
                g.key_place = v.key_edit;
            }
        }
        if (v.key != AiConnKey::Saved) hit(text, AiPanelPart::ConnKeyField);
        y += 38.0f * s;
        help(v.key_help);
    }
    // .test: margin-top 16, padding 11 12, radius 10.
    if (v.test != AiConnTest::None) {
        y += 16.0f * s;
        const float text_left = x0 + 12.0f * s + 16.0f * s + 10.0f * s;
        const float text_w = x1 - 12.0f * s - text_left;
        TextOptions o;
        o.wrap = true;
        o.line_height = 18.0f * s;
        const float sub_h = v.test_sub.empty() ? 0.0f : c.MeasureWrapped(v.test_sub, formats_.caption, text_w, o.line_height);
        const D2D1_RECT_F box = D2D1::RectF(x0, y, x1, y + 22.0f * s + 18.0f * s + sub_h);
        if (draw) {
            const bool bad = v.test == AiConnTest::Bad;
            c.Fill(box, 10.0f * s, v.test == AiConnTest::Ok ? pal.accent_tint : bad ? pal.bad_bg : pal.hover);
            if (bad) c.Stroke(box, 10.0f * s, pal.bad_line);
            const float iy = y + 12.0f * s;
            if (v.test == AiConnTest::Running) {
                Spin(c, D2D1::Point2F(x0 + 20.0f * s, iy + 8.0f * s), 16.0f * s, pal.line, pal.accent, now_ms);
                decor_ = true;
            } else {
                c.Icon(bad ? AiIcon::AlertCircle : AiIcon::CheckCircle, x0 + 12.0f * s, iy, 16.0f * s, bad ? pal.bad : pal.ok);
            }
            const D2D1_RECT_F line = D2D1::RectF(text_left, y + 11.0f * s, x1 - 12.0f * s, y + 29.0f * s);
            if (v.test == AiConnTest::Running) {
                c.Text(v.test_title, formats_.small125, line, pal.text2);
            } else {
                const float bw = c.Measure(v.test_title, formats_.small125b);
                c.Text(v.test_title, formats_.small125b, line, pal.text);
                if (!v.test_detail.empty())
                    c.Text(v.test_detail, formats_.small125, D2D1::RectF(line.left + bw, line.top, line.right, line.bottom),
                           pal.text);
            }
            if (sub_h > 0.0f)
                c.Text(v.test_sub, formats_.caption, D2D1::RectF(text_left, y + 29.0f * s, x1 - 12.0f * s, y + 29.0f * s + sub_h),
                       pal.text3, o);
        }
        y = box.bottom;
    }
    // .models: heading + a card of rows (padding 10 14, border-top line2).
    if (!v.models.empty()) {
        y += 12.0f * s;
        if (draw) c.Text(v.models_heading, formats_.heading, D2D1::RectF(x0 + 2.0f * s, y, x1, y + 16.0f * s), pal.text3);
        y += 24.0f * s;
        const float row_h = 40.0f * s;
        const D2D1_RECT_F box = D2D1::RectF(x0, y, x1, y + row_h * static_cast<float>(v.models.size()));
        if (draw) {
            c.Fill(box, 12.0f * s, pal.card);
            c.Stroke(box, 12.0f * s, pal.line);
            for (size_t i = 0; i < v.models.size(); ++i) {
                const float ry = y + row_h * static_cast<float>(i);
                if (i > 0) c.Fill(D2D1::RectF(box.left + 1.0f * s, ry, box.right - 1.0f * s, ry + 1.0f * s), 0.0f, pal.line2);
                const AiConnModelRow& m = v.models[i];
                const float nx = box.left + 14.0f * s;
                const float nw = c.Measure(m.name, formats_.body13b);
                float tag_left = box.right - 14.0f * s;
                if (!m.tag.empty()) {
                    const float tw = c.Measure(m.tag, g.tag) + 14.0f * s;
                    const D2D1_RECT_F tr = D2D1::RectF(tag_left - tw, ry + 11.5f * s, tag_left, ry + 28.5f * s);
                    c.Fill(tr, 6.0f * s, pal.hover);
                    TextOptions o;
                    o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                    c.Text(m.tag, g.tag, tr, pal.text3, o);
                    tag_left = tr.left - 8.0f * s;
                }
                c.Text(m.name, formats_.body13b, D2D1::RectF(nx, ry, tag_left, ry + row_h), pal.text);
                if (!m.desc.empty() && nx + nw + 40.0f * s < tag_left)
                    c.Text(m.desc, formats_.caption, D2D1::RectF(nx + nw + 10.0f * s, ry + 1.0f * s, tag_left, ry + row_h),
                           pal.text3);
            }
        }
        y = box.bottom;
    }
    // .adv: margin-top 12, a 14 px chevron that turns 90° in .2 s.
    {
        y += 12.0f * s;
        if (g.adv_open != v.advanced_open) {
            g.adv_open = v.advanced_open;
            g.adv_at = now_ms;
        }
        const float t = std::clamp(static_cast<float>(now_ms - g.adv_at) / 200.0f, 0.0f, 1.0f);
        if (t < 1.0f) conv_anim_ = true;
        const float turn = (g.adv_open ? EaseDesign(t) : 1.0f - EaseDesign(t)) * 90.0f;
        const float aw = c.Measure(v.advanced, formats_.small125);
        const D2D1_RECT_F r = D2D1::RectF(x0 - 4.0f * s, y - 4.0f * s, x0 + 20.0f * s + aw + 6.0f * s, y + 22.0f * s);
        if (draw) {
            const D2D1_COLOR_F col = hot(AiPanelPart::ConnAdvanced) ? pal.text : pal.text2;
            AiIconPose pose;
            pose.rotate_deg = turn;
            c.Icon(AiIcon::ChevronRight, x0, y + 2.0f * s, 14.0f * s, col, pose);
            c.Text(v.advanced, formats_.small125, D2D1::RectF(x0 + 20.0f * s, y, r.right, y + 18.0f * s), col);
        }
        hit(r, AiPanelPart::ConnAdvanced);
        y += 18.0f * s;
        if (v.advanced_open) {
            y += 10.0f * s;
            label(v.url_label);
            y += 24.0f * s;
            const D2D1_RECT_F box = D2D1::RectF(x0, y, x1, y + 38.0f * s);
            field_box(box, v.url_focused, false);
            const D2D1_RECT_F text = D2D1::RectF(x0 + 12.0f * s, y + 1.0f * s, x1 - 10.0f * s, y + 37.0f * s);
            if (draw) {
                if (!v.url_edit) {
                    if (v.url_text.empty()) c.Text(v.url_placeholder, formats_.body13, text, pal.text3);
                    else c.Text(v.url_text, formats_.body13, text, pal.text);
                }
                g.url_text = text;
                g.url_place = v.url_edit;
            }
            hit(text, AiPanelPart::ConnUrlField);
            y += 38.0f * s;
            help(v.url_help);
        }
    }
    // .safe: margin-top 12, a 15 px shield, gap 8.
    {
        y += 12.0f * s;
        TextOptions o;
        o.wrap = true;
        o.line_height = 18.0f * s;
        const float tx = x0 + 23.0f * s;
        const float h = c.MeasureWrapped(v.safe, formats_.caption, x1 - tx, o.line_height);
        if (draw) {
            c.Icon(AiIcon::Shield, x0, y + 2.0f * s, 15.0f * s, pal.accent);
            c.Text(v.safe, formats_.caption, D2D1::RectF(tx, y, x1, y + h + 4.0f * s), pal.text3, o);
        }
        y += h;
    }
    y += 4.0f * s;
    if (clip_on) {
        // Rows slide under 14 px fades at an edge that has more beyond it,
        // like the menu's list.
        auto fade = [&](float edge, float inner) {
            D2D1_COLOR_F clear = pal.card;
            clear.a = 0.0f;
            const D2D1_GRADIENT_STOP stops[] = {{0.0f, pal.card}, {1.0f, clear}};
            ID2D1GradientStopCollection* col = nullptr;
            if (FAILED(c.dc->CreateGradientStopCollection(stops, 2, &col))) return;
            ID2D1LinearGradientBrush* brush = nullptr;
            if (SUCCEEDED(c.dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(left, edge), D2D1::Point2F(left, inner)), col, &brush))) {
                c.dc->FillRectangle(D2D1::RectF(left, (std::min)(edge, inner), right, (std::max)(edge, inner)), brush);
                brush->Release();
            }
            col->Release();
        };
        if (g.card_scroll > 0.5f) fade(body_clip.top, body_clip.top + 14.0f * s);
        if (g.card_scroll < g.card_scroll_max - 0.5f) fade(body_clip.bottom, body_clip.bottom - 14.0f * s);
        c.dc->PopAxisAlignedClip();
        // A field scrolled (partly) away has no EDIT over it.
        if (g.key_place && !inside(g.key_text)) g.key_place = false;
        if (g.url_place && !inside(g.url_text)) g.url_place = false;
        // A thin thumb at the right edge, like the menu's list.
        const float track = body_clip.bottom - body_clip.top - 8.0f * s;
        const float total = track + g.card_scroll_max;
        const float thumb = (std::max)(24.0f * s, track * track / total);
        const float ty = body_clip.top + 4.0f * s + (track - thumb) * (g.card_scroll / g.card_scroll_max);
        c.Fill(D2D1::RectF(right - 6.0f * s, ty, right - 3.0f * s, ty + thumb), 1.5f * s, pal.line);
        clip_on = false;
        y = body_clip.bottom;
    }
    // .foot: border-top line2, padding 10 14 12, 32 px buttons, gap 8.
    {
        if (draw) c.Fill(D2D1::RectF(left, y, right, y + 1.0f * s), 0.0f, pal.line2);
        y += 1.0f * s;
        const float by = y + 10.0f * s;
        const float bw1 = c.Measure(v.save_button, formats_.body13b) + 32.0f * s;
        const D2D1_RECT_F b1 = D2D1::RectF(x1 - bw1, by, x1, by + 32.0f * s);
        const float bw2 = c.Measure(v.test_button, formats_.body13) + 28.0f * s;
        const D2D1_RECT_F b2 = D2D1::RectF(b1.left - 8.0f * s - bw2, by, b1.left - 8.0f * s, by + 32.0f * s);
        if (draw) {
            TextOptions o;
            o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
            // A pressed button shrinks a little, like the conversation's.
            D2D1_MATRIX_3X2_F before;
            c.dc->GetTransform(&before);
            auto press = [&](const D2D1_RECT_F& r, AiPanelPart part) {
                if (!animate_ || vm.pressed.part != part || !hot(part)) return;
                const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
                c.dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
            };
            // .b2 (near-white like the conversation's), at half opacity while it cannot run.
            const float a2 = v.can_test ? 1.0f : 0.5f;
            const bool h2 = v.can_test && hot(AiPanelPart::ConnTest);
            if (v.can_test) press(b2, AiPanelPart::ConnTest);
            c.Fill(b2, 8.0f * s, Alpha(h2 ? pal.hover : Mix(pal.card, pal.pane, 0.3f), a2));
            c.Stroke(b2, 8.0f * s, Alpha(pal.line, a2));
            c.Text(v.test_button, formats_.body13, b2, Alpha(pal.text, a2), o);
            c.dc->SetTransform(before);
            // .b1; disabled: line / text3.
            const bool h1 = v.can_save && hot(AiPanelPart::ConnSave);
            if (v.can_save) press(b1, AiPanelPart::ConnSave);
            const D2D1_COLOR_F f1 = !v.can_save ? pal.line : h1 ? Mix(pal.accent, pal.text, 0.12f) : pal.accent;
            c.Fill(b1, 8.0f * s, f1);
            c.Text(v.save_button, formats_.body13b, b1, v.can_save ? pal.on_accent : pal.text3, o);
            c.dc->SetTransform(before);
            if (!v.remove.empty()) {
                const float rw = c.Measure(v.remove, formats_.small125) + 8.0f * s;
                const D2D1_RECT_F r = D2D1::RectF(x0 - 4.0f * s, by, x0 - 4.0f * s + rw, by + 32.0f * s);
                const D2D1_COLOR_F rc = hot(AiPanelPart::ConnRemove) ? Mix(pal.bad, pal.text, 0.25f) : pal.bad;
                c.Text(v.remove, formats_.small125, r, rc, o);
                hit(r, AiPanelPart::ConnRemove);
            }
        }
        if (v.can_test) hit(b2, AiPanelPart::ConnTest);
        if (v.can_save) hit(b1, AiPanelPart::ConnSave);
        y = by + 32.0f * s + 12.0f * s;
    }
    return y - top;
}

} // namespace pulse::ui
